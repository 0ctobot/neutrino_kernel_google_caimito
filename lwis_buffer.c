/*
 * Google LWIS DMA Buffer Utilities
 *
 * Copyright (c) 2018 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME "-buffer: " fmt

#include <linux/slab.h>
// TODO(yromanenko): Refactor chipset specific to a file under platform/
#include <linux/ion_exynos.h>

#include "lwis_device.h"
#include "lwis_buffer.h"

int lwis_buffer_enroll(struct lwis_client *lwis_client,
		       struct lwis_buffer *buffer)
{
	struct lwis_buffer *old_buffer;
	BUG_ON(!lwis_client);
	BUG_ON(!buffer);

	buffer->dma_buf = dma_buf_get(buffer->info.fd);
	if (IS_ERR_OR_NULL(buffer->dma_buf)) {
		pr_err("Could not get dma buffer for fd: %d", buffer->info.fd);
		return -EINVAL;
	}

	buffer->dma_buf_attachment = dma_buf_attach(
		buffer->dma_buf, &lwis_client->lwis_dev->plat_dev->dev);

	if (IS_ERR_OR_NULL(buffer->dma_buf_attachment)) {
		pr_err("Could not attach dma buffer for fd: %d",
		       buffer->info.fd);
		dma_buf_put(buffer->dma_buf);
		return -EINVAL;
	}

	if (buffer->info.dma_read && buffer->info.dma_write) {
		buffer->dma_direction = DMA_BIDIRECTIONAL;
	} else if (buffer->info.dma_read) {
		buffer->dma_direction = DMA_TO_DEVICE;
	} else if (buffer->info.dma_write) {
		buffer->dma_direction = DMA_FROM_DEVICE;
	} else {
		buffer->dma_direction = DMA_NONE;
	}

	buffer->sg_table = dma_buf_map_attachment(buffer->dma_buf_attachment,
						  buffer->dma_direction);
	if (IS_ERR_OR_NULL(buffer->sg_table)) {
		pr_err("Could not map dma attachment for fd: %d",
		       buffer->info.fd);
		dma_buf_detach(buffer->dma_buf, buffer->dma_buf_attachment);
		dma_buf_put(buffer->dma_buf);
		return -EINVAL;
	}

	buffer->info.dma_vaddr = ion_iovmm_map(buffer->dma_buf_attachment, 0, 0,
					       buffer->dma_direction, 0);

	if (!buffer->info.dma_vaddr) {
		pr_err("Could not map into IO VMM for fd: %d", buffer->info.fd);
		dma_buf_unmap_attachment(buffer->dma_buf_attachment,
					 buffer->sg_table,
					 buffer->dma_direction);
		dma_buf_detach(buffer->dma_buf, buffer->dma_buf_attachment);
		dma_buf_put(buffer->dma_buf);
		return -EINVAL;
	}

	old_buffer = lwis_client_enrolled_buffer_find(lwis_client,
						      buffer->info.dma_vaddr);

	if (old_buffer) {
		pr_err("Duplicate vaddr %llx for fd %d", buffer->info.dma_vaddr,
		       buffer->info.fd);
		ion_iovmm_unmap(buffer->dma_buf_attachment,
				buffer->info.dma_vaddr);
		dma_buf_unmap_attachment(buffer->dma_buf_attachment,
					 buffer->sg_table,
					 buffer->dma_direction);
		dma_buf_detach(buffer->dma_buf, buffer->dma_buf_attachment);
		dma_buf_put(buffer->dma_buf);
		return -EINVAL;
	}

	hash_add(lwis_client->enrolled_buffers, &buffer->node,
		 buffer->info.dma_vaddr);

	return 0;
}

int lwis_buffer_disenroll(struct lwis_buffer *buffer)
{
	BUG_ON(!buffer);

	ion_iovmm_unmap(buffer->dma_buf_attachment, buffer->info.dma_vaddr);
	dma_buf_unmap_attachment(buffer->dma_buf_attachment, buffer->sg_table,
				 buffer->dma_direction);
	dma_buf_detach(buffer->dma_buf, buffer->dma_buf_attachment);
	dma_buf_put(buffer->dma_buf);
	/* Delete the node from the hash table */
	hash_del(&buffer->node);
	return 0;
}

struct lwis_buffer *
lwis_client_enrolled_buffer_find(struct lwis_client *lwis_client,
				 dma_addr_t dma_vaddr)
{
	struct lwis_buffer *p;
	BUG_ON(!lwis_client);

	hash_for_each_possible(lwis_client->enrolled_buffers, p, node,
			       dma_vaddr)
	{
		if (p->info.dma_vaddr == dma_vaddr) {
			return p;
		}
	}

	return NULL;
}

int lwis_client_enrolled_buffers_clear(struct lwis_client *lwis_client)
{
	/* Our hash table iterator */
	struct lwis_buffer *buffer;
	/* Temporary vars for hash table traversal */
	struct hlist_node *n;
	int i;

	BUG_ON(!lwis_client);
	/* Iterate over the entire hash table */
	hash_for_each_safe(lwis_client->enrolled_buffers, i, n, buffer, node)
	{
		/* Disenroll the buffer */
		lwis_buffer_disenroll(buffer);
		/* Free the object */
		kfree(buffer);
	}

	return 0;
}

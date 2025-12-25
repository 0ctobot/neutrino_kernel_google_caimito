// SPDX-License-Identifier: GPL-2.0-only
/*
 * Edge TPU ML accelerator telemetry: logging and tracing.
 *
 * Copyright (C) 2019-2025 Google LLC
 */

#include <linux/minmax.h>
#include <linux/mm_types.h>
#include <linux/types.h>

#include <gcip/gcip-memory.h>
#include <gcip/gcip-telemetry.h>

#include "edgetpu-internal.h"
#include "edgetpu-iremap-pool.h"
#include "edgetpu-mmu.h"
#include "edgetpu-telemetry.h"
#include "edgetpu.h"

static void set_telemetry_mem(struct edgetpu_dev *etdev)
{
	struct gcip_telemetry *tel_log = etdev->telemetry_log;
	struct gcip_telemetry *tel_trace = etdev->telemetry_trace;
	int i, offset = 0;

	for (i = 0; i < etdev->num_telemetry_buffers; i++) {
		tel_log[i].memory.virt_addr = edgetpu_firmware_shared_data_vaddr(etdev) + offset;
		tel_log[i].memory.dma_addr = edgetpu_firmware_shared_data_daddr(etdev) + offset;
		tel_log[i].memory.host_addr = 0;
		tel_log[i].memory.phys_addr = edgetpu_firmware_shared_data_paddr(etdev) + offset;
		tel_log[i].memory.size = etdev->log_buffer_size;
		offset += etdev->log_buffer_size;
		tel_trace[i].memory.virt_addr = edgetpu_firmware_shared_data_vaddr(etdev) + offset;
		tel_trace[i].memory.dma_addr = edgetpu_firmware_shared_data_daddr(etdev) + offset;
		tel_trace[i].memory.host_addr = 0;
		tel_trace[i].memory.phys_addr = edgetpu_firmware_shared_data_paddr(etdev) + offset;
		tel_trace[i].memory.size = etdev->trace_buffer_size;
		offset += etdev->trace_buffer_size;
	}
}

int edgetpu_telemetry_init(struct edgetpu_dev *etdev)
{
	int ret, i;
	size_t sz;

	sz = sizeof(*etdev->telemetry_trace) * etdev->num_telemetry_buffers;
	etdev->telemetry_log = devm_krealloc(etdev->dev, etdev->telemetry_log, sz, GFP_KERNEL);
	if (!etdev->telemetry_log)
		return -ENOMEM;

	sz = sizeof(*etdev->telemetry_trace) * etdev->num_telemetry_buffers;
	etdev->telemetry_trace = devm_krealloc(etdev->dev, etdev->telemetry_trace, sz, GFP_KERNEL);
	if (!etdev->telemetry_trace)
		return -ENOMEM;

	set_telemetry_mem(etdev);

	for (i = 0; i < etdev->num_telemetry_buffers; i++) {
		ret = gcip_telemetry_init(&etdev->telemetry_log[i], GCIP_TELEMETRY_TYPE_LOG,
					  etdev->dev);
		if (ret)
			break;

		ret = gcip_telemetry_init(&etdev->telemetry_trace[i], GCIP_TELEMETRY_TYPE_TRACE,
					  etdev->dev);
		if (ret) {
			gcip_telemetry_exit(&etdev->telemetry_log[i]);
			break;
		}
	}

	if (ret)
		while (i--) {
			gcip_telemetry_exit(&etdev->telemetry_trace[i]);
			gcip_telemetry_exit(&etdev->telemetry_log[i]);
		}

	return ret;
}

void edgetpu_telemetry_exit(struct edgetpu_dev *etdev)
{
	int i;

	for (i = 0; i < etdev->num_telemetry_buffers; i++) {
		gcip_telemetry_exit(&etdev->telemetry_trace[i]);
		gcip_telemetry_exit(&etdev->telemetry_log[i]);
	}
}

int edgetpu_telemetry_kci(struct edgetpu_dev *etdev)
{
	int ret;

	/* Core 0 will notify other cores. */
	ret = gcip_telemetry_kci(&etdev->telemetry_log[0], edgetpu_kci_map_log_buffer,
				 etdev->etkci->kci);
	if (ret)
		return ret;

	ret = gcip_telemetry_kci(&etdev->telemetry_trace[0], edgetpu_kci_map_trace_buffer,
				 etdev->etkci->kci);
	if (ret)
		return ret;

	return 0;
}

int edgetpu_telemetry_set_event(struct edgetpu_dev *etdev, struct gcip_telemetry *tel, u32 eventfd)
{
	int ret;
	int i;

	for (i = 0; i < etdev->num_telemetry_buffers; i++) {
		ret = gcip_telemetry_set_event(&tel[i], eventfd);
		if (ret) {
			edgetpu_telemetry_unset_event(etdev, tel);
			return ret;
		}
	}

	return 0;
}

void edgetpu_telemetry_unset_event(struct edgetpu_dev *etdev, struct gcip_telemetry *tel)
{
	int i;

	for (i = 0; i < etdev->num_telemetry_buffers; i++)
		gcip_telemetry_unset_event(&tel[i]);
}

void edgetpu_telemetry_irq_handler(struct edgetpu_dev *etdev)
{
	int i;

	for (i = 0; i < etdev->num_telemetry_buffers; i++) {
		gcip_telemetry_irq_handler(&etdev->telemetry_log[i]);
		gcip_telemetry_irq_handler(&etdev->telemetry_trace[i]);
	}
}

static void telemetry_mappings_show(struct gcip_telemetry *tel, struct gcip_memory *mem,
				    struct seq_file *s)
{
	seq_printf(s, "  %pad %lu %s %#llx\n", &mem->dma_addr, DIV_ROUND_UP(mem->size, PAGE_SIZE),
		   tel->name, mem->host_addr);
}

void edgetpu_telemetry_mappings_show(struct edgetpu_dev *etdev, struct seq_file *s)
{
	int i;

	for (i = 0; i < etdev->num_telemetry_buffers; i++) {
		telemetry_mappings_show(&etdev->telemetry_log[i], &etdev->telemetry_log[i].memory,
					s);
		telemetry_mappings_show(&etdev->telemetry_trace[i],
					&etdev->telemetry_trace[i].memory, s);
	}
}

int edgetpu_mmap_telemetry_buffer(struct edgetpu_dev *etdev, struct gcip_telemetry *tel,
				  struct vm_area_struct *vma, int core_id)
{
	int ret;

	if (core_id >= etdev->num_telemetry_buffers)
		return -EINVAL;

	ret = gcip_telemetry_mmap(&tel[core_id], vma);
	if (ret) {
		etdev_err(etdev, "Failed to mmap telemetry buffer: type=%d, ret=%d",
			  tel[core_id].type, ret);
		return ret;
	}

	return 0;
}

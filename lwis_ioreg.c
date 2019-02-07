/*
 * Google LWIS Register I/O Interface
 *
 * Copyright (c) 2018 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME "-ioreg: " fmt

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "lwis_ioreg.h"

int lwis_ioreg_list_alloc(struct lwis_ioreg_device *ioreg_dev, int num_blocks)
{
	struct lwis_ioreg_list *list;

	/* No need to allocate if num_blocks is invalid */
	if (!ioreg_dev || num_blocks <= 0) {
		return -EINVAL;
	}

	list = &ioreg_dev->reg_list;
	list->block =
		kzalloc(num_blocks * sizeof(struct lwis_ioreg), GFP_KERNEL);
	if (!list->block) {
		return -ENOMEM;
	}

	list->count = num_blocks;

	return 0;
}

void lwis_ioreg_list_free(struct lwis_ioreg_device *ioreg_dev)
{
	struct lwis_ioreg_list *list;

	if (!ioreg_dev) {
		return;
	}

	list = &ioreg_dev->reg_list;
	if (list->block) {
		kfree(list->block);
		list->block = NULL;
		list->count = 0;
	}
}

int lwis_ioreg_get(struct lwis_ioreg_device *ioreg_dev, int index, char *name,
		   int default_access_size)
{
	struct resource *res;
	struct lwis_ioreg *block;
	struct lwis_ioreg_list *list;
	struct platform_device *plat_dev;

	if (!ioreg_dev) {
		return -EINVAL;
	}

	plat_dev = ioreg_dev->base_dev.plat_dev;
	list = &ioreg_dev->reg_list;
	if (index < 0 || index >= list->count) {
		return -EINVAL;
	}

	res = platform_get_resource(plat_dev, IORESOURCE_MEM, index);
	if (!res) {
		pr_err("platform_get_resource error\n");
		return -EINVAL;
	}

	block = &list->block[index];
	block->name = name;
	block->start = res->start;
	block->size = resource_size(res);
	block->default_access_size = default_access_size;
	block->base = devm_ioremap_nocache(&plat_dev->dev, res->start,
					   resource_size(res));
	if (!block->base) {
		pr_err("Cannot map I/O register space\n");
		return -EINVAL;
	}

	return 0;
}

int lwis_ioreg_put_by_idx(struct lwis_ioreg_device *ioreg_dev, int index)
{
	struct lwis_ioreg_list *list;
	struct device *dev;

	if (!ioreg_dev) {
		return -EINVAL;
	}

	dev = &ioreg_dev->base_dev.plat_dev->dev;
	list = &ioreg_dev->reg_list;
	if (index < 0 || index >= list->count) {
		return -EINVAL;
	}

	if (!list->block[index].base) {
		return -EINVAL;
	}

	devm_iounmap(dev, list->block[index].base);

	return 0;
}

int lwis_ioreg_put_by_name(struct lwis_ioreg_device *ioreg_dev, char *name)
{
	int i;
	struct lwis_ioreg_list *list;
	struct device *dev;

	if (!ioreg_dev) {
		return -EINVAL;
	}

	dev = &ioreg_dev->base_dev.plat_dev->dev;
	list = &ioreg_dev->reg_list;

	for (i = 0; i < list->count; ++i) {
		if (!strcmp(list->block[i].name, name)) {
			if (list->block[i].base) {
				devm_iounmap(dev, list->block[i].base);
				return 0;
			}
			return -EINVAL;
		}
	}

	return -ENOENT;
}

static int ioreg_read_internal(unsigned int __iomem *base, uint64_t offset,
			       int value_bits, uint64_t *value)
{
	switch (value_bits) {
	case 8:
		*value = readb((void *)base + offset);
		break;
	case 16:
		*value = readw((void *)base + offset);
		break;
	case 32:
		*value = readl((void *)base + offset);
		break;
	case 64:
		*value = readq((void *)base + offset);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int ioreg_write_internal(unsigned int __iomem *base, uint64_t offset,
				int value_bits, uint64_t value)
{
	switch (value_bits) {
	case 8:
		writeb((uint8_t)value, (void *)base + offset);
		break;
	case 16:
		writew((uint16_t)value, (void *)base + offset);
		break;
	case 32:
		writel((uint32_t)value, (void *)base + offset);
		break;
	case 64:
		writeq(value, (void *)base + offset);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int lwis_ioreg_read_batch(struct lwis_ioreg_device *ioreg_dev,
			  struct lwis_io_msg *msg)
{
	int i;
	struct lwis_io_data *data = msg->buf;
	struct lwis_ioreg *block;
	struct lwis_ioreg_list *list;
	int access_size;

	const int index = msg->bid;
	const int num_entries = msg->num_entries;

	if (!ioreg_dev) {
		return -EINVAL;
	}

	list = &ioreg_dev->reg_list;
	if (index < 0 || index >= list->count) {
		return -EINVAL;
	}

	block = &list->block[index];
	if (!block->base) {
		return -EINVAL;
	}

	if (!data || num_entries <= 0) {
		return -EINVAL;
	}

	access_size = data[i].access_size ? data[i].access_size
					  : block->default_access_size;

	for (i = 0; i < num_entries; ++i) {
		int ret = ioreg_read_internal(block->base, data[i].offset,
					      access_size, &data[i].val);
		if (ret) {
			pr_err("Invalid ioreg read\n");
			return ret;
		}
	}

	return 0;
}

int lwis_ioreg_write_batch(struct lwis_ioreg_device *ioreg_dev,
			   struct lwis_io_msg *msg)
{
	int i;
	struct lwis_io_data *data = msg->buf;
	struct lwis_ioreg *block;
	struct lwis_ioreg_list *list;
	int access_size;

	const int index = msg->bid;
	const int num_entries = msg->num_entries;

	if (!ioreg_dev) {
		return -EINVAL;
	}

	list = &ioreg_dev->reg_list;
	if (index < 0 || index >= list->count) {
		return -EINVAL;
	}

	block = &list->block[index];
	if (!block->base) {
		return -EINVAL;
	}

	if (!data || num_entries <= 0) {
		return -EINVAL;
	}

	access_size = data[i].access_size ? data[i].access_size
					  : block->default_access_size;

	for (i = 0; i < num_entries; ++i) {
		int ret = ioreg_write_internal(block->base, data[i].offset,
					       access_size, data[i].val);
		if (ret) {
			pr_err("Invalid ioreg write\n");
			return ret;
		}
	}

	return 0;
}

int lwis_ioreg_read_by_block_idx(struct lwis_ioreg_device *ioreg_dev, int index,
				 uint64_t offset, int value_bits,
				 uint64_t *value)
{
	struct lwis_ioreg *block;
	struct lwis_ioreg_list *list;

	if (!ioreg_dev) {
		return -EINVAL;
	}

	list = &ioreg_dev->reg_list;
	if (index < 0 || index >= list->count) {
		return -EINVAL;
	}

	block = &list->block[index];
	if (!block->base) {
		return -EINVAL;
	}

	value_bits = value_bits ? value_bits : block->default_access_size;

	return ioreg_read_internal(block->base, offset, value_bits, value);
}

int lwis_ioreg_read_by_block_name(struct lwis_ioreg_device *ioreg_dev,
				  char *name, uint64_t offset, int value_bits,
				  uint64_t *value)
{
	int i;
	struct lwis_ioreg_list *list;

	if (!ioreg_dev) {
		return -EINVAL;
	}

	list = &ioreg_dev->reg_list;
	for (i = 0; i < list->count; ++i) {
		if (!strcmp(list->block[i].name, name)) {
			if (list->block[i].base) {
				value_bits =
					value_bits
						? value_bits
						: list->block[i]
							  .default_access_size;
				return ioreg_read_internal(list->block[i].base,
							   offset, value_bits,
							   value);
			}
			return -EINVAL;
		}
	}

	return -ENOENT;
}

int lwis_ioreg_write_by_block_idx(struct lwis_ioreg_device *ioreg_dev,
				  int index, uint64_t offset, int value_bits,
				  uint64_t value)
{
	struct lwis_ioreg *block;
	struct lwis_ioreg_list *list;

	if (!ioreg_dev) {
		return -EINVAL;
	}

	list = &ioreg_dev->reg_list;
	if (index < 0 || index >= list->count) {
		return -EINVAL;
	}

	block = &list->block[index];
	if (!block->base) {
		return -EINVAL;
	}

	value_bits = value_bits ? value_bits : block->default_access_size;

	return ioreg_write_internal(block->base, offset, value_bits, value);
}

int lwis_ioreg_write_by_block_name(struct lwis_ioreg_device *ioreg_dev,
				   char *name, uint64_t offset, int value_bits,
				   uint64_t value)
{
	int i;
	struct lwis_ioreg_list *list;

	if (!ioreg_dev) {
		return -EINVAL;
	}

	list = &ioreg_dev->reg_list;
	for (i = 0; i < list->count; ++i) {
		if (!strcmp(list->block[i].name, name)) {
			if (list->block[i].base) {
				value_bits =
					value_bits
						? value_bits
						: list->block[i]
							  .default_access_size;
				return ioreg_write_internal(list->block[i].base,
							    offset, value_bits,
							    value);
			}
			return -EINVAL;
		}
	}

	return -ENOENT;
}

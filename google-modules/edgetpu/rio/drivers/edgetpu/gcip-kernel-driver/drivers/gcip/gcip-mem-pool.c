// SPDX-License-Identifier: GPL-2.0-only
/*
 * A simple memory allocator to help allocating reserved memory pools.
 *
 * Copyright (C) 2022 Google LLC
 */

#include <linux/device.h>
#include <linux/genalloc.h>
#include <linux/log2.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <gcip/gcip-mem-pool.h>

/**
 * struct gcip_mem_pool_region - Internal structure to manage regions in a red-black tree.
 * @pool: The pool where the region belongs to.
 * @addr: The start address of the region.
 * @size: The size of the region.
 * @node: The node to manage the region in the red-black tree of @pool.
 */
struct gcip_mem_pool_region {
	struct gcip_mem_pool *pool;
	unsigned long addr;
	size_t size;
	struct rb_node node;
};

static void gcip_mem_pool_add_region_locked(struct gcip_mem_pool *pool,
					    struct gcip_mem_pool_region *region)
{
	struct rb_node **link = &pool->regions.rb_node, *parent_node = NULL;
	struct gcip_mem_pool_region *parent;

	lockdep_assert_held(&pool->regions_lock);

	while (*link) {
		parent_node = *link;
		parent = rb_entry(parent_node, struct gcip_mem_pool_region, node);

		WARN_ON(parent->addr == region->addr);

		if (parent->addr > region->addr)
			link = &(*link)->rb_left;
		else if (parent->addr < region->addr)
			link = &(*link)->rb_right;
	}

	rb_link_node(&region->node, parent_node, link);
	rb_insert_color(&region->node, &pool->regions);
}

static void gcip_mem_pool_remove_region_locked(struct gcip_mem_pool *pool,
					       struct gcip_mem_pool_region *region)
{
	lockdep_assert_held(&pool->regions_lock);

	rb_erase(&region->node, &pool->regions);
}

static struct gcip_mem_pool_region *gcip_mem_pool_find_region_locked(struct gcip_mem_pool *pool,
								     unsigned long addr,
								     size_t size, size_t *offset)
{
	struct gcip_mem_pool_region *region, *found = NULL;
	struct rb_node *node;
	unsigned long start, end;

	lockdep_assert_held(&pool->regions_lock);

	node = pool->regions.rb_node;

	while (node) {
		region = rb_entry(node, struct gcip_mem_pool_region, node);
		start = region->addr;
		end = start + region->size;

		if (start <= addr && addr < end) {
			if (end - addr >= size) {
				if (offset)
					*offset = addr - start;
				found = region;
			}
			break;
		} else if (addr < start) {
			node = node->rb_left;
		} else {
			node = node->rb_right;
		}
	}

	return found;
}

int gcip_mem_pool_init(struct gcip_mem_pool *pool, struct device *dev, unsigned long base_addr,
		       size_t size, size_t granule)
{
	int ret;

	if (!base_addr || granule == 0)
		return -EINVAL;
	if (base_addr % granule || size % granule)
		return -EINVAL;
	pool->gen_pool = gen_pool_create(ilog2(granule), -1);
	if (!pool->gen_pool) {
		dev_err(dev, "gcip memory pool allocate gen_pool failed");
		return -ENOMEM;
	}
	ret = gen_pool_add(pool->gen_pool, base_addr, size, -1);
	if (ret) {
		gen_pool_destroy(pool->gen_pool);
		pool->gen_pool = NULL;
		dev_err(dev, "gcip failed to add memory to mem pool: %d", ret);
		return ret;
	}
	pool->dev = dev;
	pool->granule = granule;
	pool->base_addr = base_addr;
	pool->regions = RB_ROOT;
	spin_lock_init(&pool->regions_lock);

	return 0;
}

void gcip_mem_pool_exit(struct gcip_mem_pool *pool)
{
	if (!pool->gen_pool)
		return;
	WARN_ON(!RB_EMPTY_ROOT(&pool->regions));
	gen_pool_destroy(pool->gen_pool);
	pool->gen_pool = NULL;
}

unsigned long gcip_mem_pool_alloc(struct gcip_mem_pool *pool, size_t size)
{
	struct gcip_mem_pool_region *region;
	unsigned long addr, flags;

	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return 0;

	addr = gen_pool_alloc(pool->gen_pool, size);
	if (!addr) {
		kfree(region);
		return 0;
	}

	region->pool = pool;
	region->addr = addr;
	region->size = size;

	spin_lock_irqsave(&pool->regions_lock, flags);
	gcip_mem_pool_add_region_locked(pool, region);
	spin_unlock_irqrestore(&pool->regions_lock, flags);

	dev_dbg(pool->dev, "%s @ size = %#zx addr=%#lx", __func__, size, addr);

	return region->addr;
}

void gcip_mem_pool_free(struct gcip_mem_pool *pool, unsigned long addr, size_t size)
{
	struct gcip_mem_pool_region *region;
	unsigned long flags;

	spin_lock_irqsave(&pool->regions_lock, flags);

	region = gcip_mem_pool_find_region_locked(pool, addr, size, NULL);
	if (!region || region->addr != addr) {
		spin_unlock_irqrestore(&pool->regions_lock, flags);
		return;
	}

	dev_dbg(pool->dev, "%s @ size = %#zx addr=%#lx", __func__, region->size, region->addr);
	gcip_mem_pool_remove_region_locked(pool, region);

	spin_unlock_irqrestore(&pool->regions_lock, flags);

	size = ALIGN(region->size, pool->granule);
	gen_pool_free(pool->gen_pool, region->addr, size);
	kfree(region);
}

unsigned long gcip_mem_pool_find(struct gcip_mem_pool *pool, unsigned long addr, size_t size,
				 size_t *offset)
{
	struct gcip_mem_pool_region *region;
	unsigned long flags;

	spin_lock_irqsave(&pool->regions_lock, flags);
	region = gcip_mem_pool_find_region_locked(pool, addr, size, offset);
	spin_unlock_irqrestore(&pool->regions_lock, flags);

	return region ? region->addr : 0;
}

unsigned long gcip_mem_pool_find_or_alloc(struct gcip_mem_pool *pool, unsigned long addr,
					  size_t size, size_t *offset, bool *allocated)
{
	addr = gcip_mem_pool_find(pool, addr, size, offset);
	if (addr) {
		*allocated = false;
		return addr;
	}

	addr = gcip_mem_pool_alloc(pool, size);
	if (!addr)
		return 0;

	*offset = 0;
	*allocated = true;

	return addr;
}

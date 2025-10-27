/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A simple memory allocator to help allocating reserved memory pools.
 *
 * Copyright (C) 2022 Google LLC
 */

#ifndef __GCIP_MEM_POOL_H__
#define __GCIP_MEM_POOL_H__

#include <linux/device.h>
#include <linux/genalloc.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct gcip_mem_pool {
	struct device *dev;
	struct gen_pool *gen_pool;
	unsigned long base_addr;
	size_t granule;
	struct rb_root regions;
	spinlock_t regions_lock;
};

/*
 * Initializes the memory pool object.
 *
 * @pool: The memory pool object to be initialized.
 * @dev: Used for logging only.
 * @base_addr: The base address of the pool. Must be greater than 0 and a multiple of @granule.
 * @size: The size of the pool. @size should be a multiple of @granule.
 * @granule: The granule when invoking the allocator. Should be a power of 2.
 *
 * Returns 0 on success, a negative errno otherwise.
 *
 * Call gcip_mem_pool_exit() to release the resources of @pool.
 */
int gcip_mem_pool_init(struct gcip_mem_pool *pool, struct device *dev, unsigned long base_addr,
		       size_t size, size_t granule);
/*
 * Releases resources of @pool.
 *
 * Note: you must release (by calling gcip_mem_pool_free) all allocations before calling this
 * function.
 */
void gcip_mem_pool_exit(struct gcip_mem_pool *pool);

/**
 * gcip_mem_pool_alloc() - Allocates and returns the allocated address.
 * @size: Size to be allocated.
 *
 * Returns the allocated address. Returns 0 on allocation failure.
 */
unsigned long gcip_mem_pool_alloc(struct gcip_mem_pool *pool, size_t size);

/**
 * gcip_mem_pool_free() - Frees the address previously allocated by gcip_mem_pool_alloc().
 * @addr: The address of allocated region which was returned from `gcip_mem_pool_alloc()`.
 * @size: The size of the region passed to `gcip_mem_pool_alloc()`.
 *
 * The address must match with the one previously returned by gcip_mem_pool_alloc().
 */
void gcip_mem_pool_free(struct gcip_mem_pool *pool, unsigned long addr, size_t size);

/*
 * Returns the offset between @addr and @base_addr passed to gcip_mem_pool_init().
 *
 * @addr must be a value returned by gcip_mem_pool_alloc().
 */
static inline size_t gcip_mem_pool_offset(struct gcip_mem_pool *pool, unsigned long addr)
{
	return addr - pool->base_addr;
}

/**
 * gcip_mem_pool_find() - Finds a region where the buffer starting from @addr with @size can fit in.
 * @pool: The memory pool which manages allocated regions.
 * @addr: The address of the buffer.
 * @size: The size of the buffer.
 * @offset: The offset of the buffer from the start address of the found region.
 *
 * Returns the start address of the found region. Otherwise, returns 0.
 */
unsigned long gcip_mem_pool_find(struct gcip_mem_pool *pool, unsigned long addr, size_t size,
				 size_t *offset);

/**
 * gcip_mem_pool_find_or_alloc() - Finds a region where the buffer starting from @addr with @size
 *                                 can fit in. If there is no available region, it will allocate a
 *                                 @size region from the pool.
 * @pool: The memory pool which manages allocated regions.
 * @addr: The address of the buffer.
 * @size: The size of the buffer.
 * @offset: The offset of the buffer from the start address of the returned region.
 * @allocated: Will be set to true if there was no region where the passed buffer can fit in and a
 *             new region with @size has been allocated from @pool.
 *
 * Returns the start address of the found or newly allocated region. Otherwise, returns 0.
 */
unsigned long gcip_mem_pool_find_or_alloc(struct gcip_mem_pool *pool, unsigned long addr,
					  size_t size, size_t *offset, bool *allocated);

#endif /* __GCIP_MEM_POOL_H__ */

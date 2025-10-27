/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Bridges DMA or IIF fence (base fence) with IIF or DMA fence (bridged fence) accoridingly. The
 * driver will allocates a bridged fence which will be signaled once the bound base fecne is
 * signaled. The fence status of the base fence will be propagated to the bridged fence.
 *
 * One of the main use cases of binding a DMA fence with an IIF is to hold a command which is
 * blocked by DMA fences in the firmware side, not the kernel driver side. The IP driver is expected
 * to create an IIF using this interface if there are DMA fences blocking the command and add the
 * created IIF to in-fences of the command and send the command to the firmware directly.
 *
 * Copyright (C) 2024-2025 Google LLC
 */

#ifndef __IIF_IIF_DMA_FENCE_H__
#define __IIF_IIF_DMA_FENCE_H__

#include <linux/dma-fence.h>
#include <linux/kref.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

#include <iif/iif-fence.h>
#include <iif/iif-manager.h>

#define iif_to_iif_dma_fence(iif) container_of(iif, struct iif_dma_fence, bridged_fence.iif_fence)
#define dma_to_iif_dma_fence(dma) container_of(dma, struct iif_dma_fence, bridged_fence.dma_fence)

/* Wrapper structure to bridge inter-IP fence and DMA fence. */
struct iif_dma_fence {
	/*
	 * The bridged fence which will be signaled by the driver once @base_fence is signaled.
	 * In case of @iif_fence, it is an AP-signaled fence as the driver will signal it.
	 */
	union {
		struct iif_fence iif_fence;
		struct dma_fence dma_fence;
	} bridged_fence;
	/* The DMA or IIF fence which will signal the bridged fence. */
	void *base_fence;
	/* The thread which will wait on @base_fence to be signaled and signal @bridged_fence. */
	struct task_struct *task;
	/* The timeout of waiting on @base_fence. */
	signed long timeout_jiffies;
	/* The lock which will be used when @bridged_fence is DMA fence. */
	spinlock_t dma_fence_lock;
	/* The callback which will be invoked when @base_fence is signaled. */
	union {
		struct iif_fence_poll_cb iif_cb;
		struct dma_fence_cb dma_cb;
	} poll_cb;
	/* The work signals @bridged_fence when @base_fence is signaled. */
	struct work_struct signal_work;
	/* The work which will be scheduled when @bridged_fence can be released. */
	struct work_struct release_work;
	/* The work which stops the @task thread asynchronously. */
	struct work_struct stop_work;
	/* Refcount. */
	struct kref kref;
};

/**
 * iif_dma_fence_get() - Acquires a refcount.
 * @iif_dma_fence: The IIF-DMA or DMA-IIF bridge to acquire a refcount.
 *
 * Returns @iif_dma_fence.
 */
struct iif_dma_fence *iif_dma_fence_get(struct iif_dma_fence *iif_dma_fence);

/**
 * iif_dma_fence_put() - Releases a refocunt.
 * @iif_dma_fence: The IIF-DMA or DMA-IIF bridge to release a refcount.
 */
void iif_dma_fence_put(struct iif_dma_fence *iif_dma_fence);

/**
 * iif_dma_fence_wait_timeout() - Creates an inter-IP fence which will be signaled once @dma_fence
 *                                is signaled.
 * @mgr: IIF manager to create an inter-IP fence.
 * @dma_fence: The DMA fence to wait on.
 * @timeout_jiffies: The timeout in jiffies, or MAX_SCHEDULE_TIMEOUT to wait until @dma_fence gets
 *                   signaled.
 *
 * Internally, it will create a thread waiting on @dma_fence to be signaled and the thread will
 * signal the IIF once @dma_fence is signaled. As the subject who signal the fence is the IIF
 * driver, the signaler IP type of the returned IIF is AP.
 *
 * Note that the caller should release the refcount of the returned IIF (i.e., iif_fence_put())
 * if they don't need to access it anymore.
 *
 * This function cannot be called in the IRQ context.
 *
 * Returns an inter-IP fence on success. Otherwise, returns an error.
 */
struct iif_fence *iif_dma_fence_wait_timeout(struct iif_manager *mgr, struct dma_fence *dma_fence,
					     signed long timeout_jiffies);

/**
 * iif_dma_fence_wait() - The same with `iif_dma_fence_wait()`, but without timeout.
 * @mgr: IIF manager to create an inter-IP fence.
 * @dma_fence: The DMA fence to wait on.
 *
 * See `iif_dma_fence_wait_timeout()` for details.
 */
static inline struct iif_fence *iif_dma_fence_wait(struct iif_manager *mgr,
						   struct dma_fence *dma_fence)
{
	return iif_dma_fence_wait_timeout(mgr, dma_fence, MAX_SCHEDULE_TIMEOUT);
}

/**
 * iif_dma_fence_stop() - Stops @iif_fence waiting on its bridged DMA fence.
 * @iif_fence: The inter-IP fence to stop waiting on its bridged DMA fence.
 *
 * The thread waiting on the DMA fence will be interrupted. If the thread was interrupted before
 * the DMA fence is signaled, @iif_fence will be signaled with -ERESTARTSYS error.
 *
 * This function must be called with an inter-IP fence which was created by `iif_dma_fence_wait()`
 * only. Also, it cannot be called in the IRQ context.
 *
 * This function can be called once at any time even after @iif_fence has been signaled and the
 * thread waiting on the DMA fence already terminated.
 */
void iif_dma_fence_stop(struct iif_fence *iif_fence);

/**
 * iif_dma_fence_stop_and_put_async() - Stops @iif_fence waiting on its bridged DMA fence and put
 *                                      @iif_fence asynchronously.
 * @iif_fence: The inter-IP fence to stop waiting on its bridged DMA fence.
 *
 * The role of this function is basically the same with the `iif_dma_fence_stop()` function, but
 * stops the thread waiting on its bridged DMA fence asynchronously. Also, the caller cannot decide
 * when to put the refcount of @iif_fence as it stops the thread asynchronously, this function will
 * do that after it has actually stopped the thread. Therefore, the caller must NOT put the refcount
 * from their side unless they are holding additional refcounts.
 */
void iif_dma_fence_stop_and_put_async(struct iif_fence *iif_fence);

/**
 * iif_dma_fence_bridge() - Creates a single-shot IIF which will be signaled once @dma_fence is
 *                          signaled.
 * @mgr: IIF manager to create an inter-IP fence.
 * @dma_fence: The DMA fence to be bridged to the created IIF.
 *
 * The major difference from the `iif_dma_fence_wait{_timeout}()` functions is that, this function
 * doesn't spawn a thread to wait on @dma_fence. It simply registers a poll callback to @dma_fence,
 * which will notify the returned IIF once @dma_fence is signaled. Therefore, the caller doesn't
 * need to take care of the thread life-cycle. (i.e., don't need to call the `iif_dma_fence_stop()`
 * function)
 *
 * Note that the caller must release the refcount of the returned IIF (i.e., iif_fence_put()) if
 * they don't need to access it anymore.
 *
 * This function cannot be called in the IRQ context.
 *
 * Returns a IIF on success. Otherwise, returns an error pointer.
 */
struct iif_fence *iif_dma_fence_bridge(struct iif_manager *mgr, struct dma_fence *dma_fence);

/**
 * iif_dma_fence_get_base() - Returns the base DMA fence which @iif_fence was bridged to.
 *
 * @dma_fence: The IIF to get its base DMA fence.
 *
 * Returns the DMA fence bridged to @iif_fence if it was created by the `iif_dma_fence_bridge()` or
 * `iif_dma_fence_wait{_timeout}()` functions. Otherwise, returns NULL.
 */
struct dma_fence *iif_dma_fence_get_base(struct iif_fence *iif_fence);

/**
 * dma_iif_fence_bridge() - Creates a DMA fence which will be signaled once @iif_fence is signaled.
 * @iif_fence: The IIF to be bridged.
 *
 * Note that the caller must release the refcount of the returned DMA fence (i.e., dma_fence_put())
 * if they don't need to access it anymore.
 *
 * If @iif_fence is a reusable fence, the returned DMA fence will be signaled only once at the first
 * signal of reached sync point after this function call.
 *
 * This function cannot be called in the IRQ context.
 *
 * Returns a DMA fence on success. Otherwise, returns an error.
 */
struct dma_fence *dma_iif_fence_bridge(struct iif_fence *iif_fence);

/**
 * dma_iif_fence_get_base() - Returns the base IIF which @dma_fence was bridged to.
 *
 * @dma_fence: The DMA fence to get its base IIF.
 *
 * Returns the IIF bridged to @dma_fence if it was created by the `dma_iif_fence_bridge()` function.
 * Otherwise, returns NULL.
 */
struct iif_fence *dma_iif_fence_get_base(struct dma_fence *dma_fence);

#endif /* __IIF_IIF_DMA_FENCE_H__ */

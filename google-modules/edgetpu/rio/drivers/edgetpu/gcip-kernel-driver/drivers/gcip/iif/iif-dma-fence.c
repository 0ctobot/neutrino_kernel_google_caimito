// SPDX-License-Identifier: GPL-2.0-only
/*
 * Implementation of IIF-DMA or DMA-IIF bridge.
 *
 * Copyright (C) 2024-2025 Google LLC
 */

#include <linux/dma-fence.h>
#include <linux/kthread.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <iif/iif-dma-fence.h>
#include <iif/iif-fence.h>
#include <iif/iif-platform.h>
#include <iif/iif-shared.h>
#include <iif/iif.h>

#define TEST_NOTIFY_TASK_START()
#define TEST_NOTIFY_FENCE_SIGNAL()
#define TEST_SKIP_WAKE_UP_PROCESS() (false)
#define TEST_INIT_SIGNALED_WAITQ(...)
#define TEST_NOTIFY_SIGNALED_WAITQ(...)

static void iif_dma_fence_release(struct kref *kref)
{
	struct iif_dma_fence *iif_dma_fence = container_of(kref, struct iif_dma_fence, kref);

	kfree(iif_dma_fence);
}

/* The callback which will be invoked when the refcount of @iif becomes 0. */
static void iif_dma_fence_on_release(struct iif_fence *iif)
{
	struct iif_dma_fence *iif_dma_fence = iif_to_iif_dma_fence(iif);
	struct dma_fence *dma_fence = iif_dma_fence->base_fence;

	if (dma_fence) {
		dma_fence_remove_callback(dma_fence, &iif_dma_fence->poll_cb.dma_cb);
		dma_fence_put(dma_fence);
	}

	if (iif_dma_fence->task)
		put_task_struct(iif_dma_fence->task);

	iif_dma_fence_put(iif_dma_fence);
}

/* The operators which will be registered to inter-IP fences. */
static const struct iif_fence_ops iif_dma_fence_ops = {
	.on_release = iif_dma_fence_on_release,
};

static void iif_dma_fence_poll_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct iif_dma_fence *iif_dma_fence =
		container_of(cb, struct iif_dma_fence, poll_cb.dma_cb);
	struct iif_fence *iif_fence = &iif_dma_fence->bridged_fence.iif_fence;
	int status = dma_fence_get_status_locked(fence);

	/* Theoretically, this function must be called only when @dma_fence has been signaled. */
	if (!status) {
		iif_warn(iif_fence, "dma_fence has not been signaled yet. Skip signaling IIF.");
		return;
	}

	iif_fence_signal_with_status_async(iif_fence, status < 0 ? status : 0);
}

static const char *dma_iif_fence_get_driver_name(struct dma_fence *fence)
{
	return IIF_DRIVER_NAME;
}

static const char *dma_iif_fence_get_timeline_name(struct dma_fence *fence)
{
	return IIF_DRIVER_NAME "_timeline";
}

static void dma_iif_fence_release(struct dma_fence *dma_fence)
{
	struct iif_dma_fence *iif_dma_fence = dma_to_iif_dma_fence(dma_fence);

	/*
	 * Instead of releasing @iif_dma_fence directly here, schedule a deferred work because we
	 * should cancel @iif_dma_fence->signal_work synchronously, but `dma_fence_put()` can be
	 * called in any context.
	 */
	schedule_work(&iif_dma_fence->release_work);
}

static const struct dma_fence_ops dma_iif_fence_ops = {
	.get_driver_name = dma_iif_fence_get_driver_name,
	.get_timeline_name = dma_iif_fence_get_timeline_name,
	.release = dma_iif_fence_release,
};

/*
 * The deferred work function which will be scheduled when @iif_fence is signaled to signal
 * @dma_fence.
 */
static void dma_iif_fence_signal_work(struct work_struct *work)
{
	struct iif_dma_fence *iif_dma_fence = container_of(work, struct iif_dma_fence, signal_work);
	struct iif_fence *iif_fence = iif_dma_fence->base_fence;
	struct dma_fence *dma_fence = &iif_dma_fence->bridged_fence.dma_fence;

	/*
	 * If @iif_fence is reusable fence, this work can be scheduled multiple times. If @dma_fence
	 * is already signaled ignore it.
	 */
	if (dma_fence_is_signaled(dma_fence))
		return;

	if (iif_fence->signal_error)
		dma_fence_set_error(dma_fence, iif_fence->signal_error);
	dma_fence_signal(dma_fence);
}

/*
 * The deferred work function which will be scheduled when @iif_dma_fence->dma_fence is going to be
 * released.
 */
static void dma_iif_fence_release_work(struct work_struct *work)
{
	struct iif_dma_fence *iif_dma_fence =
		container_of(work, struct iif_dma_fence, release_work);
	struct iif_fence *iif_fence = iif_dma_fence->base_fence;

	if (iif_fence) {
		/*
		 * It is not guaranteed that @iif_dma_fence->dma_fence will be released only after
		 * @iif_fence is signaled. Therefore, we should unregister the poll callback from it
		 * and cancel @signal_work to ensure that there will be UAF bug.
		 */
		iif_fence_remove_poll_callback(iif_fence, &iif_dma_fence->poll_cb.iif_cb);
		cancel_work_sync(&iif_dma_fence->signal_work);
		iif_fence_put(iif_fence);
	}

	iif_dma_fence_put(iif_dma_fence);
}

/*
 * The poll callback which will be registered to @fence and it will be invoked when @fence is
 * signaled. This callback schedules a deferred work which executes `dma_iif_fence_signal_work()`.
 */
static void dma_iif_fence_poll_cb(struct iif_fence *fence, struct iif_fence_poll_cb *cb)
{
	struct iif_dma_fence *iif_dma_fence =
		container_of(cb, struct iif_dma_fence, poll_cb.iif_cb);

	/* No need to check the return value since a DMA fence can only be signaled at most once. */
	schedule_work(&iif_dma_fence->signal_work);
}

/*
 * Will be called when @iif_dma_fence->task thread has stopped waiting on @iif_fence to be signaled.
 * If the thread has been interrupted by the `iif_dma_fence_stop()` function before it executes,
 * the stop function will call this function on behald of the thread.
 *
 * If @iif_fence hasn't been signaled (i.e., its bridged DMA fence hasn't been signaled), the fence
 * will be signaled with @error error.
 */
static void iif_dma_fence_thread_waited(struct iif_dma_fence *iif_dma_fence, int error)
{
	struct iif_fence *iif_fence = &iif_dma_fence->bridged_fence.iif_fence;
	struct dma_fence *dma_fence = iif_dma_fence->base_fence;

	/*
	 * When this function is called, the thread has stopped waiting on @iif_fence to be signaled
	 * which means that:
	 * 1. @iif_fence was actually signaled. Or,
	 * 2. A timeout has occurred before the fence is signaled.
	 *
	 * If the case is the first one, it is safe to remove the callback registered to @dma_fence
	 * since the fence was already signaled.
	 *
	 * If the case is the second one, the thread should take the responsibility of signaling
	 * @iif_fence with an error, it should remove the registered callback first to prevent any
	 * race conditions with @dma_fence.
	 */
	dma_fence_remove_callback(dma_fence, &iif_dma_fence->poll_cb.dma_cb);

	/* If @iif_fence was already signaled, don't need to do anything. */
	if (iif_fence_is_signaled(iif_fence))
		goto out;

	/*
	 * If code reaches here, it is guaranteed that @iif_fence is not and will never be signaled.
	 * Signals the fence with @error.
	 */
	iif_fence_signal_with_status(iif_fence, error);
	iif_fence_signaler_completed(iif_fence);
	TEST_NOTIFY_SIGNALED_WAITQ(iif_dma_fence);
out:
	iif_fence_put(iif_fence);
}

/*
 * The thread function which waits on the DMA fence to be signaled and will signal the inter-IP
 * fence once the DMA fence has been signaled.
 */
static int iif_dma_fence_thread_func(void *data)
{
	struct iif_dma_fence *iif_dma_fence = data;
	struct iif_fence *iif_fence = &iif_dma_fence->bridged_fence.iif_fence;
	signed long wait_status;

	TEST_NOTIFY_TASK_START();

	wait_status = iif_fence_wait_timeout(iif_fence, true, iif_dma_fence->timeout_jiffies);

	/*
	 * For testing the race condition that the IP driver is calling `iif_dma_fence_stop()` right
	 * after the fence has been signaled. The thread should handle the fence signaled as usual
	 * and the function call should wait for the thread termination properly.
	 */
	TEST_NOTIFY_FENCE_SIGNAL();

	if (wait_status > 0)
		wait_status = 0;
	else if (!wait_status)
		wait_status = -ETIMEDOUT;

	iif_dma_fence_thread_waited(iif_dma_fence, wait_status);

	return 0;
}

static void iif_dma_fence_stop_work(struct work_struct *work)
{
	struct iif_dma_fence *iif_dma_fence = container_of(work, struct iif_dma_fence, stop_work);
	struct iif_fence *iif_fence = &iif_dma_fence->bridged_fence.iif_fence;

	iif_dma_fence_stop(iif_fence);
	iif_fence_put(iif_fence);
}

struct iif_dma_fence *iif_dma_fence_get(struct iif_dma_fence *iif_dma_fence)
{
	if (iif_dma_fence)
		kref_get(&iif_dma_fence->kref);
	return iif_dma_fence;
}

void iif_dma_fence_put(struct iif_dma_fence *iif_dma_fence)
{
	kref_put(&iif_dma_fence->kref, iif_dma_fence_release);
}

struct iif_fence *iif_dma_fence_wait_timeout(struct iif_manager *mgr, struct dma_fence *dma_fence,
					     signed long timeout_jiffies)
{
	struct iif_dma_fence *iif_dma_fence;
	struct iif_fence *iif_fence;
	int ret;

	iif_fence = iif_dma_fence_bridge(mgr, dma_fence);
	if (IS_ERR(iif_fence))
		return iif_fence;

	iif_dma_fence = iif_to_iif_dma_fence(iif_fence);
	iif_dma_fence->timeout_jiffies = timeout_jiffies;
	INIT_WORK(&iif_dma_fence->stop_work, iif_dma_fence_stop_work);

	/* The kthread should hold the refcount of @iif_fence to avoid UAF bug. */
	iif_fence_get(iif_fence);
	TEST_INIT_SIGNALED_WAITQ(iif_dma_fence);

	/* Creates a thread waiting on @dma_fence. */
	iif_dma_fence->task = kthread_create(iif_dma_fence_thread_func, iif_dma_fence,
					     "iif_dma_fence_iif_%d_dma_%llu_%llu", iif_fence->id,
					     dma_fence->context, dma_fence->seqno);
	if (IS_ERR(iif_dma_fence->task)) {
		ret = PTR_ERR(iif_dma_fence->task);
		goto err_put_iif_kthread;
	}

	/*
	 * To prevent a race condition which can happen between the thread completion and the
	 * `iif_dma_fence_stop()` function call, increment the refcount of @task.
	 */
	get_task_struct(iif_dma_fence->task);

	/* Starts the thread. */
	if (!TEST_SKIP_WAKE_UP_PROCESS())
		wake_up_process(iif_dma_fence->task);

	return iif_fence;

err_put_iif_kthread:
	/* Releases the refcount which was supposed to be held by the kthread. */
	iif_fence_put(iif_fence);

	/*
	 * Theoretically, we don't need to signal IIF in the error case as no one other than this
	 * driver accesses IIF. However, IIF driver may print warning logs if IIF is going to be
	 * destroyed with outstanding signalers. To prevent that and to keep the convention of the
	 * usage of IIF, signal IIF here.
	 */
	iif_fence_signal_with_status(iif_fence, ret);
	iif_fence_signaler_completed(iif_fence);

	/* Releases the refcount which was set when the IIF was initilaized. */
	iif_fence_put(iif_fence);

	return ERR_PTR(ret);
}

void iif_dma_fence_stop(struct iif_fence *iif_fence)
{
	struct iif_dma_fence *iif_dma_fence = iif_to_iif_dma_fence(iif_fence);
	int ret;

	/*
	 * Interrupts the thread of @iif_dma_fence.
	 *
	 * - If the thread hasn't started execution, the `kthread_stop()` will return -EINTR error.
	 *   In this case, this stop function will call the `iif_dma_fence_thread_waited()` function
	 *   below on behalf of the thread to clean up resources held by the thread.
	 *
	 * - Otherwise, the function will always return 0 and it is guaranteed that the thread has
	 *   executed and cleaned up its resources.
	 *
	 * Note that this function call waits the thread termination synchronously.
	 */
	ret = kthread_stop(iif_dma_fence->task);
	if (!ret)
		return;

	WARN_ON(ret != -EINTR);

	/*
	 * As `iif_fence_wait_timeout()` returns -ERESTARTSYS when the thread is interrupted, follow
	 * the same error code here.
	 */
	iif_dma_fence_thread_waited(iif_dma_fence, -ERESTARTSYS);
}

void iif_dma_fence_stop_and_put_async(struct iif_fence *iif_fence)
{
	struct iif_dma_fence *iif_dma_fence = iif_to_iif_dma_fence(iif_fence);

	schedule_work(&iif_dma_fence->stop_work);
}

struct iif_fence *iif_dma_fence_bridge(struct iif_manager *mgr, struct dma_fence *dma_fence)
{
	struct iif_dma_fence *iif_dma_fence = kzalloc(sizeof(*iif_dma_fence), GFP_KERNEL);
	struct iif_fence *iif_fence;
	int ret;

	if (!iif_dma_fence)
		return ERR_PTR(-ENOMEM);

	iif_fence = &iif_dma_fence->bridged_fence.iif_fence;

	ret = iif_fence_init(mgr, iif_fence, &iif_dma_fence_ops, IIF_IP_AP, 1);
	if (ret) {
		kfree(iif_dma_fence);
		return ERR_PTR(ret);
	}

	/* From now on, resources will be released when the refcount of @iif_fence becomes 0. */

	kref_init(&iif_dma_fence->kref);
	iif_dma_fence->base_fence = dma_fence_get(dma_fence);

	/*
	 * Set `IIF_FLAGS_RETIRE_ON_RELEASE` flag to let the IIF retire only on release. Otherwise,
	 * if @fence is signaled before any waiter starts waiting on @iif_fence, the IIF can retire
	 * and the waiter will access an invalid IIF.
	 */
	iif_fence_set_flags(iif_fence, IIF_FLAGS_RETIRE_ON_RELEASE, false);

	/*
	 * Add a signaler to @iif_fence. The fence will be signaled when the poll callback which
	 * will be registered below is invoked.
	 */
	iif_fence_submit_signaler(iif_fence);

	ret = dma_fence_add_callback(dma_fence, &iif_dma_fence->poll_cb.dma_cb,
				     iif_dma_fence_poll_cb);
	if (ret)
		iif_dma_fence_poll_cb(dma_fence, &iif_dma_fence->poll_cb.dma_cb);

	return iif_fence;
}

struct dma_fence *iif_dma_fence_get_base(struct iif_fence *iif_fence)
{
	if (iif_fence->ops != &iif_dma_fence_ops)
		return NULL;
	return iif_to_iif_dma_fence(iif_fence)->base_fence;
}

struct dma_fence *dma_iif_fence_bridge(struct iif_fence *iif_fence)
{
	struct iif_dma_fence *iif_dma_fence = kzalloc(sizeof(*iif_dma_fence), GFP_KERNEL);
	struct dma_fence *dma_fence;
	int ret;

	if (!iif_dma_fence)
		return ERR_PTR(-ENOMEM);

	dma_fence = &iif_dma_fence->bridged_fence.dma_fence;
	spin_lock_init(&iif_dma_fence->dma_fence_lock);

	/* Initializes @dma_fence which will be signaled when @iif_fence is signaled. */
	dma_fence_init(dma_fence, &dma_iif_fence_ops, &iif_dma_fence->dma_fence_lock,
		       iif_fence->mgr->dma_fence_context,
		       atomic64_fetch_inc(&iif_fence->mgr->dma_fence_seqno));

	/*
	 * From now on, @iif_dma_fence will be released when the refcount of @dma_fence becomes
	 * 0.
	 */

	INIT_WORK(&iif_dma_fence->signal_work, dma_iif_fence_signal_work);
	INIT_WORK(&iif_dma_fence->release_work, dma_iif_fence_release_work);
	kref_init(&iif_dma_fence->kref);
	iif_dma_fence->base_fence = iif_fence_get(iif_fence);

	ret = iif_fence_add_poll_callback(iif_fence, &iif_dma_fence->poll_cb.iif_cb,
					  dma_iif_fence_poll_cb);
	/* If @ret is non-zero, @iif_fence is already signaled. */
	if (ret) {
		if (iif_fence->signal_error)
			dma_fence_set_error(dma_fence, iif_fence->signal_error);
		dma_fence_signal(dma_fence);
	}

	return dma_fence;
}

struct iif_fence *dma_iif_fence_get_base(struct dma_fence *dma_fence)
{
	if (dma_fence->ops != &dma_iif_fence_ops)
		return NULL;
	return dma_to_iif_dma_fence(dma_fence)->base_fence;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * GCIP-integrated IIF driver fence.
 *
 * Copyright (C) 2023 Google LLC
 */

#define pr_fmt(fmt) "iif: " fmt

#include <linux/atomic.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
#include <linux/container_of.h>
#endif

#include <gcip/iif/iif-fence-table.h>
#include <gcip/iif/iif-fence.h>
#include <gcip/iif/iif-manager.h>
#include <gcip/iif/iif-sync-file.h>
#include <gcip/iif/iif.h>

/*
 * Returns the number of remaining signalers to be submitted. Returns 0 if all signalers are
 * submitted.
 *
 * Caller must hold @fence->submitted_signalers_lock.
 */
static int iif_fence_unsubmitted_signalers_locked(struct iif_fence *fence)
{
	lockdep_assert_held(&fence->submitted_signalers_lock);

	return fence->total_signalers - fence->submitted_signalers;
}

/*
 * Checks whether all signalers have signaled @fence or not.
 *
 * Caller must hold @fence->outstanding_waiters_lock.
 */
static bool iif_fence_is_signaled_locked(struct iif_fence *fence)
{
	lockdep_assert_held(&fence->outstanding_waiters_lock);

	return fence->signaled_signalers == fence->total_signalers;
}

static inline bool iif_fence_has_retired(struct iif_fence *fence)
{
	return fence->state == IIF_FENCE_STATE_RETIRED;
}

/* Returns the fence ID to the ID pool. */
static void iif_fence_retire(struct iif_fence *fence)
{
	if (iif_fence_has_retired(fence))
		return;
	ida_free(&fence->mgr->idp, fence->id);
	fence->state = IIF_FENCE_STATE_RETIRED;
}

/*
 * If there are no more outstanding waiters and no file binding to this fence, we can assume that
 * there will be no more signalers/waiters. Therefore, we can retire the fence ID earlier to not
 * block allocating an another fence.
 *
 * This function must be called with holding @fence->outstanding_waiters_lock.
 */
static void iif_fence_retire_if_possible_locked(struct iif_fence *fence)
{
	lockdep_assert_held(&fence->outstanding_waiters_lock);

	if (!fence->outstanding_waiters && fence->state != IIF_FENCE_STATE_FILE_CREATED)
		iif_fence_retire(fence);
}

/* Cleans up @fence which was initialized by the `iif_fence_init` function. */
static void iif_fence_destroy(struct kref *kref)
{
	struct iif_fence *fence = container_of(kref, struct iif_fence, kref);

	/*
	 * It is supposed to be retired when the file is closed and there are no more outstanding
	 * waiters. However, let's ensure that the fence is retired before releasing it. We don't
	 * have to hold @fence->outstanding_waiters_lock here because this function is called only
	 * when the fence can't be accessed anymore.
	 */
	iif_fence_retire(fence);

	if (fence->ops && fence->ops->on_release)
		fence->ops->on_release(fence);
}

int iif_fence_init(struct iif_manager *mgr, struct iif_fence *fence,
		   const struct iif_fence_ops *ops, enum iif_ip_type signaler_ip,
		   uint16_t total_signalers)
{
	unsigned int id_min = signaler_ip * IIF_NUM_FENCES_PER_IP,
		     id_max = id_min + IIF_NUM_FENCES_PER_IP - 1;

	fence->id = ida_alloc_range(&mgr->idp, id_min, id_max, GFP_KERNEL);
	if (fence->id < 0)
		return fence->id;

	fence->mgr = mgr;
	fence->total_signalers = total_signalers;
	fence->submitted_signalers = 0;
	fence->signaled_signalers = 0;
	fence->outstanding_waiters = 0;
	fence->ops = ops;
	fence->state = IIF_FENCE_STATE_INITIALIZED;
	kref_init(&fence->kref);
	spin_lock_init(&fence->submitted_signalers_lock);
	spin_lock_init(&fence->signaled_signalers_lock);
	spin_lock_init(&fence->outstanding_waiters_lock);
	iif_fence_table_init_fence_entry(&mgr->fence_table, fence->id, total_signalers);
	INIT_LIST_HEAD(&fence->poll_cb_list);
	INIT_LIST_HEAD(&fence->all_signaler_submitted_cb_list);

	return 0;
}

int iif_fence_install_fd(struct iif_fence *fence)
{
	struct iif_sync_file *sync_file;
	int fd, ret;

	spin_lock(&fence->outstanding_waiters_lock);

	if (fence->state != IIF_FENCE_STATE_INITIALIZED) {
		if (iif_fence_has_retired(fence)) {
			pr_err("The fence is already retired, can't install an FD");
			ret = -EPERM;
		} else {
			pr_err("Only one file can be bound to an fence");
			ret = -EEXIST;
		}
		goto err_unlock;
	}

	ret = get_unused_fd_flags(O_CLOEXEC);
	if (ret < 0)
		goto err_unlock;
	fd = ret;

	sync_file = iif_sync_file_create(fence);
	if (IS_ERR(sync_file)) {
		ret = PTR_ERR(sync_file);
		goto err_put_fd;
	}

	fd_install(fd, sync_file->file);
	fence->state = IIF_FENCE_STATE_FILE_CREATED;

	spin_unlock(&fence->outstanding_waiters_lock);

	return fd;

err_put_fd:
	put_unused_fd(fd);
err_unlock:
	spin_unlock(&fence->outstanding_waiters_lock);

	return ret;
}

void iif_fence_on_sync_file_release(struct iif_fence *fence)
{
	unsigned long flags;

	spin_lock_irqsave(&fence->outstanding_waiters_lock, flags);

	fence->state = IIF_FENCE_STATE_FILE_RELEASED;
	iif_fence_retire_if_possible_locked(fence);

	spin_unlock_irqrestore(&fence->outstanding_waiters_lock, flags);
}

struct iif_fence *iif_fence_get(struct iif_fence *fence)
{
	if (fence)
		kref_get(&fence->kref);
	return fence;
}

struct iif_fence *iif_fence_fdget(int fd)
{
	struct iif_sync_file *sync_file;
	struct iif_fence *fence;

	sync_file = iif_sync_file_fdget(fd);
	if (IS_ERR(sync_file))
		return ERR_CAST(sync_file);

	fence = iif_fence_get(sync_file->fence);

	/*
	 * Since `iif_sync_file_fdget` opens the file and increases the file refcount, put here as
	 * we don't need to access the file anymore in this function.
	 */
	fput(sync_file->file);

	return fence;
}

void iif_fence_put(struct iif_fence *fence)
{
	if (fence)
		kref_put(&fence->kref, iif_fence_destroy);
}

int iif_fence_submit_signaler(struct iif_fence *fence)
{
	struct iif_fence_all_signaler_submitted_cb *cur, *tmp;
	int ret = 0;

	spin_lock(&fence->submitted_signalers_lock);

	/* Already all signalers are submitted. No more submission is allowed. */
	if (fence->submitted_signalers >= fence->total_signalers) {
		ret = -EPERM;
		goto out;
	}

	fence->submitted_signalers++;

	/* The last signaler has been submitted. */
	if (fence->submitted_signalers == fence->total_signalers) {
		list_for_each_entry_safe (cur, tmp, &fence->all_signaler_submitted_cb_list, node) {
			list_del_init(&cur->node);
			cur->func(fence, cur);
		}
	}
out:
	spin_unlock(&fence->submitted_signalers_lock);

	return ret;
}

int iif_fence_unsubmitted_signalers(struct iif_fence *fence)
{
	unsigned long flags;
	int unsubmitted;

	spin_lock_irqsave(&fence->submitted_signalers_lock, flags);
	unsubmitted = iif_fence_unsubmitted_signalers_locked(fence);
	spin_unlock_irqrestore(&fence->submitted_signalers_lock, flags);

	return unsubmitted;
}

int iif_fence_submit_waiter(struct iif_fence *fence, enum iif_ip_type ip)
{
	int unsubmitted = iif_fence_unsubmitted_signalers(fence);

	if (unsubmitted)
		return unsubmitted;

	spin_lock(&fence->outstanding_waiters_lock);

	fence->outstanding_waiters++;
	iif_fence_table_set_waiting_ip(&fence->mgr->fence_table, fence->id, ip);

	spin_unlock(&fence->outstanding_waiters_lock);

	return 0;
}

void iif_fence_signal(struct iif_fence *fence)
{
	struct iif_fence_poll_cb *cur, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&fence->signaled_signalers_lock, flags);

	if (fence->signaled_signalers >= fence->total_signalers) {
		pr_warn("The fence is already signaled, id=%u", fence->id);
		goto out;
	}

	fence->signaled_signalers++;

	/* All signalers have signaled the fence. */
	if (iif_fence_is_signaled_locked(fence)) {
		list_for_each_entry_safe (cur, tmp, &fence->poll_cb_list, node) {
			list_del_init(&cur->node);
			cur->func(fence, cur);
		}
	}
out:
	spin_unlock_irqrestore(&fence->signaled_signalers_lock, flags);
}

bool iif_fence_is_signaled(struct iif_fence *fence)
{
	unsigned long flags;
	bool signaled;

	spin_lock_irqsave(&fence->outstanding_waiters_lock, flags);
	signaled = iif_fence_is_signaled_locked(fence);
	spin_unlock_irqrestore(&fence->outstanding_waiters_lock, flags);

	return signaled;
}

void iif_fence_waited(struct iif_fence *fence)
{
	unsigned long flags;

	spin_lock_irqsave(&fence->outstanding_waiters_lock, flags);

	if (fence->outstanding_waiters) {
		fence->outstanding_waiters--;
		iif_fence_retire_if_possible_locked(fence);
	}

	spin_unlock_irqrestore(&fence->outstanding_waiters_lock, flags);
}

int iif_fence_add_poll_callback(struct iif_fence *fence, struct iif_fence_poll_cb *poll_cb,
				iif_fence_poll_cb_t func)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&fence->signaled_signalers_lock, flags);

	if (iif_fence_is_signaled_locked(fence)) {
		ret = -EPERM;
		goto out;
	}

	poll_cb->func = func;
	list_add_tail(&poll_cb->node, &fence->poll_cb_list);
out:
	spin_unlock_irqrestore(&fence->signaled_signalers_lock, flags);

	return ret;
}

void iif_fence_remove_poll_callback(struct iif_fence *fence, struct iif_fence_poll_cb *poll_cb)
{
	unsigned long flags;

	spin_lock_irqsave(&fence->signaled_signalers_lock, flags);

	if (!list_empty(&poll_cb->node))
		list_del_init(&poll_cb->node);

	spin_unlock_irqrestore(&fence->signaled_signalers_lock, flags);
}

int iif_fence_add_all_signaler_submitted_callback(struct iif_fence *fence,
						  struct iif_fence_all_signaler_submitted_cb *cb,
						  iif_fence_all_signaler_submitted_cb_t func)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&fence->submitted_signalers_lock, flags);

	cb->remaining_signalers = iif_fence_unsubmitted_signalers_locked(fence);

	/* Already all signalers are submitted. */
	if (!cb->remaining_signalers) {
		ret = -EPERM;
		goto out;
	}

	cb->func = func;
	list_add_tail(&cb->node, &fence->all_signaler_submitted_cb_list);
out:
	spin_unlock_irqrestore(&fence->submitted_signalers_lock, flags);

	return ret;
}

bool iif_fence_remove_all_signaler_submitted_callback(
	struct iif_fence *fence, struct iif_fence_all_signaler_submitted_cb *cb)
{
	unsigned long flags;
	bool removed = false;

	spin_lock_irqsave(&fence->submitted_signalers_lock, flags);

	if (!list_empty(&cb->node)) {
		list_del_init(&cb->node);
		removed = true;
	}

	spin_unlock_irqrestore(&fence->submitted_signalers_lock, flags);

	return removed;
}

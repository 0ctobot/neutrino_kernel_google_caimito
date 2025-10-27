// SPDX-License-Identifier: GPL-2.0-only
/*
 * Implementation of the direct IIF.
 *
 * The direct IIF can be used if there is no underlying sync-unit (e.g., SSU) and IP firmwares are
 * communicating with each other directly to signal fences. Therefore, to support direct fences, we
 * also need support of the firmware side.
 *
 * Copyright (C) 2025 Google LLC
 */

#include <linux/idr.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <iif/iif-fence-table.h>
#include <iif/iif-fence.h>
#include <iif/iif-manager.h>
#include <iif/iif.h>

struct iif_direct_fence {
	int id;
	int timeline;
	int error;
	int num_sync_points;
	int last_poll_timeline;
	struct iif_fence *iif;
	struct iif_manager *mgr;
	struct iif_fence_params params;
	struct list_head poll_cb_list;
	spinlock_t poll_cb_lock;
	bool error_set_by_kernel;
};

static void iif_direct_fence_set_error(struct iif_direct_fence *fence, int error)
{
	if (fence->error && error != fence->error)
		iif_warn(fence->iif, "fence error has been overwritten: %d -> %d\n", fence->error,
			 error);

	fence->error = error;
}

static inline void iif_direct_fence_set_timeline(struct iif_direct_fence *fence, int timeline)
{
	fence->timeline = timeline;
}

static bool iif_direct_fence_table_is_errored(struct iif_direct_fence *fence)
{
	return iif_fence_table_get_flag(&fence->mgr->fence_table, fence->id) &
	       BIT(IIF_SIGNAL_TABLE_FLAG_ERROR_BIT);
}

static int iif_direct_fence_table_get_error(struct iif_direct_fence *fence)
{
	return iif_fence_table_get_error(&fence->mgr->fence_table, fence->id);
}

static void iif_direct_fence_table_set_error(struct iif_direct_fence *fence, int error)
{
	iif_fence_table_set_error(&fence->mgr->fence_table, fence->id, error);
}

static unsigned int iif_direct_fence_table_get_remaining_signals(struct iif_direct_fence *fence)
{
	return iif_fence_table_get_remaining_signals(&fence->mgr->fence_table, fence->id);
}

/* Sets the remaining signals of the fence to the signal fence table. */
static void iif_direct_fence_table_set_remaining_signals(struct iif_direct_fence *fence,
							 int remaining_signals)
{
	if (fence->iif->propagate)
		iif_fence_table_set_remaining_signals(&fence->mgr->fence_table, fence->id,
						      remaining_signals);
}

/* Increases the timeline value of the fence in the fence table. */
static void iif_direct_fence_table_inc_timeline(struct iif_direct_fence *fence)
{
	if (fence->iif->propagate)
		iif_fence_table_inc_timeline(&fence->mgr->fence_table, fence->id);
}

/* Returns the timeline value of the fence from the fence table. */
static unsigned int iif_direct_fence_table_get_timeline(struct iif_direct_fence *fence)
{
	if (fence->params.fence_type == IIF_FENCE_TYPE_SINGLE_SHOT)
		return fence->params.remaining_signalers -
		       iif_direct_fence_table_get_remaining_signals(fence);

	return iif_fence_table_get_timeline(&fence->mgr->fence_table, fence->id);
}

/* Adds a new sync point to the fence table. */
static void iif_direct_fence_table_set_sync_point(struct iif_direct_fence *fence,
						  unsigned int timeline, unsigned int count)
{
	iif_fence_table_set_sync_point(&fence->mgr->fence_table, fence->id, fence->num_sync_points,
				       timeline, count);
	fence->num_sync_points++;
}

/* Gets the sync point timeline and count at @sync_point_index from the fence table. */
static void iif_direct_fence_table_get_sync_point(struct iif_direct_fence *fence,
						  int sync_point_index, u8 *timeline, u8 *count)
{
	iif_fence_table_get_sync_point(&fence->mgr->fence_table, fence->id, sync_point_index,
				       timeline, count);
}

/* Returns true if the sync point at @sync_point_index is ready to notify waiters. */
static bool iif_direct_fence_is_sync_point_ready(struct iif_direct_fence *fence,
						 int sync_point_index)
{
	u8 timeline, count;

	iif_direct_fence_table_get_sync_point(fence, sync_point_index, &timeline, &count);

	/*
	 * E.g., sync point window = [3, 5] (timeline=3, count=3)
	 *
	 * timeline          fence->timeline
	 *       |           |
	 * +-----------------------+
	 * |...| 3 | 4 | 5 | 6 |...|
	 * +-----------------------+
	 *       |       |
	 *       |       timeline + count - 1
	 *       |
	 *       fence->last_poll_timeline
	 *
	 * This case can happen if the sync point is a new one and there were no sync points which
	 * notify waiters for timeline 4 and 5. We should consider this new sync point as ready to
	 * notify waiters even though the current timeline, @fence->timeline, is already outside of
	 * the sync point window.
	 */
	return (count == IIF_WAIT_TABLE_SYNC_POINT_COUNT_ALL ||
		fence->last_poll_timeline < timeline + count - 1) &&
	       fence->timeline >= timeline;
}

/* Handles the error passed while signaling the kernel fence object. */
static void iif_direct_fence_handle_signal_error(struct iif_direct_fence *fence, int error)
{
	int table_error = iif_direct_fence_table_get_error(fence);
	bool errored = iif_direct_fence_table_is_errored(fence);

	/*
	 * For the backward compatibility of IP firmwares which don't set the fence error code to
	 * the table (i.e., only mark the errored flag), if the errored flag was set, but the error
	 * code is empty in the table, let the kernel driver set it on behalf of the signaler IP
	 * firmware regardless of @fence->iif->propagate.
	 */
	if (fence->iif->propagate || (errored && !table_error))
		fence->error_set_by_kernel = true;

	/*
	 * If @error is 0, we don't need to handle the error.
	 * If @fence->error_set_by_kernel is false, the firmware is supposed to set error to the
	 * fence table, and the error of kernel fence object will be synced with the fence table
	 * when `iif_direct_fence_sync_status()` is called.
	 */
	if (!error || !fence->error_set_by_kernel)
		return;

	iif_direct_fence_table_set_error(fence, error);
	iif_direct_fence_set_error(fence, error);
}

/* Invokes poll callbacks registered to a direct single-shot fence. */
static void iif_direct_fence_invoke_poll_callbacks_single_shot(struct iif_direct_fence *fence)
{
	struct iif_manager_fence_ops_poll_cb *cur, *tmp;
	struct iif_fence_status status;
	unsigned long flags;

	lockdep_assert_held(&fence->iif->fence_lock);

	/*
	 * For single-shot fence, notify waiters only if all signalers have signaled the fence
	 * regardless of the fence error.
	 */
	if (fence->timeline < fence->params.remaining_signalers)
		return;

	status.error = fence->error;
	status.signaled = fence->timeline >= fence->params.remaining_signalers;

	spin_lock_irqsave(&fence->poll_cb_lock, flags);

	list_for_each_entry_safe(cur, tmp, &fence->poll_cb_list, node) {
		list_del_init(&cur->node);
		cur->func(fence->iif, &status);
	}

	spin_unlock_irqrestore(&fence->poll_cb_lock, flags);
}

/* Invokes poll callbacks registered to a direct reusable fence. */
static void iif_direct_fence_invoke_poll_callbacks_reusable(struct iif_direct_fence *fence)
{
	struct iif_manager_fence_ops_poll_cb *cur, *tmp;
	struct iif_fence_status status;
	unsigned long flags;
	bool found = false;
	int i;

	lockdep_assert_held(&fence->iif->fence_lock);

	status.timeline = fence->timeline;
	status.error = fence->error;

	if (status.error) {
		/*
		 * If the fence is errored out, poll callbacks should be invoked regardless of sync
		 * points.
		 */
		found = true;
	} else {
		/* If poll callbacks were already notified for the current timeline, skip it. */
		if (fence->last_poll_timeline >= fence->timeline)
			return;

		/* Finds any sync point which should notify waiters for the current timeline. */
		for (i = 0; i < fence->num_sync_points; i++) {
			if (iif_direct_fence_is_sync_point_ready(fence, i)) {
				found = true;
				break;
			}
		}
	}

	if (!found)
		return;

	spin_lock_irqsave(&fence->poll_cb_lock, flags);

	list_for_each_entry_safe(cur, tmp, &fence->poll_cb_list, node) {
		/* If the fence is errored out, the fence doesn't need to be signaled anymore. */
		if (status.error)
			list_del_init(&cur->node);
		cur->func(fence->iif, &status);
	}

	spin_unlock_irqrestore(&fence->poll_cb_lock, flags);

	fence->last_poll_timeline = status.timeline;
}

/*
 * Synchronizes the fence status with the fence table. If it has been actually updated, invoke the
 * poll callback to notify the update to the IIF driver users.
 */
static void iif_direct_fence_sync_status(struct iif_direct_fence *fence,
					 void (*invoke_poll_cb)(struct iif_direct_fence *fence))
{
	int table_timeline, table_error;
	bool table_errored;

	lockdep_assert_held(&fence->iif->fence_lock);

	table_timeline = iif_direct_fence_table_get_timeline(fence);
	table_errored = iif_direct_fence_table_is_errored(fence);
	table_error = iif_direct_fence_table_get_error(fence);

	/* If @fence->error_set_by_kernel is true, use the kernel level fence error directly. */
	if (fence->error_set_by_kernel)
		table_error = fence->error;

	/*
	 * @fence->error has a value means that the errored flag was set and the fence was already
	 * errored out before. Reverting the errored fence to normal state is an invalid action. It
	 * is likely a bug of the signaler IP.
	 */
	if (unlikely(!table_errored && fence->error)) {
		iif_warn(fence->iif, "fence was already errored out, cannot revert it\n");
		table_errored = true;
	}

	/*
	 * Setting the fence error without the setting errored flag to the table is an invalid
	 * action. The error code might be a dirty value unintendedly set by the signaler IP.
	 */
	if (unlikely(!table_errored && table_error)) {
		iif_warn(fence->iif, "fence was not errored out, ignore the error\n");
		table_error = 0;
	}

	/*
	 * If the fence is errored out without any reason, treat it as canceled. If there is another
	 * thread which is going to signal @fence with the exact error code, @fence->error will be
	 * updated at that moment.
	 */
	if (unlikely(table_errored && !table_error))
		table_error = -ECANCELED;

	/* The fence timeline must be always increasing. */
	if (unlikely(table_timeline < fence->timeline)) {
		iif_warn(fence->iif, "fence timeline shouldn't be decreased\n");
		table_timeline = fence->timeline;
	}

	/* If the fence status is not updated, do nothing. */
	if (fence->timeline == table_timeline && fence->error == table_error)
		return;

	/*
	 * If @fence->error_set_by_kernel is true, this function call will be NO-OP as @fence->error
	 * must have been set when `iif_direct_fence_handle_signal_error()` is called. Otherwise, it
	 * will sync the error code of kernel fence object to the fence table.
	 */
	iif_direct_fence_set_error(fence, table_error);
	iif_direct_fence_set_timeline(fence, table_timeline);

	invoke_poll_cb(fence);
}

/* Synchronizes the fence status with the fence table. (For single-shot) */
static inline void iif_direct_fence_sync_status_single_shot(struct iif_direct_fence *fence)
{
	iif_direct_fence_sync_status(fence, iif_direct_fence_invoke_poll_callbacks_single_shot);
}

/* Synchronizes the fence status with the fence table. (For reusable) */
static inline void iif_direct_fence_sync_status_reusable(struct iif_direct_fence *fence)
{
	iif_direct_fence_sync_status(fence, iif_direct_fence_invoke_poll_callbacks_reusable);
}

static int iif_direct_fence_signal_single_shot(struct iif_direct_fence *fence, int error)
{
	int table_remaining_signals;
	bool table_errored;

	lockdep_assert_held(&fence->iif->fence_lock);

	/*
	 * The meaning of this function called when the signaler is an IP is that the IP has
	 * become faulty and the IIF driver takes care of updating the fence table. However, since
	 * the timing of the IP crash is nondeterministic, a race condition that the IP already
	 * unblocked the fence right before the crash, but the IP driver is going to signal the
	 * fence with an error because of the IP crash can happen. Therefore if the fence is already
	 * unblocked without error, we should ignore the signal error sent from the IP driver side.
	 *
	 * Note that if this case happens, some waiter IPs might be already notified of the fence
	 * unblock from the signaler IP before it crashes, but the IIF driver will notify waiter IP
	 * drivers and they may notify their IP of the unblock of the same fences again. That says
	 * waiter IPs can receive the fence unblock notification for the same fence for two times by
	 * the race condition, but we expect that they will ignore the second one.
	 *
	 * When the signaler is AP, that race condition won't happen since the fence table should be
	 * always managed by the IIF driver only and theoretically this logic won't have any effect.
	 */
	table_remaining_signals = iif_direct_fence_table_get_remaining_signals(fence);
	table_errored = iif_direct_fence_table_is_errored(fence);

	if (!table_remaining_signals && !table_errored && error) {
		error = 0;
	} else if (!table_remaining_signals && table_errored && !error) {
		/*
		 * Theoretically, this case wouldn't happen since @fence->propagate was set means
		 * that the signaler IP has been crashed and the IP driver will signal the fence
		 * with an error. Handle it just in case and we can consider that the signaler
		 * command has been canceled.
		 */
		error = -ECANCELED;
	}

	if (fence->iif->propagate && table_remaining_signals)
		table_remaining_signals--;

	/*
	 * Sets the error and remaining signals to the fence table. Note that these functions will
	 * be NO-OP if @propagate is false except setting @error to @fence->error.
	 *
	 * We should set the error before signaling the fence. Otherwise, if @fence->propagate is
	 * true so that the IIF driver is updating the fence table and if it signals the fence
	 * first, waiter IPs may misundestand that the fence has been unblocked without an error.
	 */
	iif_direct_fence_handle_signal_error(fence, error);
	iif_direct_fence_table_set_remaining_signals(fence, table_remaining_signals);

	/* Synchronizes the fence status with the fence table. */
	iif_direct_fence_sync_status_single_shot(fence);

	return 0;
}

static int iif_direct_fence_signal_reusable(struct iif_direct_fence *fence, int error)
{
	int table_timeline;
	bool table_errored;

	lockdep_assert_held(&fence->iif->fence_lock);

	table_timeline = iif_direct_fence_table_get_timeline(fence);
	table_errored = iif_direct_fence_table_is_errored(fence);

	/*
	 * Theoretically, if the error flag in the fence table was set by the firmware, the error
	 * should be propagated from the firmware to kernel driver and the kernel driver should pass
	 * non-zero value to @error. Handle it just in case.
	 */
	if (table_errored && !error)
		error = -ECANCELED;

	/*
	 * If @propagate is true, see if the fence will be timedout by this signal. Otherwise, see
	 * if the fence was already timedout.
	 */
	if (table_timeline >= IIF_SIGNAL_TABLE_MAX_TIMELINE ||
	    table_timeline + (fence->iif->propagate ? 1 : 0) >= fence->params.timeout)
		error = -ETIMEDOUT;

	/*
	 * Updates the error and timeline of the fence table. Note that these functions will be
	 * NO-OP if @fence->iif->propagate is false except setting @error to @fence->error.
	 *
	 * We should set the error before increasing the timeline value. Otherwise, if
	 * @fence->propagate is true so that the IIF driver is updating the fence table and if it
	 * signals the fence first, waiter IPs may misundestand that the fence has been unblocked
	 * without an error.
	 */
	iif_direct_fence_handle_signal_error(fence, error);

	/* Prevents overflow. */
	if (table_timeline < IIF_SIGNAL_TABLE_MAX_TIMELINE)
		iif_direct_fence_table_inc_timeline(fence);

	/* Synchronizes the fence status with the fence table. */
	iif_direct_fence_sync_status_reusable(fence);

	return 0;
}

static void iif_direct_fence_init_single_shot(struct iif_direct_fence *fence)
{
	struct iif_manager *mgr = fence->mgr;

	iif_fence_table_init_single_shot_fence_entry(&mgr->fence_table, fence->id,
						     fence->params.remaining_signalers,
						     fence->params.waiters);
}

static void iif_direct_fence_init_reusable(struct iif_direct_fence *fence)
{
	struct iif_manager *mgr = fence->mgr;

	if (fence->params.timeout > IIF_SIGNAL_TABLE_MAX_TIMELINE)
		fence->params.timeout = IIF_SIGNAL_TABLE_MAX_TIMELINE;

	iif_fence_table_init_reusable_fence_entry(&mgr->fence_table, fence->id,
						  fence->params.timeout, fence->params.waiters);
}

static const char *iif_direct_sync_unit_name(void *driver_data)
{
	return "iif_direct";
}

static int iif_direct_fence_create(struct iif_fence *iif, const struct iif_fence_params *params,
				   void *driver_data)
{
	struct iif_manager *mgr = driver_data;
	struct iif_direct_fence *fence;
	const unsigned int id_min = params->signaler_ip * IIF_NUM_FENCES_PER_IP;
	const unsigned int id_max = id_min + IIF_NUM_FENCES_PER_IP - 1;
	int id, ret;

	/* Direct fence only supports IP or AP signaled fences. */
	if (params->signaler_type != IIF_FENCE_SIGNALER_TYPE_IP)
		return -EOPNOTSUPP;

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence)
		return -ENOMEM;

	/* Allocates the fence ID. */
	id = ida_alloc_range(&mgr->idp, id_min, id_max, GFP_KERNEL);
	if (id < 0) {
		ret = id;
		goto err_free_fence;
	}

	fence->iif = iif;
	fence->mgr = mgr;
	fence->id = id;
	fence->params = *params;
	fence->last_poll_timeline = -1;
	INIT_LIST_HEAD(&fence->poll_cb_list);
	spin_lock_init(&fence->poll_cb_lock);

	/* Initializes the entry of the fence table. */
	if (params->fence_type == IIF_FENCE_TYPE_SINGLE_SHOT) {
		iif_direct_fence_init_single_shot(fence);
	} else if (params->fence_type == IIF_FENCE_TYPE_REUSABLE) {
		iif_direct_fence_init_reusable(fence);
	} else {
		ret = -EOPNOTSUPP;
		goto err_free_id;
	}

	iif_fence_set_priv_fence_data(iif, fence);

	return fence->id;

err_free_id:
	ida_free(&fence->mgr->idp, fence->id);
err_free_fence:
	kfree(fence);

	return ret;
}

static void iif_direct_fence_retire(struct iif_fence *iif, void *driver_data)
{
	struct iif_direct_fence *fence = iif_fence_get_priv_fence_data(iif);

	ida_free(&fence->mgr->idp, fence->id);
	kfree(fence);
}

static int iif_direct_fence_add_sync_point(struct iif_fence *iif, u64 timeline, u64 count,
					   void *driver_data)
{
	struct iif_direct_fence *fence = iif_fence_get_priv_fence_data(iif);

	lockdep_assert_held(&fence->iif->fence_lock);

	/* Only reusable fence supports adding sync points. */
	if (fence->params.fence_type != IIF_FENCE_TYPE_REUSABLE)
		return -EOPNOTSUPP;

	/* No more space to register new sync points. */
	if (fence->num_sync_points >= IIF_NUM_SYNC_POINTS)
		return -ENOSPC;

	/* The timeline and signal count should be bigger than 0. */
	if (!timeline || !count)
		return -EINVAL;

	if (timeline > IIF_SIGNAL_TABLE_MAX_TIMELINE)
		timeline = IIF_SIGNAL_TABLE_MAX_TIMELINE;

	if (count >= IIF_WAIT_TABLE_SYNC_POINT_COUNT_ALL)
		count = IIF_WAIT_TABLE_SYNC_POINT_COUNT_ALL;

	iif_direct_fence_table_set_sync_point(fence, timeline, count);

	/*
	 * If the current fence timeline already overlaps or has paased the new sync point window,
	 * but the last timeline invoked poll callbacks was too old, invoke callbacks here.
	 */
	iif_direct_fence_invoke_poll_callbacks_reusable(fence);

	return 0;
}

static int iif_direct_fence_signal(struct iif_fence *iif, int error, void *driver_data)
{
	struct iif_direct_fence *fence = iif_fence_get_priv_fence_data(iif);

	lockdep_assert_held(&iif->fence_lock);

	if (fence->params.fence_type == IIF_FENCE_TYPE_SINGLE_SHOT)
		return iif_direct_fence_signal_single_shot(fence, error);
	if (fence->params.fence_type == IIF_FENCE_TYPE_REUSABLE)
		return iif_direct_fence_signal_reusable(fence, error);

	return -EOPNOTSUPP;
}

static int iif_direct_add_poll_cb(struct iif_fence *iif, struct iif_manager_fence_ops_poll_cb *cb,
				  void *driver_data)
{
	struct iif_direct_fence *fence = iif_fence_get_priv_fence_data(iif);
	unsigned long flags;

	INIT_LIST_HEAD(&cb->node);

	spin_lock_irqsave(&fence->poll_cb_lock, flags);
	list_add_tail(&cb->node, &fence->poll_cb_list);
	spin_unlock_irqrestore(&fence->poll_cb_lock, flags);

	return 0;
}

static bool iif_direct_remove_poll_cb(struct iif_fence *iif,
				      struct iif_manager_fence_ops_poll_cb *cb, void *driver_data)
{
	struct iif_direct_fence *fence = iif_fence_get_priv_fence_data(iif);
	unsigned long flags;
	bool removed = false;

	spin_lock_irqsave(&fence->poll_cb_lock, flags);

	if (!list_empty(&cb->node)) {
		list_del_init(&cb->node);
		removed = true;
	}

	spin_unlock_irqrestore(&fence->poll_cb_lock, flags);

	return removed;
}

const struct iif_manager_fence_ops iif_direct_fence_ops = {
	.sync_unit_name = iif_direct_sync_unit_name,
	.fence_create = iif_direct_fence_create,
	.fence_retire = iif_direct_fence_retire,
	.fence_add_sync_point = iif_direct_fence_add_sync_point,
	.fence_signal = iif_direct_fence_signal,
	.add_poll_cb = iif_direct_add_poll_cb,
	.remove_poll_cb = iif_direct_remove_poll_cb,
};

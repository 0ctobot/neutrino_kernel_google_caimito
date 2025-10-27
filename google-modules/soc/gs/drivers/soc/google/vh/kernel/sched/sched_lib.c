// SPDX-License-Identifier: GPL-2.0-only
/* core.c
 *
 * Android Vendor Hook Support
 *
 * Copyright 2021 Google LLC
 */

#include <linux/maple_tree.h>
#include <linux/sched.h>
#include <linux/sched/cputime.h>
#include <kernel/sched/sched.h>

#include "sched_priv.h"

bool disable_sched_setaffinity;

extern unsigned int vendor_sched_priority_task_boost_value;
char priority_task_name[LIB_PATH_LENGTH];
DEFINE_SPINLOCK(priority_task_name_lock);

char prefer_idle_task_name[LIB_PATH_LENGTH];
DEFINE_SPINLOCK(prefer_idle_task_name_lock);

char boost_at_fork_task_name[LIB_PATH_LENGTH];
DEFINE_RAW_SPINLOCK(boost_at_fork_task_name_lock);
unsigned long vendor_sched_boost_at_fork_value = SCHED_CAPACITY_SCALE/2;

bool is_vcpu_task(struct task_struct *p)
{
	if (strstr(p->comm, "crosvm_vcpu"))
		return true;

	return false;
}

void rvh_sched_setaffinity_mod(void *data, struct task_struct *task,
				const struct cpumask *in_mask, int *res)
{
	bool block_affinity;
	int group;

	if (*res != 0)
		return;

	if (is_vcpu_task(task)) {
		__reset_task_affinity(task, NULL);
		*res = -EPERM;
		return;
	}

	if (capable(CAP_SYS_NICE))
		return;

	group = get_vendor_group(task);

	block_affinity = disable_sched_setaffinity;
	block_affinity |= vg[group].disable_sched_setaffinity;

	if (block_affinity) {
		__reset_task_affinity(task, NULL);
		*res = -EPERM;
		return;
	}

	if (vg[group].disable_sched_setaffinity_mask)
		__reset_task_affinity(task, in_mask);
}

/*
 * boost uclamp.min of priority task to above LC capacity
 */
static inline void boost_priority_task(struct task_struct *p)
{
	struct rq *rq = task_rq(p);
	struct rq_flags rf;

	rq_lock_irqsave(rq, &rf);
	uclamp_rq_dec_id(task_rq(p), p, UCLAMP_MIN);
	uclamp_se_set(&p->uclamp_req[UCLAMP_MIN], vendor_sched_priority_task_boost_value, true);
	uclamp_rq_inc_id(task_rq(p), p, UCLAMP_MIN);
	rq_unlock_irqrestore(rq, &rf);
}

void vh_set_task_comm_pixel_mod(void *data, struct task_struct *p)
{
	char tmp[LIB_PATH_LENGTH];
	char *tok, *str;
	unsigned long flags;

	spin_lock_irqsave(&priority_task_name_lock, flags);
	strlcpy(tmp, priority_task_name, LIB_PATH_LENGTH);
	spin_unlock_irqrestore(&priority_task_name_lock, flags);
	str = tmp;

	if (*tmp != '\0') {
		while (1) {
			tok = strsep(&str, ",");

			if (tok == NULL)
				break;

			if (strstr(p->comm, tok) != NULL) {
				boost_priority_task(p);
				break;
			}
		}
	}
}

int set_prefer_idle_task_name(void)
{
	char tmp[LIB_PATH_LENGTH];
	char *tok, *str;
	struct task_struct *p, *t;
	int ret = -1;

	spin_lock(&prefer_idle_task_name_lock);
	strlcpy(tmp, prefer_idle_task_name, LIB_PATH_LENGTH);
	spin_unlock(&prefer_idle_task_name_lock);

	if (*tmp != '\0') {
		str = tmp;

		while (1) {
			tok = strsep(&str, ",");

			if (tok == NULL)
				break;

			rcu_read_lock();
			for_each_process_thread(p, t) {
				if (strstr(t->comm, tok) != NULL) {
					set_bit(SCHED_QOS_PREFER_IDLE_BIT,
					  &get_vendor_task_struct(t)->sched_qos_user_defined_flag);
					ret = 0;
					break;
				}
			}
			rcu_read_unlock();
		}
	}

	return ret;
}

bool should_boost_at_fork(struct task_struct *p)
{
	int group = get_vendor_group(p);
	unsigned long irqflags;
	bool boost = false;

	raw_spin_lock_irqsave(&boost_at_fork_task_name_lock, irqflags);
	if (strlen(boost_at_fork_task_name) &&
	    strstr(p->parent->comm, boost_at_fork_task_name) &&
	    (group == VG_FOREGROUND || group == VG_TOPAPP))
		boost = true;
	raw_spin_unlock_irqrestore(&boost_at_fork_task_name_lock, irqflags);

	return boost;
}

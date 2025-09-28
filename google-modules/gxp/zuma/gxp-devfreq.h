/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Devfreq driver for GXP.
 *
 * Copyright (C) 2024 Google LLC
 */

#ifndef __GXP_DEVFREQ_H__
#define __GXP_DEVFREQ_H__

#include <linux/mutex.h>
#include <linux/workqueue.h>

#include <gcip/gcip-devfreq.h>

#include "gxp-internal.h"

struct gxp_devfreq_freq_limit_work_data {
	struct work_struct work;
	struct gxp_dev *gxp;
	u32 min_freq_khz;
	u32 max_freq_khz;
	/*
	 * For provisioning the consistent update to the {min,max}_freq_khz during the race between
	 * the worker thread and the thread calling the schedule_work().
	 */
	struct mutex freq_limit_work_data_mutex;
};

struct gxp_devfreq {
	struct gcip_devfreq *devfreq;
	struct gxp_devfreq_freq_limit_work_data freq_limit_work_data;
};

/**
 * gxp_devfreq_init() - API to initialize devfreq for the device. Should be called once per probe.
 *
 * @gxp: The GXP device to operate
 *
 * Return:
 * * 0       - Initialization finished successfully.
 * * -EEXIST - devfreq is already initialized.
 * * Error codes propagated by gcip_devfreq_create() on failure.
 */
int gxp_devfreq_init(struct gxp_dev *gxp);

/**
 * gxp_devfreq_exit() - API for removing devfreq for the device.
 *
 * @gxp: The GXP device to operate.
 */
void gxp_devfreq_exit(struct gxp_dev *gxp);

#endif /* __GXP_DEVFREQ_H__ */

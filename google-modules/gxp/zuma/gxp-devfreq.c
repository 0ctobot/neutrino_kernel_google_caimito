// SPDX-License-Identifier: GPL-2.0-only
/*
 * Devfreq driver for GXP.
 *
 * Copyright (C) 2024 Google LLC
 */

#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/units.h>
#include <linux/workqueue.h>

#include "gxp-config.h"
#include "gxp-devfreq.h"
#include "gxp-pm.h"

static u32 gxp_devfreq_get_freq_table(void *data, u32 *dvfs_table, u32 max_size)
{
	struct gxp_dev *gxp = (struct gxp_dev *)data;
	u32 i, j, distinct_freq = 0;

	for (i = 0; i < AUR_NUM_POWER_STATE; i++) {
		/* Ignore the null frequency value. */
		if (!aur_power_state2rate[i])
			continue;

		/* Check if frequency value already exist in dvfs_table[]. */
		for (j = 0; j < distinct_freq; j++) {
			if (dvfs_table[j] == aur_power_state2rate[i])
				break;
		}
		if (j != distinct_freq)
			continue;

		/*
		 * Return 0 in case available frequencies exceed the maximum frequencies that GCIP
		 * devfreq interface supports. On getting hit with this error consider increasing
		 * the maximum supported frequencies in the GCIP devfreq interface.
		 */
		if (distinct_freq == max_size) {
			dev_err(gxp->dev,
				"Number of distinct frequencies greater than max limit of %u.\n",
				max_size);
			return 0;
		}

		dvfs_table[distinct_freq] = aur_power_state2rate[i];
		distinct_freq++;
	}

	return distinct_freq;
}

static void gxp_devfreq_update_min_max_freq_range(void *data, u32 min_freq_khz, u32 max_freq_khz)
{
	struct gxp_dev *gxp = (struct gxp_dev *)data;
	struct gxp_devfreq_freq_limit_work_data *work_data = &gxp->devfreq->freq_limit_work_data;

	/*
	 * This function executes inside the global linux devfreq framework lock that ensures that
	 * at a given time only a single devfreq request is processed.
	 *
	 * In scenario when the worker is already in the worker queue and new devfreq request comes
	 * then simply the latest {min,max}_freq_khz would get updated and be acted upon by the
	 * worker thread whenever it is scheduled next.
	 *
	 * In race condition when the work is already getting executed and the new devfreq request
	 * comes then mutex lock ensures the consistent update of the {min,max}_freq_khz followed
	 * by the rescheduling of the work to act upon the latest {min,max}_freq_khz request.
	 */
	mutex_lock(&work_data->freq_limit_work_data_mutex);
	work_data->min_freq_khz = min_freq_khz;
	work_data->max_freq_khz = max_freq_khz;
	mutex_unlock(&work_data->freq_limit_work_data_mutex);

	schedule_work(&work_data->work);
}

static int gxp_devfreq_get_cur_freq(struct device *dev, unsigned long *freq_hz)
{
	struct gxp_dev *gxp = dev_get_drvdata(dev);

	if (!gcip_pm_get_if_powered(gxp->power_mgr->pm, false)) {
		*freq_hz = (unsigned long)HZ_PER_KHZ * gxp_pm_blk_get_rate(gxp);
		gcip_pm_put(gxp->power_mgr->pm);
	} else {
		*freq_hz = 0;
	}

	return 0;
}

static void gxp_freq_limit_aync_work(struct work_struct *work)
{
	struct gxp_devfreq_freq_limit_work_data *data =
		container_of(work, struct gxp_devfreq_freq_limit_work_data, work);
	struct gxp_dev *gxp = data->gxp;
	int min_freq_khz, max_freq_khz, ret;

	mutex_lock(&data->freq_limit_work_data_mutex);
	min_freq_khz = data->min_freq_khz;
	max_freq_khz = data->max_freq_khz;
	mutex_unlock(&data->freq_limit_work_data_mutex);

	ret = gxp_pm_set_min_max_freq_limit(gxp, min_freq_khz, max_freq_khz);
	if (ret)
		dev_warn(gxp->dev, "Failed to set [%u, %u] kHz range with error %d.", min_freq_khz,
			 max_freq_khz, ret);
}

static const struct gcip_devfreq_ops devfreq_ops = {
	.get_freq_table = gxp_devfreq_get_freq_table,
	.update_min_max_freq_range = gxp_devfreq_update_min_max_freq_range,
	.get_cur_freq = gxp_devfreq_get_cur_freq,
};

int gxp_devfreq_init(struct gxp_dev *gxp)
{
	struct gxp_devfreq *gdevfreq;
	int ret;

	const struct gcip_devfreq_args args = {
		.dev = gxp->dev,
		.data = gxp,
		.ops = &devfreq_ops,
	};

	if (gxp->devfreq)
		return -EEXIST;

	gdevfreq = devm_kzalloc(gxp->dev, sizeof(*gdevfreq), GFP_KERNEL);
	if (!gdevfreq)
		return -ENOMEM;

	gdevfreq->devfreq = gcip_devfreq_create(&args);

	if (IS_ERR(gdevfreq->devfreq)) {
		ret = PTR_ERR(gdevfreq->devfreq);
		dev_err(gxp->dev, "Failed to initialize devfreq:%d.", ret);
		goto err;
	}

	gdevfreq->freq_limit_work_data.gxp = gxp;
	gdevfreq->freq_limit_work_data.min_freq_khz = 0;
	gdevfreq->freq_limit_work_data.max_freq_khz = 0;
	INIT_WORK(&gdevfreq->freq_limit_work_data.work, gxp_freq_limit_aync_work);
	mutex_init(&gdevfreq->freq_limit_work_data.freq_limit_work_data_mutex);

	gxp->devfreq = gdevfreq;
	return 0;
err:
	devm_kfree(gxp->dev, gdevfreq);
	return ret;
}

void gxp_devfreq_exit(struct gxp_dev *gxp)
{
	if (!gxp->devfreq)
		return;
	cancel_work_sync(&gxp->devfreq->freq_limit_work_data.work);
	gcip_devfreq_destroy(gxp->devfreq->devfreq);
	devm_kfree(gxp->dev, gxp->devfreq);
	gxp->devfreq = NULL;
}

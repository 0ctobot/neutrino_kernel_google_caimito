// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020 Google LLC.
 *
 * Author: Sidath Senanayake <sidaths@google.com>
 */

/* Linux includes */
#ifdef CONFIG_OF
#include <linux/of.h>
#endif
#include <trace/events/power.h>

/* SOC includes */
#if IS_ENABLED(CONFIG_CAL_IF)
#include <soc/google/cal-if.h>
#endif

/* Mali core includes */
#include <mali_kbase.h>

/* Pixel integration includes */
#include "mali_kbase_config_platform.h"
#include "pixel_gpu_control.h"
#include "pixel_gpu_debug.h"
#include "pixel_gpu_dvfs.h"

#define DVFS_TABLE_ROW_MAX (8)
static struct gpu_dvfs_opp gpu_dvfs_table[DVFS_TABLE_ROW_MAX];

/* DVFS metrics code */

/**
 * gpu_dvfs_metrics_update() - Updates GPU metrics on level or power change.
 *
 * @kbdev:       The &struct kbase_device for the GPU.
 * @next_level:  The level that the GPU is about to move to. Can be the same as the current level.
 * @power_state: The current power state of the GPU. Can be the same as the current power state.
 *
 * This function should be called (1) right after a change in power state of the GPU, or (2) just
 * prior to changing the level of a powered on GPU. It will update the metrics for each of the GPU
 * DVFS level metrics and the power metrics as appropriate.
 *
 * Context: Expects the caller to hold the DVFS lock.
 */
void gpu_dvfs_metrics_update(struct kbase_device *kbdev, int next_level, bool power_state)
{
	struct pixel_context *pc = kbdev->platform_context;
	int level = pc->dvfs.level;

	const u64 prev = pc->dvfs.metrics_last_time;
	u64 curr = ktime_get_ns();

	lockdep_assert_held(&pc->dvfs.lock);

	if (pc->dvfs.metrics_last_power_state) {
		if (power_state) {
			/* Power state was ON and is not changing */
			if (level != next_level) {
				pc->dvfs.table[next_level].metrics.entry_count++;
				pc->dvfs.table[next_level].metrics.time_last_entry = curr;
			}
		} else {
			/* Power status was ON and is turning OFF */
			pc->pm.power_off_metrics.entry_count++;
			pc->pm.power_off_metrics.time_last_entry = curr;
		}

		pc->dvfs.table[level].metrics.time_total += (curr - prev);
		pc->pm.power_on_metrics.time_total += (curr - prev);

	} else {
		if (power_state) {
			/* Power state was OFF and is turning ON */
			pc->pm.power_on_metrics.entry_count++;
			pc->pm.power_on_metrics.time_last_entry = curr;

			if (pc->dvfs.metrics_last_level != next_level) {
				/* Level was changed while the GPU was powered off, and that change
				 * is being reflected now.
				 */
				pc->dvfs.table[next_level].metrics.entry_count++;
				pc->dvfs.table[next_level].metrics.time_last_entry = curr;
			}
		}

		pc->pm.power_off_metrics.time_total += (curr - prev);
	}

	pc->dvfs.metrics_last_power_state = power_state;
	pc->dvfs.metrics_last_time = curr;
	pc->dvfs.metrics_last_level = next_level;
}

/**
 * gpu_dvfs_metrics_init() - Initializes DVFS metrics.
 *
 * @kbdev: The &struct kbase_device for the GPU.
 *
 * Context: Process context. Takes and releases the DVFS lock.
 *
 * Return: This function currently always returns 0 for success.
 */
int gpu_dvfs_metrics_init(struct kbase_device *kbdev)
{
	struct pixel_context *pc = kbdev->platform_context;

	mutex_lock(&pc->dvfs.lock);

	pc->dvfs.metrics_last_time = ktime_get_ns();
	pc->dvfs.metrics_last_power_state = gpu_power_status(kbdev);

	pc->dvfs.table[pc->dvfs.level].metrics.entry_count++;
	pc->dvfs.table[pc->dvfs.level].metrics.time_last_entry =
		pc->dvfs.metrics_last_time;

	mutex_unlock(&pc->dvfs.lock);

	return 0;
}

/**
 * gpu_dvfs_metrics_term() - Terminates the Pixel GPU DVFS QOS subsystem.
 *
 * @kbdev: The &struct kbase_device for the GPU.
 *
 * Note that this function currently doesn't do anything.
 */
void gpu_dvfs_metrics_term(struct kbase_device *kbdev)
{
}

/* DVFS event handling code */

/**
 * gpu_dvfs_trace_clock() - Emits trace events corresponding to a change in GPU clocks.
 *
 * @kbdev:    The &struct kbase_device for the GPU.
 * @power_on: Whether the GPU is currently powered on or not.
 */
static void gpu_dvfs_trace_clock(struct kbase_device *kbdev, bool power_on)
{
	struct pixel_context *pc = kbdev->platform_context;
	int proc = raw_smp_processor_id();
	int gpu0 = 0;
	int gpu1 = 0;

	if (power_on) {
		gpu0 = cal_dfs_get_rate(pc->dvfs.gpu0_cal_id);
		gpu1 = cal_dfs_get_rate(pc->dvfs.gpu1_cal_id);
	}

	trace_clock_set_rate("gpu0", gpu0, proc);
	trace_clock_set_rate("gpu1", gpu1, proc);
}

/**
 * gpu_dvfs_set_new_level() - Updates the GPU operating point.
 *
 * @kbdev:      The &struct kbase_device for the GPU.
 * @next_level: The level to set the GPU to.
 *
 * Context: Process context. Takes and releases the GPU power domain lock. Expects the caller to
 *          hold the DVFS lock.
 */
static int gpu_dvfs_set_new_level(struct kbase_device *kbdev, int next_level)
{
	struct pixel_context *pc = kbdev->platform_context;

	lockdep_assert_held(&pc->dvfs.lock);

#ifdef CONFIG_MALI_PIXEL_GPU_QOS
	/* If we are clocking up, update QOS frequencies before GPU frequencies */
	if (next_level < pc->dvfs.level)
		gpu_dvfs_qos_set(kbdev, next_level);
#endif /* CONFIG_MALI_PIXEL_GPU_QOS */

	mutex_lock(&pc->pm.domain->access_lock);

	gpu_dvfs_metrics_update(kbdev, next_level, true);

	cal_dfs_set_rate(pc->dvfs.gpu0_cal_id, pc->dvfs.table[next_level].clk0);
	cal_dfs_set_rate(pc->dvfs.gpu1_cal_id, pc->dvfs.table[next_level].clk1);

	pc->dvfs.level = next_level;

	mutex_unlock(&pc->pm.domain->access_lock);

#ifdef CONFIG_MALI_PIXEL_GPU_QOS
	/* If we are clocking down, update QOS frequencies after GPU frequencies */
	if (next_level > pc->dvfs.level)
		gpu_dvfs_qos_set(kbdev, next_level);
#endif /* CONFIG_MALI_PIXEL_GPU_QOS */

	gpu_dvfs_trace_clock(kbdev, true);

	return 0;
}

/**
 * gpu_dvfs_update_level_locks() - Validates and enforces sysfs-set DVFS locks.
 *
 * @kbdev: The &struct kbase_device for the GPU.
 *
 * This function ensures that a recent update to the DVFS scaling locks are self consistent. If the
 * GPU is currently running at a level outside of the scaling range, the GPU's level is marked for
 * update at the next opportunity.
 *
 * Context: Process context. Expects the caller to hold the DVFS lock.
 */
void gpu_dvfs_update_level_locks(struct kbase_device *kbdev)
{
	struct pixel_context *pc = kbdev->platform_context;

	lockdep_assert_held(&pc->dvfs.lock);

	/* Validate that scaling frequencies are in the right order */
	if (pc->dvfs.level_scaling_max > pc->dvfs.level_scaling_min) {
		GPU_LOG(LOG_WARN, kbdev, "scaling frequencies are invalid");
		pc->dvfs.level_scaling_max = 0;
		pc->dvfs.level_scaling_min = pc->dvfs.table_size - 1;
	}

	/* Check if the current level needs to be adjusted */
	if (pc->dvfs.level < pc->dvfs.level_scaling_max)
		pc->dvfs.level_target = pc->dvfs.level_scaling_max;
	else if (pc->dvfs.level > pc->dvfs.level_scaling_min)
		pc->dvfs.level_target = pc->dvfs.level_scaling_min;

#ifdef CONFIG_MALI_PIXEL_GPU_THERMAL
	/* Check if a TMU limit needs to be applied */
	if (pc->dvfs.level < pc->dvfs.tmu.level_limit)
		pc->dvfs.level_target = pc->dvfs.tmu.level_limit;
#endif /* CONFIG_MALI_PIXEL_GPU_THERMAL */
}

/**
 * gpu_dvfs_event_power_on() - DVFS event handler for when the GPU powers on.
 *
 * @kbdev: The &struct kbase_device for the GPU.
 *
 * This function updates GPU metrics and outputs trace events to track the change in power status.
 *
 * Context: Process context. Takes and releases the DVFS lock
 */
void gpu_dvfs_event_power_on(struct kbase_device *kbdev)
{
	struct pixel_context *pc = kbdev->platform_context;

	mutex_lock(&pc->dvfs.lock);
	gpu_dvfs_metrics_update(kbdev, pc->dvfs.level, true);
	mutex_unlock(&pc->dvfs.lock);

	gpu_dvfs_trace_clock(kbdev, true);
}

/**
 * gpu_dvfs_event_power_off() - DVFS event handler for when the GPU powers off.
 *
 * @kbdev: The &struct kbase_device for the GPU.
 *
 * This function updates GPU metrics and outputs trace events to track the change in power status.
 *
 * Context: Process context. Takes and releases the DVFS lock.
 */
void gpu_dvfs_event_power_off(struct kbase_device *kbdev)
{
	struct pixel_context *pc = kbdev->platform_context;

	mutex_lock(&pc->dvfs.lock);
	gpu_dvfs_metrics_update(kbdev, pc->dvfs.level, false);
	mutex_unlock(&pc->dvfs.lock);

	gpu_dvfs_trace_clock(kbdev, false);
}

/**
 * gpu_dvfs_worker() - The main DVFS entry point for the Pixel GPU integration.
 *
 * @data: Worker data structure, used to determine the corresponding GPU context.
 *
 * This function handles the processing of incoming GPU utilization data from the core Mali driver
 * that was passed via &kbase_platform_dvfs_event.
 *
 * If the GPU is powered on, the reported utilization is used to determine whether a level change is
 * required via the current governor and if so, make that change.
 *
 * If the GPU is powered off, this function determines whether the GPU should be clocked down
 * depending on how long the gpu has been powered off for.
 *
 * Context: Process context. Takes and releases the DVFS lock.
 */
static void gpu_dvfs_worker(struct work_struct *data)
{
	struct pixel_context *pc = container_of(data, struct pixel_context, dvfs.work);
	struct kbase_device *kbdev = pc->kbdev;
	int util;

	mutex_lock(&pc->dvfs.lock);

	util = atomic_read(&pc->dvfs.util);

	if (gpu_power_status(kbdev)) {
		pc->dvfs.clock_down_delay = pc->dvfs.clock_down_hysteresis;
		pc->dvfs.level_target = gpu_dvfs_governor_get_next_level(kbdev, util);

#ifdef CONFIG_MALI_PIXEL_GPU_QOS
		/* If we have reset our QOS requests due to the GPU going idle, and haven't
		 * changed level, we need to request the QOS values for that level again
		 */
		if (pc->dvfs.level_target == pc->dvfs.level && !pc->dvfs.qos.enabled)
			gpu_dvfs_qos_set(kbdev, pc->dvfs.level_target);
#endif /* CONFIG_MALI_PIXEL_GPU_QOS */

		if (pc->dvfs.level_target != pc->dvfs.level) {
			GPU_LOG(LOG_DEBUG, kbdev, "util=%d results in level change (%d->%d)\n",
				util, pc->dvfs.level, pc->dvfs.level_target);
			gpu_dvfs_set_new_level(kbdev, pc->dvfs.level_target);
		}

	} else if (pc->dvfs.clock_down_delay) {
		if (--pc->dvfs.clock_down_delay == 0) {
			pc->dvfs.level_target = pc->dvfs.level_scaling_min;
#ifdef CONFIG_MALI_PIXEL_GPU_QOS
			gpu_dvfs_qos_reset(kbdev);
#endif /* CONFIG_MALI_PIXEL_GPU_QOS */
		}
	}

	mutex_unlock(&pc->dvfs.lock);

	GPU_LOG(LOG_DEBUG, kbdev, "dvfs worker is called\n");
}

/**
 * kbase_platform_dvfs_event() - Callback from Mali driver to report updated utilization metrics.
 *
 * @kbdev:         The &struct kbase_device for the GPU.
 * @utilisation:   The calculated utilization as measured by the core Mali driver's metrics system.
 * @util_gl_share: The calculated GL share of utilization. Currently unused.
 * @util_cl_share: The calculated CL share of utilization per core group. Currently unused.
 *
 * This is the function that bridges the core Mali driver and the Pixel integration code. As this is
 * made in interrupt context, it is swiftly handed off to a work_queue for further processing.
 *
 * Context: Interrupt context.
 *
 * Return: Returns 1 to signal success as specified in mali_kbase_pm_internal.h.
 */
int kbase_platform_dvfs_event(struct kbase_device *kbdev, u32 utilisation,
	u32 util_gl_share, u32 util_cl_share[2])
{
	struct pixel_context *pc = kbdev->platform_context;

	atomic_set(&pc->dvfs.util, utilisation);
	queue_work(pc->dvfs.wq, &pc->dvfs.work);

	return 1;
}

/* Initialization code */

/**
 * find_voltage_for_freq() - Retrieves voltage for a frequency from ECT.
 *
 * @clock:      The frequency to search for.
 * @vol:        A pointer into which the voltage, if found, will be written.
 * @arr:        The &struct dvfs_rate_volt array to search through.
 * @arr_length: The size of @arr.
 *
 * Return: Returns 0 on success, -ENOENT if @clock doesn't exist in ECT.
 */
static int find_voltage_for_freq(unsigned int clock, unsigned int *vol,
	struct dvfs_rate_volt *arr, unsigned int arr_length)
{
	int i;

	for (i = 0; i < arr_length; i++) {
		if (arr[i].rate == clock) {
			*vol = arr[i].volt;
			return 0;
		}
	}

	return -ENOENT;
}

/**
 * gpu_dvfs_update_asv_table() - Populate the GPU's DVFS table from DT.
 *
 * @kbdev: The &struct kbase_device for the GPU.
 *
 * This function reads data out of the GPU's device tree entry and uses it to populate
 * &gpu_dvfs_table. For each entry in the DVFS table, it makes calls to determine voltages from ECT.
 *
 * This function will fail if the required data is not present in the GPU's device tree entry.
 *
 * Return: Returns the size of the DVFS table on success, -EINVAL on failure.
 */
static int gpu_dvfs_update_asv_table(struct kbase_device *kbdev)
{
	struct pixel_context *pc = kbdev->platform_context;
	struct device_node *np = kbdev->dev->of_node;

	int i, idx;

	int of_data_int_array[OF_DATA_NUM_MAX];
	int dvfs_table_row_num = 0, dvfs_table_col_num = 0;
	int dvfs_table_size = 0;

	struct dvfs_rate_volt gpu0_vf_map[16];
	struct dvfs_rate_volt gpu1_vf_map[16];
	int gpu0_level_count, gpu1_level_count;

	/* Get frequency -> voltage mapping */
	gpu0_level_count = cal_dfs_get_lv_num(pc->dvfs.gpu0_cal_id);
	gpu1_level_count = cal_dfs_get_lv_num(pc->dvfs.gpu1_cal_id);

	if (!cal_dfs_get_rate_asv_table(pc->dvfs.gpu0_cal_id, gpu0_vf_map)) {
		GPU_LOG(LOG_ERROR, kbdev, "failed to get gpu0 ASV table\n");
		goto err;
	}

	if (!cal_dfs_get_rate_asv_table(pc->dvfs.gpu1_cal_id, gpu1_vf_map)) {
		GPU_LOG(LOG_ERROR, kbdev, "failed to get gpu1 ASV table\n");
		goto err;
	}

	/* Get size of DVFS table data from device tree */
	if (of_property_read_u32_array(np, "gpu_dvfs_table_size", of_data_int_array, 2))
		goto err;

	dvfs_table_row_num = of_data_int_array[0];
	dvfs_table_col_num = of_data_int_array[1];
	dvfs_table_size = dvfs_table_row_num * dvfs_table_col_num;

	if (dvfs_table_row_num > DVFS_TABLE_ROW_MAX) {
		GPU_LOG(LOG_ERROR, kbdev,
			"DVFS table has %d rows but only up to %d are supported\n",
			dvfs_table_row_num, DVFS_TABLE_ROW_MAX);
		goto err;
	}

	if (dvfs_table_size > OF_DATA_NUM_MAX) {
		GPU_LOG(LOG_ERROR, kbdev, "DVFS table is too big\n");
		goto err;
	}

	of_property_read_u32_array(np, "gpu_dvfs_table", of_data_int_array, dvfs_table_size);

	/* Process DVFS table data from device tree and store it in OPP table */
	for (i = 0; i < dvfs_table_row_num; i++) {
		idx = i * dvfs_table_col_num;

		/* Read raw data from device tree table */
		gpu_dvfs_table[i].clk0         = of_data_int_array[idx + 0];
		gpu_dvfs_table[i].clk1         = of_data_int_array[idx + 1];
		gpu_dvfs_table[i].util_min     = of_data_int_array[idx + 2];
		gpu_dvfs_table[i].util_max     = of_data_int_array[idx + 3];
		gpu_dvfs_table[i].hysteresis   = of_data_int_array[idx + 4];
		gpu_dvfs_table[i].qos.int_min  = of_data_int_array[idx + 5];
		gpu_dvfs_table[i].qos.mif_min  = of_data_int_array[idx + 6];
		gpu_dvfs_table[i].qos.cpu0_min = of_data_int_array[idx + 7];
		gpu_dvfs_table[i].qos.cpu1_min = of_data_int_array[idx + 8];
		gpu_dvfs_table[i].qos.cpu2_max = of_data_int_array[idx + 9];

		/* Handle case where CPU cluster 2 has no limit set */
		if (!gpu_dvfs_table[i].qos.cpu2_max)
			gpu_dvfs_table[i].qos.cpu2_max = CPU_FREQ_MAX;

		/* Get and validate voltages from cal-if */
		if (find_voltage_for_freq(gpu_dvfs_table[i].clk0,
				&(gpu_dvfs_table[i].vol0),
				gpu0_vf_map, gpu0_level_count))
			goto err;

		if (find_voltage_for_freq(gpu_dvfs_table[i].clk1,
				&(gpu_dvfs_table[i].vol1),
				gpu1_vf_map, gpu1_level_count))
			goto err;
	}

	return dvfs_table_row_num;

err:
	GPU_LOG(LOG_ERROR, kbdev, "failed to set GPU ASV table\n");
	return -EINVAL;
}

/**
 * gpu_dvfs_get_initial_level() - Determine the boot DVFS level from cal-if
 *
 * @kbdev: The &struct kbase_device for the GPU
 *
 * This function searches through the DVFS table until it finds the lowest throughput level that
 * matches the boot clocks for the two GPU clock domains.
 *
 * Return: The level corresponding to the boot state, -EINVAL if it doesn't exist.
 */
static int gpu_dvfs_get_initial_level(struct kbase_device *kbdev)
{
	int level;

	struct pixel_context *pc = kbdev->platform_context;
	int clk0 = cal_dfs_get_boot_freq(pc->dvfs.gpu0_cal_id);
	int clk1 = cal_dfs_get_boot_freq(pc->dvfs.gpu1_cal_id);

	for (level = pc->dvfs.table_size - 1; level >= 0; level--)
		if (pc->dvfs.table[level].clk0 == clk0 && pc->dvfs.table[level].clk1 == clk1)
			break;

	if (level < 0) {
		GPU_LOG(LOG_ERROR, kbdev,
			"boot OPP pair (gpu0: %d, gpu1: %d) not present in DVFS table\n",
			clk0, clk1);
		return -EINVAL;
	}

	return level;
}

/**
 * gpu_dvfs_init() - Initializes the Pixel GPU DVFS system.
 *
 * @kbdev: The &struct kbase_device for the GPU.
 *
 * Depending on the compile time options set, this function calls initializers for the subsystems
 * related to GPU DVFS: governors, metrics, qos & tmu.
 *
 * Return: On success, returns 0. -EINVAL on error.
 */
int gpu_dvfs_init(struct kbase_device *kbdev)
{
	int ret = 0;
	struct pixel_context *pc = kbdev->platform_context;
	struct device_node *np = kbdev->dev->of_node;

	/* Initialize lock */
	mutex_init(&pc->dvfs.lock);

	/* Get data from DT */
	if (of_property_read_u32(np, "gpu0_cmu_cal_id", &pc->dvfs.gpu0_cal_id) ||
		of_property_read_u32(np, "gpu1_cmu_cal_id", &pc->dvfs.gpu1_cal_id)) {
		ret = -EINVAL;
		goto done;
	}

	/* Get the ASV table */
	pc->dvfs.table_size = gpu_dvfs_update_asv_table(kbdev);
	if (pc->dvfs.table_size < 0) {
		ret = -EINVAL;
		goto done;
	}

	pc->dvfs.table = gpu_dvfs_table;
	pc->dvfs.level_max = 0;
	pc->dvfs.level_min = pc->dvfs.table_size - 1;
	pc->dvfs.level_scaling_max = pc->dvfs.level_max;
	pc->dvfs.level_scaling_min = pc->dvfs.level_min;
#ifdef CONFIG_MALI_PIXEL_GPU_THERMAL
	pc->dvfs.tmu.level_limit = pc->dvfs.level_max;
#endif /* CONFIG_MALI_PIXEL_GPU_THERMAL */

	/* Determine initial state */
	pc->dvfs.level_start = gpu_dvfs_get_initial_level(kbdev);
	if (pc->dvfs.level_start < 0) {
		ret = -EINVAL;
		goto done;
	}

	pc->dvfs.level = pc->dvfs.level_start;
	pc->dvfs.level_target = pc->dvfs.level_start;

	/* Initialize power down hysteresis */
	if (of_property_read_u32(np, "gpu_dvfs_clock_down_hysteresis",
		&pc->dvfs.clock_down_hysteresis)) {
		GPU_LOG(LOG_ERROR, kbdev, "DVFS clock down hysteresis not set in DT");
		ret = -EINVAL;
		goto done;
	}

	pc->dvfs.clock_down_delay = pc->dvfs.clock_down_hysteresis;

	/* Initialize DVFS governors */
	ret = gpu_dvfs_governor_init(kbdev);
	if (ret) {
		GPU_LOG(LOG_ERROR, kbdev, "DVFS governor init failed\n");
		goto done;
	}

	/* Initialize DVFS metrics */
	ret = gpu_dvfs_metrics_init(kbdev);
	if (ret) {
		GPU_LOG(LOG_ERROR, kbdev, "DVFS metrics init failed\n");
		goto fail_metrics_init;
	}

#ifdef CONFIG_MALI_PIXEL_GPU_QOS
	/* Initialize QOS framework */
	ret = gpu_dvfs_qos_init(kbdev);
	if (ret) {
		GPU_LOG(LOG_ERROR, kbdev, "DVFS QOS init failed\n");
		goto fail_qos_init;
	}
#endif /* CONFIG_MALI_PIXEL_GPU_QOS */

#ifdef CONFIG_MALI_PIXEL_GPU_THERMAL
	/* Initialize thermal framework */
	ret = gpu_tmu_init(kbdev);
	if (ret) {
		GPU_LOG(LOG_ERROR, kbdev, "DVFS thermal init failed\n");
		goto fail_tmu_init;
	}
#endif /* CONFIG_MALI_PIXEL_GPU_THERMAL */

	/* Initialize workqueue */
	pc->dvfs.wq = create_singlethread_workqueue("gpu-dvfs");
	INIT_WORK(&pc->dvfs.work, gpu_dvfs_worker);

	/* Initialization was successful */
	goto done;

#ifdef CONFIG_MALI_PIXEL_GPU_THERMAL
fail_tmu_init:
#ifdef CONFIG_MALI_PIXEL_GPU_QOS
	gpu_dvfs_qos_term(kbdev);
#endif /* CONFIG_MALI_PIXEL_GPU_QOS */
#endif /* CONFIG_MALI_PIXEL_GPU_THERMAL*/

#ifdef CONFIG_MALI_PIXEL_GPU_QOS
fail_qos_init:
#endif /* CONFIG_MALI_PIXEL_GPU_QOS */
	gpu_dvfs_metrics_term(kbdev);

fail_metrics_init:
	gpu_dvfs_governor_term(kbdev);

done:
	return ret;
}

/**
 * gpu_dvfs_term() - Terminates the Pixel GPU DVFS system.
 *
 * @kbdev: The &struct kbase_device for the GPU.
 */
void gpu_dvfs_term(struct kbase_device *kbdev)
{
	struct pixel_context *pc = kbdev->platform_context;

	destroy_workqueue(pc->dvfs.wq);

#ifdef CONFIG_MALI_PIXEL_GPU_THERMAL
	gpu_tmu_term(kbdev);
#endif /* CONFIG_MALI_PIXEL_GPU_THERMAL */
#ifdef CONFIG_MALI_PIXEL_GPU_QOS
	gpu_dvfs_qos_term(kbdev);
#endif /* CONFIG_MALI_PIXEL_GPU_QOS */
	gpu_dvfs_metrics_term(kbdev);
	gpu_dvfs_governor_term(kbdev);
}

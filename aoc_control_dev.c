// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2020 Google LLC. All Rights Reserved.
 *
 * Interface to the AoC control service
 */

#define pr_fmt(fmt) "aoc_control: " fmt

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "aoc.h"
#include "aoc-interface.h"

#define AOC_CONTROL_NAME "aoc_stats"
#define AOC_SERVICE_NAME "control"

struct aoc_stat {
	char name[8];
	u8 type;
};

struct stats_prvdata {
	/* Exclusive access to the channel for attribute transactions */
	struct mutex lock;
	struct work_struct discovery_work;
	struct aoc_service_dev *service;
	struct aoc_stat *discovered_stats;
	int total_stats;
};

/* Driver methods */

/*
 * Convenience method to send a write/read combination to the AoC.
 * Returns # of bytes returned, or negative codes for errors.
 */
static ssize_t read_attribute(struct stats_prvdata *prvdata, void *in_cmd,
			      size_t in_size, void *out_cmd, size_t out_size)
{
	struct aoc_service_dev *service = prvdata->service;
	ssize_t ret;

	ret = mutex_lock_interruptible(&prvdata->lock);
	if (ret != 0)
		return ret;

	ret = aoc_service_write(service, in_cmd, in_size, true);
	if (ret < 0)
		goto out;

	ret = aoc_service_read(service, out_cmd, out_size, true);

out:
	mutex_unlock(&prvdata->lock);
	return ret;
}

static const char * const service_names[] = {
	AOC_SERVICE_NAME,
	NULL,
};

static ssize_t read_core_build_info(int core, struct device *dev, char *buf)
{
	struct stats_prvdata *prvdata = dev_get_drvdata(dev);
	struct CMD_SYS_VERSION_GET version_get = { 0 };
	int ret;

	AocCmdHdrSet(&version_get.parent.parent, CMD_SYS_VERSION_GET_ID,
		     sizeof(version_get));

	version_get.parent.core = core;

	ret = read_attribute(prvdata, &version_get, sizeof(version_get),
			     &version_get, sizeof(version_get));

	if (ret < 0)
		return ret;

	if (strnlen(version_get.version, sizeof(version_get.version)) ==
			sizeof(version_get.version))
		dev_err(dev, "invalid version string returned\n");

	if (strnlen(version_get.link_time, sizeof(version_get.link_time)) ==
			sizeof(version_get.link_time))
		dev_err(dev, "invalid link time string returned\n");

	ret = scnprintf(buf, PAGE_SIZE, "Build Hash: %.64s\nLink Time: %.64s\n",
			version_get.version, version_get.link_time);

	return ret;
}

static ssize_t a32_build_info_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return read_core_build_info(1, dev, buf);
}

static DEVICE_ATTR_RO(a32_build_info);

static ssize_t f1_build_info_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return read_core_build_info(2, dev, buf);
}

static DEVICE_ATTR_RO(f1_build_info);

static ssize_t hf_build_info_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return read_core_build_info(3, dev, buf);
}

static DEVICE_ATTR_RO(hf_build_info);

static ssize_t read_timed_stat(struct device *dev, char *buf, int index)
{
	struct stats_prvdata *prvdata = dev_get_drvdata(dev);
	struct CMD_SYS_STATS_TIMED_GET get_stat = { 0 };
	int ret;

	AocCmdHdrSet(&get_stat.parent.parent, CMD_SYS_STATS_TIMED_GET_ID,
		     sizeof(get_stat));
	get_stat.index = index;

	ret = read_attribute(prvdata, &get_stat, sizeof(get_stat), &get_stat,
			     sizeof(get_stat));

	/* Partial response */
	if (ret >= 0 && ret < sizeof(get_stat))
		ret = -EIO;

	if (ret < 0)
		goto out;

	ret = scnprintf(buf, PAGE_SIZE,
		"Counter: %llu\nCumulative time: %llu\nTime last entered: %llu\nTime last exited: %llu\n",
		le64_to_cpu(get_stat.timed.counter),
		le64_to_cpu(get_stat.timed.cumulative_time),
		le64_to_cpu(get_stat.timed.timestamp_enter_last),
		le64_to_cpu(get_stat.timed.timestamp_exit_last));

out:
	return ret;
}

static ssize_t read_data_stat(struct device *dev, char *buf, int index)
{
	struct stats_prvdata *prvdata = dev_get_drvdata(dev);
	struct CMD_SYS_STATS_DATA_GET get_stat = { 0 };
	int ret;

	AocCmdHdrSet(&get_stat.parent.parent, CMD_SYS_STATS_DATA_GET_ID,
		     sizeof(get_stat));
	get_stat.index = index;

	ret = read_attribute(prvdata, &get_stat, sizeof(get_stat), &get_stat,
			     sizeof(get_stat));

	/* Partial response */
	if (ret >= 0 && ret < sizeof(get_stat))
		ret = -EIO;

	if (ret < 0)
		goto out;

	ret = scnprintf(buf, PAGE_SIZE,
			"Counter: %llu\nFailed: %llu\nTx: %llu\nRx: %llu\n",
			le64_to_cpu(get_stat.transfer.counter),
			le64_to_cpu(get_stat.transfer.counter_failed),
			le64_to_cpu(get_stat.transfer.transfer_tx),
			le64_to_cpu(get_stat.transfer.transfer_rx));

out:
	return ret;
}

static ssize_t read_stat_by_name(struct device *dev, char *buf,
				 const char *name)
{
	struct stats_prvdata *prvdata = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < prvdata->total_stats; i++) {
		if (strcmp(name, prvdata->discovered_stats[i].name))
			continue;

		switch (prvdata->discovered_stats[i].type) {
		case 0:
			dev_dbg(dev, "Reading timed stat %d (%s)\n", i, name);
			return read_timed_stat(dev, buf, i);
		case 1:
			dev_dbg(dev, "Reading transfer stat %d (%s)\n", i,
				name);
			return read_data_stat(dev, buf, i);
		default:
			dev_err(dev, "Unknown stats type %d\n",
				prvdata->discovered_stats[i].type);
			return 0;
		}
	}

	dev_err(dev, "failed to find stat with name %s\n", name);
	return 0;
}

#define DECLARE_STAT(stat_name, sysfs_name)                                    \
	static ssize_t sysfs_name##_show(                                      \
		struct device *dev, struct device_attribute *attr, char *buf)  \
	{                                                                      \
		return read_stat_by_name(dev, buf, stat_name);                 \
	}                                                                      \
	static DEVICE_ATTR_RO(sysfs_name)

DECLARE_STAT("A3-WFI", a32_wfi);
DECLARE_STAT("A3-RET", a32_retention);
DECLARE_STAT("A3-DWN", a32_off);
DECLARE_STAT("F1-WFI", ff1_wfi);
DECLARE_STAT("F1-RET", ff1_retention);
DECLARE_STAT("F1-DWN", ff1_off);
DECLARE_STAT("H0-WFI", hf0_wfi);
DECLARE_STAT("H0-RET", hf0_retention);
DECLARE_STAT("H0-DWN", hf0_off);
DECLARE_STAT("H1-WFI", hf1_wfi);
DECLARE_STAT("H1-RET", hf1_retention);
DECLARE_STAT("H1-DWN", hf1_off);
DECLARE_STAT("AC-MON", monitor_mode);
DECLARE_STAT("I2C0", i2c0);
DECLARE_STAT("I2C1", i2c1);
DECLARE_STAT("I2C2", i2c2);
DECLARE_STAT("I2C3", i2c3);
DECLARE_STAT("I2C4", i2c4);
DECLARE_STAT("SPI0", spi0);
DECLARE_STAT("SPI1", spi1);
DECLARE_STAT("I3C0", i3c0);
DECLARE_STAT("PDM", pdm);
DECLARE_STAT("PDM-SW", pdm_16khz);
DECLARE_STAT("TDM", tdm);
DECLARE_STAT("MIF_UP", mif_up);
DECLARE_STAT("SLC_UP", slc_up);
DECLARE_STAT("V_NOM", voltage_nominal);
DECLARE_STAT("V_UD", voltage_underdrive);
DECLARE_STAT("V_SUD", voltage_super_underdrive);
DECLARE_STAT("V_UUD", voltage_ultra_underdrive);

static struct attribute *aoc_stats_attrs[] = {
	&dev_attr_a32_build_info.attr,
	&dev_attr_f1_build_info.attr,
	&dev_attr_hf_build_info.attr,
	&dev_attr_a32_wfi.attr,
	&dev_attr_a32_retention.attr,
	&dev_attr_a32_off.attr,
	&dev_attr_ff1_wfi.attr,
	&dev_attr_ff1_retention.attr,
	&dev_attr_ff1_off.attr,
	&dev_attr_hf0_wfi.attr,
	&dev_attr_hf0_retention.attr,
	&dev_attr_hf0_off.attr,
	&dev_attr_hf1_wfi.attr,
	&dev_attr_hf1_retention.attr,
	&dev_attr_hf1_off.attr,
	&dev_attr_monitor_mode.attr,
	&dev_attr_i2c0.attr,
	&dev_attr_i2c1.attr,
	&dev_attr_i2c2.attr,
	&dev_attr_i2c3.attr,
	&dev_attr_i2c4.attr,
	&dev_attr_spi0.attr,
	&dev_attr_spi1.attr,
	&dev_attr_i3c0.attr,
	&dev_attr_pdm.attr,
	&dev_attr_pdm_16khz.attr,
	&dev_attr_tdm.attr,
	&dev_attr_mif_up.attr,
	&dev_attr_slc_up.attr,
	&dev_attr_voltage_nominal.attr,
	&dev_attr_voltage_underdrive.attr,
	&dev_attr_voltage_super_underdrive.attr,
	&dev_attr_voltage_ultra_underdrive.attr,
	NULL
};

ATTRIBUTE_GROUPS(aoc_stats);

static void discovery_workitem(struct work_struct *work)
{
	struct stats_prvdata *prvdata =
		container_of(work, struct stats_prvdata, discovery_work);
	struct CMD_SYS_STATS_TOT cmd_total;
	struct device *dev = &prvdata->service->dev;
	struct aoc_stat *stats;
	int i, ret;

	dev_dbg(dev, "started the discovery thread\n");

	AocCmdHdrSet(&cmd_total.parent.parent, CMD_SYS_STATS_TOT_ID,
		     sizeof(cmd_total));

	ret = read_attribute(prvdata, &cmd_total, sizeof(cmd_total), &cmd_total,
			     sizeof(cmd_total));

	if (ret < sizeof(cmd_total)) {
		dev_err(dev, "failed to read the number of statistics\n");
		goto out;
	}

	if (cmd_total.tot > 256 || cmd_total.tot < 0) {
		dev_err(dev, "invalid number of statistics %d\n",
			cmd_total.tot);
		goto out;
	}

	stats = devm_kcalloc(dev, cmd_total.tot, sizeof(struct aoc_stat),
			     GFP_KERNEL | __GFP_ZERO);
	if (!stats)
		goto out;

	prvdata->total_stats = cmd_total.tot;
	dev_dbg(dev, "Returned %d, %d stats", ret, prvdata->total_stats);

	for (i = 0; i < prvdata->total_stats; i++) {
		struct CMD_SYS_STATS_INFO_GET cmd_info;

		memset(&cmd_info, 0, sizeof(cmd_info));
		AocCmdHdrSet(&cmd_info.parent.parent, CMD_SYS_STATS_INFO_GET_ID,
			     sizeof(cmd_info));
		cmd_info.index = i;

		ret = read_attribute(prvdata, &cmd_info, sizeof(cmd_info),
				     &cmd_info, sizeof(cmd_info));
		if (ret == sizeof(cmd_info)) {
			stats[i].type = cmd_info.info.type;
			memcpy(stats[i].name, cmd_info.info.name,
			       STATS_ENTRY_LEN);
			dev_dbg(dev, "stat %d name %s type %d\n", i,
				stats[i].name, stats[i].type);
		} else {
			dev_err(dev, "failed to read stat %d info: %d\n", i,
				ret);
		}
	}

	prvdata->discovered_stats = stats;
	device_add_groups(&prvdata->service->dev, aoc_stats_groups);

out:
	do_exit(0);
}

static int aoc_control_probe(struct aoc_service_dev *sd)
{
	struct device *dev = &sd->dev;
	struct stats_prvdata *prvdata;

	pr_debug("probe service with name %s\n", dev_name(dev));

	prvdata = devm_kzalloc(dev, sizeof(*prvdata), GFP_KERNEL);
	prvdata->service = sd;
	mutex_init(&prvdata->lock);

	INIT_WORK(&prvdata->discovery_work, discovery_workitem);
	dev_set_drvdata(dev, prvdata);

	schedule_work(&prvdata->discovery_work);

	return 0;
}

static int aoc_control_remove(struct aoc_service_dev *sd)
{
	struct device *dev = &sd->dev;
	struct stats_prvdata *prvdata = dev_get_drvdata(dev);

	pr_debug("remove service with name %s\n", dev_name(dev));

	device_remove_groups(dev, aoc_stats_groups);

	cancel_work_sync(&prvdata->discovery_work);

	return 0;
}

static struct aoc_driver aoc_control_driver = {
	.drv = {
			.name = AOC_CONTROL_NAME,
		},
	.service_names = service_names,
	.probe = aoc_control_probe,
	.remove = aoc_control_remove,
};

module_aoc_driver(aoc_control_driver);

MODULE_LICENSE("GPL v2");

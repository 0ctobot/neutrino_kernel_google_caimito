/*
 * Copyright 2018 Google, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#ifdef CONFIG_PM_SLEEP
#define SUPPORT_PM_SLEEP 1
#endif


#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/usb/pd.h>
#include <linux/usb/tcpm.h>
#include <linux/alarmtimer.h>
#include "gbms_power_supply.h"
#include "pmic-voter.h" /* TODO(b/163679860): use gvotables */
#include "google_bms.h"
#include "google_dc_pps.h"
#include "google_psy.h"
#include "logbuffer.h"

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#endif

/* 200 * 250 = 50 seconds of logging */
#define CHG_LOG_PSY_RATELIMIT_CNT	200
#define CHG_DELAY_INIT_MS		250
#define CHG_DELAY_INIT_DETECT_MS	1000

#define DEFAULT_CHARGE_STOP_LEVEL 100
#define DEFAULT_CHARGE_START_LEVEL 0

#define CHG_DRV_EAGAIN_RETRIES	3
#define CHG_WORK_ERROR_RETRY_MS 1000


#define CHG_DRV_CC_HW_TOLERANCE_MAX	250

#define CHG_DRV_MODE_NOIRDROP	1
#define CHG_DRV_MODE_DISABLED	2
#define CHG_DRV_MODE_NOOP	3

#define DRV_DEFAULTCC_UPDATE_INTERVAL	30000
#define DRV_DEFAULTCV_UPDATE_INTERVAL	2000

#define MAX_VOTER			"MAX_VOTER"
#define THERMAL_DAEMON_VOTER		"THERMAL_DAEMON_VOTER"
#define USER_VOTER			"USER_VOTER"	/* same as QCOM */
#define MSC_CHG_VOTER			"msc_chg"
#define MSC_CHG_FULL_VOTER		"msc_chg_full"
#define MSC_USER_VOTER			"msc_user"
#define MSC_USER_CHG_LEVEL_VOTER	"msc_user_chg_level"
#define MSC_CHG_TERM_VOTER		"msc_chg_term"

#define CHG_TERM_LONG_DELAY_MS		300000	/* 5 min */
#define CHG_TERM_SHORT_DELAY_MS		60000	/* 1 min */
#define CHG_TERM_RETRY_MS		2000	/* 2 sec */
#define CHG_TERM_RETRY_CNT		5

#define FCC_OF_CDEV_NAME "google,charger"
#define FCC_CDEV_NAME "fcc"
#define WLC_OF_CDEV_NAME "google,wlc_charger"
#define WLC_CDEV_NAME "dc_icl"

#define PPS_CC_TOLERANCE_PCT_DEFAULT	5
#define PPS_CC_TOLERANCE_PCT_MAX	10

#define get_boot_sec() div_u64(ktime_to_ns(ktime_get_boottime()), NSEC_PER_SEC)

struct chg_drv;

enum chg_thermal_devices {
	CHG_TERMAL_DEVICES_COUNT = 2,
	CHG_TERMAL_DEVICE_FCC = 0,
	CHG_TERMAL_DEVICE_DC_IN = 1,
};

struct chg_thermal_device {
	struct chg_drv *chg_drv;

	struct thermal_cooling_device *tcd;
	int *thermal_mitigation;
	int thermal_levels;
	int current_level;
};

struct chg_termination {
	bool enable;
	bool alarm_start;
	struct work_struct work;
	struct alarm alarm;
	int cc_full_ref;
	int retry_cnt;
	int usb_5v;
};

/* re-evaluate FCC when switching power supplies */
static int chg_therm_update_fcc(struct chg_drv *chg_drv);
static void chg_reset_termination_data(struct chg_drv *chg_drv);
static int chg_vote_input_suspend(struct chg_drv *chg_drv,
				  char *voter, bool suspend);

struct chg_drv {
	struct device *device;

	struct power_supply *chg_psy;
	const char *chg_psy_name;
	struct power_supply *wlc_psy;
	const char *wlc_psy_name;
	struct power_supply *bat_psy;
	const char *bat_psy_name;
	struct power_supply *tcpm_psy;
	const char *tcpm_psy_name;
	int log_psy_ratelimit;

	struct notifier_block psy_nb;
	struct delayed_work init_work;
	struct delayed_work chg_work;
	struct work_struct chg_psy_work;
	struct wakeup_source *chg_ws;
	struct alarm chg_wakeup_alarm;
	u32 tcpm_phandle;

	struct power_supply *usb_psy;
	const char *usb_psy_name;
	bool usb_skip_probe;

	/* */
	struct chg_thermal_device thermal_devices[CHG_TERMAL_DEVICES_COUNT];
	bool therm_wlc_override_fcc;

	/* */
	u32 cv_update_interval;
	u32 cc_update_interval;
	union gbms_ce_adapter_details adapter_details;

	struct votable	*msc_interval_votable;
	struct votable	*msc_fv_votable;
	struct votable	*msc_fcc_votable;
	struct votable	*msc_chg_disable_votable;
	struct votable	*msc_pwr_disable_votable;
	struct votable	*usb_icl_votable;
	struct votable	*dc_suspend_votable;
	struct votable	*dc_icl_votable;

	bool batt_present;
	bool dead_battery;
	int batt_profile_fcc_ua;	/* max/default fcc */
	int batt_profile_fv_uv;		/* max/default fv_uv */
	int fv_uv;
	int cc_max;
	int chg_cc_tolerance;
	int chg_mode;			/* debug */
	int stop_charging;		/* no power source */
	int egain_retries;

	/* retail */
	int disable_charging;		/* from retail */
	int disable_pwrsrc;		/* from retail */
	bool lowerdb_reached;		/* user charge level */
	int charge_stop_level;		/* user charge level */
	int charge_start_level;		/* user charge level */

	/* pps charging */
	struct pd_pps_data pps_data;
	unsigned int pps_cc_tolerance_pct;
	union gbms_charger_state chg_state;

	/* override voltage and current */
	bool enable_user_fcc_fv;
	int user_fv_uv;
	int user_cc_max;
	int user_interval;

	/* prevent overcharge */
	struct chg_termination	chg_term;

	/* debug */
	struct dentry *debug_entry;
};

static void reschedule_chg_work(struct chg_drv *chg_drv)
{
	cancel_delayed_work_sync(&chg_drv->chg_work);
	schedule_delayed_work(&chg_drv->chg_work, 0);
}

static enum alarmtimer_restart
google_chg_alarm_handler(struct alarm *alarm, ktime_t time)
{
	struct chg_drv *chg_drv =
	    container_of(alarm, struct chg_drv, chg_wakeup_alarm);

	__pm_stay_awake(chg_drv->chg_ws);

	schedule_delayed_work(&chg_drv->chg_work, 0);

	return ALARMTIMER_NORESTART;
}

static void chg_psy_work(struct work_struct *work)
{
	struct chg_drv *chg_drv =
		container_of(work, struct chg_drv, chg_psy_work);
	reschedule_chg_work(chg_drv);
}

/* cannot block: run in atomic context when called from chg_psy_changed() */
static int chg_psy_changed(struct notifier_block *nb,
		       unsigned long action, void *data)
{
	struct power_supply *psy = data;
	struct chg_drv *chg_drv = container_of(nb, struct chg_drv, psy_nb);

	pr_debug("name=%s evt=%lu\n", psy->desc->name, action);

	if ((action != PSY_EVENT_PROP_CHANGED) ||
	    (psy == NULL) || (psy->desc == NULL) || (psy->desc->name == NULL))
		return NOTIFY_OK;

	if (action == PSY_EVENT_PROP_CHANGED &&
	    (!strcmp(psy->desc->name, chg_drv->chg_psy_name) ||
	     !strcmp(psy->desc->name, chg_drv->bat_psy_name) ||
	     (chg_drv->usb_psy_name &&
	      !strcmp(psy->desc->name, chg_drv->usb_psy_name)) ||
	     (chg_drv->tcpm_psy_name &&
	      !strcmp(psy->desc->name, chg_drv->tcpm_psy_name)) ||
	     (chg_drv->wlc_psy_name &&
	      !strcmp(psy->desc->name, chg_drv->wlc_psy_name)))) {
		schedule_work(&chg_drv->chg_psy_work);
	}
	return NOTIFY_OK;
}

#if 0
static char *psy_usb_type_str[] = {
	"Unknown", "Battery", "UPS", "Mains", "USB",
	"USB_DCP", "USB_CDP", "USB_ACA", "USB_C",
	"USB_PD", "USB_PD_DRP", "BrickID",
	"USB_HVDCP", "USB_HVDCP_3", "Wireless", "USB_FLOAT",
	"BMS", "Parallel", "Main", "Wipower", "USB_C_UFP", "USB_C_DFP",
};
#endif

static char *psy_usb_type_str[] = {
	[POWER_SUPPLY_USB_TYPE_UNKNOWN] = "Unknown",
	[POWER_SUPPLY_USB_TYPE_SDP] = "USB",
	[POWER_SUPPLY_USB_TYPE_DCP] = "USB_DCP",
	[POWER_SUPPLY_USB_TYPE_CDP] = "USB_CDP",
	[POWER_SUPPLY_USB_TYPE_ACA] = "USB_ACA",
	[POWER_SUPPLY_USB_TYPE_C] =  "USB_C",
	[POWER_SUPPLY_USB_TYPE_PD] = "USB_PD",
	[POWER_SUPPLY_USB_TYPE_PD_DRP] = "USB_PD_DRP",
	[POWER_SUPPLY_USB_TYPE_PD_PPS] = "PPS",
	[POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID] = "BrickID",
};

static char *psy_usbc_type_str[] = {
	"Unknown", "SDP", "DCP", "CDP", "ACA", "C",
	"PD", "PD_DRP", "PD_PPS", "BrickID"
};

/* called on google_charger_init_work() and on every disconnect */
static inline void chg_init_state(struct chg_drv *chg_drv)
{
	/* reset retail state */
	chg_drv->disable_charging = -1;
	chg_drv->disable_pwrsrc = -1;
	chg_drv->lowerdb_reached = true;

	/* reset charging parameters */
	chg_drv->fv_uv = -1;
	chg_drv->cc_max = -1;
	chg_drv->chg_state.v = 0;
	vote(chg_drv->msc_fv_votable, MSC_CHG_VOTER, false, chg_drv->fv_uv);
	vote(chg_drv->msc_fcc_votable, MSC_CHG_VOTER, false, chg_drv->cc_max);
	chg_drv->egain_retries = 0;

	/* reset and re-enable PPS detection */
	pps_init_state(&chg_drv->pps_data);
	if (chg_drv->pps_data.nr_snk_pdo)
		chg_drv->pps_data.stage = PPS_NONE;
}

/* NOTE: doesn't reset chg_drv->adapter_details.v = 0 see chg_work() */
static inline void chg_reset_state(struct chg_drv *chg_drv)
{
	union gbms_charger_state chg_state = { .v = 0 };

	chg_init_state(chg_drv);

	if (chg_drv->chg_term.enable)
		chg_reset_termination_data(chg_drv);

	if (!pps_is_disabled(chg_drv->pps_data.stage)) {
		unsigned int nr_pdo = chg_drv->pps_data.default_pps_pdo ?
				      PDO_PPS : PDO_FIXED_HIGH_VOLTAGE;

		chg_update_capability(chg_drv->tcpm_psy, nr_pdo,
				      chg_drv->pps_data.default_pps_pdo);
	}

	if (chg_drv->chg_term.usb_5v == 1)
		chg_drv->chg_term.usb_5v = 0;

	/* TODO: handle interaction with PPS code */
	vote(chg_drv->msc_interval_votable, CHG_PPS_VOTER, false, 0);
	/* when/if enabled */
	GPSY_SET_PROP(chg_drv->chg_psy,
			POWER_SUPPLY_PROP_TAPER_CONTROL,
			POWER_SUPPLY_TAPER_CONTROL_OFF);
	/* make sure the battery knows that it's disconnected */
	GPSY_SET_INT64_PROP(chg_drv->bat_psy,
			POWER_SUPPLY_PROP_CHARGE_CHARGER_STATE,
			chg_state.v);
}

static int info_usb_ad_type(int usb_type, int usbc_type)
{
	switch (usb_type) {
	case POWER_SUPPLY_TYPE_USB:
		return CHG_EV_ADAPTER_TYPE_USB_SDP;
	case POWER_SUPPLY_TYPE_USB_CDP:
		return CHG_EV_ADAPTER_TYPE_USB_CDP;
	case POWER_SUPPLY_TYPE_USB_DCP:
		return CHG_EV_ADAPTER_TYPE_USB_DCP;
	case POWER_SUPPLY_TYPE_USB_PD:
		return (usbc_type == POWER_SUPPLY_USB_TYPE_PD_PPS) ?
			CHG_EV_ADAPTER_TYPE_USB_PD_PPS :
			CHG_EV_ADAPTER_TYPE_USB_PD;
	case POWER_SUPPLY_TYPE_USB_FLOAT:
		return CHG_EV_ADAPTER_TYPE_USB_FLOAT;
	default:
		return CHG_EV_ADAPTER_TYPE_USB;
	}
}

static int info_usb_state(union gbms_ce_adapter_details *ad,
			  struct power_supply *usb_psy,
			  struct power_supply *tcpm_psy)
{
	const char *usb_type_str = psy_usb_type_str[0];
	int usb_type, voltage_max = -1, amperage_max = -1;
	int usbc_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;

	if (usb_psy) {
		int voltage_now, current_now;

		/* TODO: handle POWER_SUPPLY_PROP_REAL_TYPE in qc-compat */
		usb_type = GPSY_GET_PROP(usb_psy, POWER_SUPPLY_PROP_USB_TYPE);
		if (tcpm_psy)
			usbc_type = GPSY_GET_PROP(tcpm_psy,
						  POWER_SUPPLY_PROP_USB_TYPE);

		voltage_max = GPSY_GET_PROP(usb_psy,
					    POWER_SUPPLY_PROP_VOLTAGE_MAX);
		amperage_max = GPSY_GET_PROP(usb_psy,
					     POWER_SUPPLY_PROP_CURRENT_MAX);
		voltage_now = GPSY_GET_PROP(usb_psy,
					    POWER_SUPPLY_PROP_VOLTAGE_NOW);
		current_now = GPSY_GET_PROP(usb_psy,
					    POWER_SUPPLY_PROP_INPUT_CURRENT_NOW);

		if (usb_type < ARRAY_SIZE(psy_usb_type_str))
			usb_type_str = psy_usb_type_str[usb_type];

		pr_info("usbchg=%s typec=%s usbv=%d usbc=%d usbMv=%d usbMc=%d\n",
			usb_type_str,
			tcpm_psy ? psy_usbc_type_str[usbc_type] : "null",
			voltage_now / 1000,
			current_now / 1000,
			voltage_max / 1000,
			amperage_max / 1000);
	}

	ad->ad_voltage = (voltage_max < 0) ? voltage_max
					   : voltage_max / 100000;
	ad->ad_amperage = (amperage_max < 0) ? amperage_max
					     : amperage_max / 100000;

	if (voltage_max < 0 || amperage_max < 0) {
		ad->ad_type = CHG_EV_ADAPTER_TYPE_UNKNOWN;
		return -EINVAL;
	}

	ad->ad_type = info_usb_ad_type(usb_type, usbc_type);

	return 0;
}

static int info_wlc_state(union gbms_ce_adapter_details *ad,
			  struct power_supply *wlc_psy)
{
	int voltage_max, amperage_max;

	voltage_max = GPSY_GET_PROP(wlc_psy, POWER_SUPPLY_PROP_VOLTAGE_MAX);
	amperage_max = GPSY_GET_PROP(wlc_psy, POWER_SUPPLY_PROP_CURRENT_MAX);

	pr_info("wlcv=%d wlcc=%d wlcMv=%d wlcMc=%d wlct=%d\n",
		GPSY_GET_PROP(wlc_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW) / 1000,
		GPSY_GET_PROP(wlc_psy, POWER_SUPPLY_PROP_CURRENT_NOW) / 1000,
		voltage_max / 1000,
		amperage_max / 1000,
		GPSY_GET_PROP(wlc_psy, POWER_SUPPLY_PROP_TEMP));

	if (voltage_max < 0 || amperage_max < 0) {
		ad->ad_type = CHG_EV_ADAPTER_TYPE_UNKNOWN;
		ad->ad_voltage = voltage_max;
		ad->ad_amperage = amperage_max;
		return -EINVAL;
	}

	if (amperage_max >= WLC_EPP_THRESHOLD_UV) {
		ad->ad_type = CHG_EV_ADAPTER_TYPE_WLC_EPP;
	} else if (amperage_max >= WLC_BPP_THRESHOLD_UV) {
		ad->ad_type = CHG_EV_ADAPTER_TYPE_WLC_SPP;
	}

	ad->ad_voltage = voltage_max / 100000;
	ad->ad_amperage = amperage_max / 100000;

	return 0;
}

/* NOTE: do not call this directly */
static int chg_set_charger(struct power_supply *chg_psy, int fv_uv, int cc_max)
{
	int rc;

	/* TAPER CONTROL is in the charger */
	rc = GPSY_SET_PROP(chg_psy,
		POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX, cc_max);
	if (rc != 0) {
		pr_err("MSC_CHG cannot set charging current rc=%d\n", rc);
		return -EIO;
	}

	rc = GPSY_SET_PROP(chg_psy, POWER_SUPPLY_PROP_VOLTAGE_MAX, fv_uv);
	if (rc != 0) {
		pr_err("MSC_CHG cannot set float voltage rc=%d\n", rc);
		return -EIO;
	}

	return rc;
}

static int chg_update_charger(struct chg_drv *chg_drv, int fv_uv, int cc_max)
{
	int rc = 0;
	struct power_supply *chg_psy = chg_drv->chg_psy;

	if (chg_drv->fv_uv != fv_uv || chg_drv->cc_max != cc_max) {
		int fcc = cc_max;

		/* when set cc_tolerance needs to be applied to everything */
		if (chg_drv->chg_cc_tolerance)
			fcc = (cc_max / 1000) *
			      (1000 - chg_drv->chg_cc_tolerance);

		/* TODO:
		 *   when cc_max < chg_drv->cc_max, set current, voltage
		 *   when cc_max > chg_drv->cc_max, set voltage, current
		 */
		rc = chg_set_charger(chg_psy, fv_uv, fcc);
		if (rc == 0) {
			pr_info("MSC_CHG fv_uv=%d->%d cc_max=%d->%d rc=%d\n",
				chg_drv->fv_uv, fv_uv,
				chg_drv->cc_max, cc_max,
				rc);

			chg_drv->cc_max = cc_max;
			chg_drv->fv_uv = fv_uv;
		}
	}

	return rc;
}

/* b/117985113 */
static int chg_usb_online(struct power_supply *usb_psy)
{
	int usb_online, mode = POWER_SUPPLY_TYPEC_SOURCE_DEFAULT;
	int rc = -EINVAL;

	if (!usb_psy)
		return 1;

#ifdef CONFIG_USB_ONLINE_IS_TYPEC_MODE
	mode = GPSY_GET_INT_PROP(usb_psy, POWER_SUPPLY_PROP_TYPEC_MODE, &rc);
	pr_info("TYPEC mode=%d rc=%d\n", mode, rc);
#else
	mode = GPSY_GET_INT_PROP(usb_psy, POWER_SUPPLY_PROP_ONLINE, &rc);
	if (mode)
		mode = POWER_SUPPLY_TYPEC_SOURCE_DEFAULT;
#endif
	switch (mode) {
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
	case POWER_SUPPLY_TYPEC_DAM_MEDIUM:
		usb_online = 1;
		break;
	default:
		usb_online = 0;
		break;
	}

	if (rc < 0)
		pr_info("online mode=%d rc=%d\n", usb_online, rc);

	return usb_online;
}
/* returns 1 if charging should be disabled given the current battery capacity
 * given in percent, return 0 if charging should happen
 */
static int chg_work_is_charging_disabled(struct chg_drv *chg_drv, int capacity)
{
	int disable_charging = 0;
	int upperbd = chg_drv->charge_stop_level;
	int lowerbd = chg_drv->charge_start_level;

	/* disabled */
	if ((upperbd == DEFAULT_CHARGE_STOP_LEVEL) &&
	    (lowerbd == DEFAULT_CHARGE_START_LEVEL))
		return 0;

	/* invalid */
	if ((upperbd < lowerbd) ||
	    (upperbd > DEFAULT_CHARGE_STOP_LEVEL) ||
	    (lowerbd < DEFAULT_CHARGE_START_LEVEL))
		return 0;

	if (chg_drv->lowerdb_reached && upperbd <= capacity) {
		pr_info("MSC_CHG lowerbd=%d, upperbd=%d, capacity=%d, lowerdb_reached=1->0, charging off\n",
			lowerbd, upperbd, capacity);
		disable_charging = 1;
		chg_drv->lowerdb_reached = false;
	} else if (!chg_drv->lowerdb_reached && lowerbd < capacity) {
		pr_info("MSC_CHG lowerbd=%d, upperbd=%d, capacity=%d, charging off\n",
			lowerbd, upperbd, capacity);
		disable_charging = 1;
	} else if (!chg_drv->lowerdb_reached && capacity <= lowerbd) {
		pr_info("MSC_CHG lowerbd=%d, upperbd=%d, capacity=%d, lowerdb_reached=0->1, charging on\n",
			lowerbd, upperbd, capacity);
		chg_drv->lowerdb_reached = true;
	} else {
		pr_info("MSC_CHG lowerbd=%d, upperbd=%d, capacity=%d, charging on\n",
			lowerbd, upperbd, capacity);
	}

	return disable_charging;
}

static void chg_termination_work(struct work_struct *work)
{
	struct chg_termination *chg_term =
			container_of(work, struct chg_termination, work);
	struct chg_drv *chg_drv =
			container_of(chg_term, struct chg_drv, chg_term);
	struct power_supply *bat_psy = chg_drv->bat_psy;
	int rc, cc, full, delay = CHG_TERM_LONG_DELAY_MS;

	cc = GPSY_GET_INT_PROP(bat_psy, POWER_SUPPLY_PROP_CHARGE_COUNTER, &rc);
	if (rc == -EAGAIN) {
		chg_term->retry_cnt++;

		if (chg_term->retry_cnt <= CHG_TERM_RETRY_CNT) {
			pr_info("Get CHARGE_COUNTER fail, try_cnt=%d, rc=%d\n",
				chg_term->retry_cnt, rc);
			/* try again and keep the pm_stay_awake */
			alarm_start_relative(&chg_term->alarm,
					     ms_to_ktime(CHG_TERM_RETRY_MS));
			return;
		} else {
			goto error;
		}
	} else if (rc < 0) {
		goto error;
	}

	/* Reset count if read successfully */
	chg_term->retry_cnt = 0;

	if (chg_term->cc_full_ref == 0)
		chg_term->cc_full_ref = cc;

	/*
	 * Suspend/Unsuspend USB input to keep cc_soc within the 0.5% to 0.75%
	 * overshoot range of the cc_soc value at termination, to prevent
	 * overcharging.
	 */
	full = chg_term->cc_full_ref;
	if ((long)cc < DIV_ROUND_CLOSEST((long)full * 10050, 10000)) {
		chg_vote_input_suspend(chg_drv, MSC_CHG_TERM_VOTER, false);
		delay = CHG_TERM_LONG_DELAY_MS;
	} else if ((long)cc > DIV_ROUND_CLOSEST((long)full * 10075, 10000)) {
		chg_vote_input_suspend(chg_drv, MSC_CHG_TERM_VOTER, true);
		delay = CHG_TERM_SHORT_DELAY_MS;
	}

	pr_info("Prevent overcharge data: cc: %d, cc_full_ref: %d, delay: %d\n",
		cc, chg_term->cc_full_ref, delay);

	alarm_start_relative(&chg_term->alarm, ms_to_ktime(delay));

	pm_relax(chg_drv->device);
	return;

error:
	pr_info("Get CHARGE_COUNTER fail, rc=%d\n", rc);
	chg_reset_termination_data(chg_drv);
}

static enum alarmtimer_restart chg_termination_alarm_cb(struct alarm *alarm,
							ktime_t now)
{
	struct chg_termination *chg_term =
			container_of(alarm, struct chg_termination, alarm);
	struct chg_drv *chg_drv =
			container_of(chg_term, struct chg_drv, chg_term);

	pr_info("Prevent overcharge alarm triggered %lld\n",
		ktime_to_ms(now));

	pm_stay_awake(chg_drv->device);
	schedule_work(&chg_term->work);

	return ALARMTIMER_NORESTART;
}

static void chg_reset_termination_data(struct chg_drv *chg_drv)
{
	if (!chg_drv->chg_term.alarm_start)
		return;

	chg_drv->chg_term.alarm_start = false;
	alarm_cancel(&chg_drv->chg_term.alarm);
	cancel_work_sync(&chg_drv->chg_term.work);
	chg_vote_input_suspend(chg_drv, MSC_CHG_TERM_VOTER, false);
	chg_drv->chg_term.cc_full_ref = 0;
	chg_drv->chg_term.retry_cnt = 0;
	pm_relax(chg_drv->device);
}

static void chg_eval_chg_termination(struct chg_termination *chg_term)
{
	if (chg_term->alarm_start)
		return;

	/*
	 * Post charge termination, switch to BSM mode triggers the risk of
	 * over charging as BATFET opening may take some time post the necessity
	 * of staying in supplemental mode, leading to unintended charging of
	 * battery. Trigger the function once charging is completed
	 * to prevent overcharing.
	 */
	alarm_start_relative(&chg_term->alarm,
			     ms_to_ktime(CHG_TERM_LONG_DELAY_MS));
	chg_term->alarm_start = true;
	chg_term->cc_full_ref = 0;
	chg_term->retry_cnt = 0;
}

static int chg_work_batt_roundtrip(const union gbms_charger_state *chg_state,
				   struct power_supply *bat_psy,
				   int *fv_uv, int *cc_max)
{
	int rc;

	rc = GPSY_SET_INT64_PROP(bat_psy,
				 POWER_SUPPLY_PROP_CHARGE_CHARGER_STATE,
				 chg_state->v);
	if (rc == -EAGAIN) {
		return -EAGAIN;
	} else if (rc < 0) {
		pr_err("MSC_CHG error cannot set CHARGE_CHARGER_STATE rc=%d\n",
		       rc);
		return -EINVAL;
	}

	/* battery can return negative values for cc_max and fv_uv. */
	*cc_max = GPSY_GET_INT_PROP(bat_psy,
				    POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
				    &rc);
	if (rc < 0) {
		pr_err("MSC_CHG error reading cc_max (%d)\n", rc);
		return -EIO;
	}

	/* ASSERT: (chg_state.f.flags&GBMS_CS_FLAG_DONE) && cc_max == 0 */

	*fv_uv = GPSY_GET_INT_PROP(bat_psy,
				   POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
				   &rc);
	if (rc < 0) {
		pr_err("MSC_CHG error reading fv_uv (%d)\n", rc);
		return -EIO;
	}

	return 0;
}

/* 0 stop charging, positive keep going */
static int chg_work_next_interval(const struct chg_drv *chg_drv,
				  union gbms_charger_state *chg_state)
{
	int update_interval = chg_drv->cc_update_interval;

	switch (chg_state->f.chg_status) {
	case POWER_SUPPLY_STATUS_FULL:
		update_interval = 0;
		break;
	case POWER_SUPPLY_STATUS_CHARGING:
		break;
	case POWER_SUPPLY_STATUS_NOT_CHARGING:
		update_interval = chg_drv->cv_update_interval;
		break;
	case POWER_SUPPLY_STATUS_DISCHARGING:
		/* DISCHARGING only when not connected -> stop charging */
		update_interval = 0;
		break;
	case POWER_SUPPLY_STATUS_UNKNOWN:
		update_interval = chg_drv->cv_update_interval;
		break;
	default:
		pr_err("invalid charging status %d\n", chg_state->f.chg_status);
		update_interval = chg_drv->cv_update_interval;
		break;
	}

	return update_interval;
}

static void chg_work_adapter_details(union gbms_ce_adapter_details *ad,
				     int usb_online, int wlc_online,
				     struct chg_drv *chg_drv)
{
	/* print adapter details, route after at the end */
	if (wlc_online)
		(void)info_wlc_state(ad, chg_drv->wlc_psy);
	if (usb_online)
		(void)info_usb_state(ad, chg_drv->usb_psy, chg_drv->tcpm_psy);
}

/* not executed when battery is NOT present */
static int chg_work_roundtrip(struct chg_drv *chg_drv,
			      union gbms_charger_state *chg_state)
{
	int fv_uv = -1, cc_max = -1, update_interval, rc;

	rc = gbms_read_charger_state(chg_state, chg_drv->chg_psy);
	if (rc < 0)
		return rc;

	/* NOIRDROP is default for remote sensing */
	if (chg_drv->chg_mode == CHG_DRV_MODE_NOIRDROP)
		chg_state->f.vchrg = 0;

	/* might return negative values in fv_uv and cc_max */
	rc = chg_work_batt_roundtrip(chg_state, chg_drv->bat_psy,
				     &fv_uv, &cc_max);
	if (rc < 0)
		return rc;

	/*
	 * on fv_uv < 0 (eg JEITA tripped) in the middle of charging keep
	 * charging voltage steady. fv_uv will get the max value (if set in
	 * device tree) if routtrip return a negative value on connect.
	 * Sanity check on current make sure that cc_max doesn't jump to max.
	 */
	if (fv_uv < 0)
		fv_uv = chg_drv->fv_uv;
	if  (cc_max < 0)
		cc_max = chg_drv->cc_max;

	/*
	 * battery has already voted on these with MSC_LOGIC
	 * TODO: could use fv_uv<0 to enable/disable a safe charge voltage
	 * TODO: could use cc_max<0 to enable/disable a safe charge current
	 */
	vote(chg_drv->msc_fv_votable, MSC_CHG_VOTER,
			(chg_drv->user_fv_uv == -1) && (fv_uv > 0), fv_uv);
	vote(chg_drv->msc_fcc_votable, MSC_CHG_VOTER,
			(chg_drv->user_cc_max == -1) && (cc_max >= 0), cc_max);

	/* update_interval <= 0 means stop charging */
	update_interval = chg_work_next_interval(chg_drv, chg_state);
	if (update_interval <= 0)
		return update_interval;

	/* no ping will cause timeout when pps_is_disabled() */
	if (chg_drv->tcpm_psy && !pps_is_disabled(chg_drv->pps_data.stage)) {
		int pps_ui;

		pps_ui = pps_work(&chg_drv->pps_data, chg_drv->tcpm_psy);
		if (pps_ui < 0)
			pps_ui = MSEC_PER_SEC;

		vote(chg_drv->msc_interval_votable,
			CHG_PPS_VOTER,
			(pps_ui != 0),
			pps_ui);
	}

	/*
	 * update_interval <= 0 means stop charging
	 * NOTE: pps code moved to msc_update_charger_cb()
	 */
	return update_interval;
}

/* true if still in dead battery */
#define DEAD_BATTERY_DEADLINE_SEC	(45 * 60)

static bool chg_update_dead_battery(const struct chg_drv *chg_drv)
{
	int dead = 0;
	const time_t uptime = get_boot_sec();

	if (uptime < DEAD_BATTERY_DEADLINE_SEC)
		dead = GPSY_GET_PROP(chg_drv->bat_psy,
				     POWER_SUPPLY_PROP_DEAD_BATTERY);
	if (dead == 0 && chg_drv->usb_psy) {
		dead = GPSY_SET_PROP(chg_drv->usb_psy,
				     POWER_SUPPLY_PROP_DEAD_BATTERY, 0);
		if (dead == 0)
			pr_info("dead battery cleared uptime=%ld\n", uptime);
	}

	return (dead != 0);
}

static int chg_work_read_soc(struct power_supply *bat_psy, int *soc)
{
	union power_supply_propval val;
	int ret = 0;

	ret = power_supply_get_property(bat_psy,
					POWER_SUPPLY_PROP_CAPACITY,
					&val);
	if (ret == 0)
		*soc = val.intval;

	return ret;
}

/* No op on battery not present */
static void chg_work(struct work_struct *work)
{
	struct chg_drv *chg_drv =
		container_of(work, struct chg_drv, chg_work.work);
	struct power_supply *usb_psy = chg_drv->usb_psy;
	struct power_supply *wlc_psy = chg_drv->wlc_psy;
	struct power_supply *bat_psy = chg_drv->bat_psy;
	union gbms_ce_adapter_details ad = { .v = 0 };
	int soc, disable_charging = 0, disable_pwrsrc = 0;
	int usb_online, wlc_online = 0;
	int update_interval = -1;
	bool chg_done = false;
	int success, rc = 0;

	__pm_stay_awake(chg_drv->chg_ws);
	pr_debug("battery charging work item\n");

	if (!chg_drv->batt_present) {
		/* -EGAIN = NOT ready, <0 don't know yet */
		rc = GPSY_GET_PROP(bat_psy, POWER_SUPPLY_PROP_PRESENT);
		if (rc < 0)
			goto rerun_error;

		chg_drv->batt_present = (rc > 0);
		if (!chg_drv->batt_present)
			goto exit_chg_work;

		pr_info("MSC_CHG battery present\n");
	}

	if (chg_drv->dead_battery)
		chg_drv->dead_battery = chg_update_dead_battery(chg_drv);

	if (chg_drv->chg_mode == CHG_DRV_MODE_DISABLED)
		goto exit_skip;

	/* cause msc_update_charger_cb to ignore updates */
	vote(chg_drv->msc_interval_votable, MSC_CHG_VOTER, true, 0);

	usb_online = chg_usb_online(usb_psy);
	if (wlc_psy)
		wlc_online = GPSY_GET_PROP(wlc_psy, POWER_SUPPLY_PROP_ONLINE);

	if (usb_online  < 0 || wlc_online < 0) {
		pr_err("MSC_CHG error reading usb=%d wlc=%d\n",
						usb_online, wlc_online);

		/* TODO: maybe disable charging when this happens? */
		goto rerun_error;
	} else if (!usb_online && !wlc_online) {

		if (chg_drv->stop_charging != 1) {
			pr_info("MSC_CHG no power source, disabling charging\n");

			vote(chg_drv->msc_chg_disable_votable,
			     MSC_CHG_VOTER, true, 0);

			chg_reset_state(chg_drv);
			chg_drv->stop_charging = 1;
		}

		goto exit_chg_work;
	} else if (chg_drv->stop_charging != 0) {
		/* will re-enable charging after setting FCC,CC_MAX */

		if (chg_drv->therm_wlc_override_fcc)
			(void)chg_therm_update_fcc(chg_drv);
	}

	/* retry for max CHG_DRV_EGAIN_RETRIES * CHG_WORK_ERROR_RETRY_MS on
	 * -EGAIN (race on wake). Retry is silent until we exceed the
	 * threshold, ->egain_retries is reset on every wakeup.
	 * NOTE: -EGAIN after this should be flagged
	 */
	rc = chg_work_read_soc(bat_psy, &soc);
	if (rc == -EAGAIN) {
		chg_drv->egain_retries += 1;
		if (chg_drv->egain_retries < CHG_DRV_EAGAIN_RETRIES)
			goto rerun_error;
	}

	if (rc < 0) {
		/* update_interval = -1, will reschedule */
		pr_err("MSC_CHG cannot get capacity (%d)\n", rc);
		goto update_charger;
	}

	chg_drv->egain_retries = 0;

	/* fast drain to the stop level */
	disable_charging = chg_work_is_charging_disabled(chg_drv, soc);
	if (disable_charging && soc > chg_drv->charge_stop_level)
		disable_pwrsrc = 1;
	else
		disable_pwrsrc = 0;

	/* disable charging is set in retail mode */
	if (disable_charging != chg_drv->disable_charging) {
		pr_info("MSC_CHG disable_charging %d -> %d",
			chg_drv->disable_charging, disable_charging);

		/* voted but not applied since msc_interval_votable <= 0 */
		vote(chg_drv->msc_fcc_votable,
		     MSC_USER_CHG_LEVEL_VOTER,
		     disable_charging != 0, 0);
	}
	chg_drv->disable_charging = disable_charging;

	/* when disable_pwrsrc is set, disable_charging is set also */
	if (disable_pwrsrc != chg_drv->disable_pwrsrc) {
		pr_info("MSC_CHG disable_pwrsrc %d -> %d",
			chg_drv->disable_pwrsrc, disable_pwrsrc);

		/* applied right away */
		vote(chg_drv->msc_pwr_disable_votable,
		     MSC_USER_CHG_LEVEL_VOTER,
		     disable_pwrsrc != 0, 0);
	}
	chg_drv->disable_pwrsrc = disable_pwrsrc;

	/* make sure ->fv_uv and ->cc_max are always correct */
	chg_work_adapter_details(&ad, usb_online, wlc_online, chg_drv);

	/* determine the next update interval */
	update_interval = chg_work_roundtrip(chg_drv, &chg_drv->chg_state);
	if (update_interval >= 0)
		chg_done = !!(chg_drv->chg_state.f.flags & GBMS_CS_FLAG_DONE);

	/* update_interval=0 when disconnected or on EOC (check for races)
	 * update_interval=-1 on an error (from roundtrip or reading soc)
	 * NOTE: might have cc_max==0 from the roundtrip on JEITA
	 */
update_charger:
	if (!disable_charging && update_interval > 0) {

		/* msc_update_charger_cb will write to charger and reschedule */
		vote(chg_drv->msc_interval_votable,
			MSC_CHG_VOTER, true,
			update_interval);

		if (chg_drv->stop_charging != 0) {
			pr_info("MSC_CHG power source usb=%d wlc=%d, enabling charging\n",
				usb_online, wlc_online);

			vote(chg_drv->msc_chg_disable_votable,
			     MSC_CHG_VOTER, false, 0);
			chg_drv->stop_charging = 0;
		}
	} else {
		int res;

		/* connected but needs to disable_charging */
		res = chg_update_charger(chg_drv, chg_drv->fv_uv, 0);
		if (res < 0)
			pr_err("MSC_CHG cannot update charger (%d)\n", res);
		if (res < 0 || rc < 0 || update_interval < 0)
			goto rerun_error;

	}

	/* tied to the charger: could tie to battery @ 100% instead */
	if ((chg_drv->chg_term.usb_5v == 0) && chg_done) {
		pr_info("MSC_CHG switch to 5V on full\n");
		chg_update_capability(chg_drv->tcpm_psy, PDO_FIXED_5V, 0);
		chg_drv->chg_term.usb_5v = 1;
	} else if (chg_drv->pps_data.stage == PPS_ACTIVE && chg_done) {
		pr_info("MSC_CHG switch to Fixed Profile on full\n");
		chg_drv->pps_data.stage = PPS_DISABLED;
		chg_update_capability(chg_drv->tcpm_psy, PDO_FIXED_HIGH_VOLTAGE,
				      0);
	}

	/* WAR: battery overcharge on a weak adapter */
	if (chg_drv->chg_term.enable && chg_done && (soc == 100))
		chg_eval_chg_termination(&chg_drv->chg_term);

	goto exit_chg_work;

rerun_error:
	success = schedule_delayed_work(&chg_drv->chg_work,
					CHG_WORK_ERROR_RETRY_MS);

	/* no need to reschedule the pending after an error
	 * NOTE: rc is the return code from battery properties
	 */
	if (rc != -EAGAIN)
		pr_err("MSC_CHG error rerun=%d in %d ms (%d)\n",
			success, CHG_WORK_ERROR_RETRY_MS, rc);

	/*
	 * If stay_awake is false, we are safe to ping the adapter
	 * NOTE: this probably needs to be changed to pps_keep_alive()
	 */
	if (!chg_drv->pps_data.stay_awake &&
	    chg_drv->pps_data.stage == PPS_ACTIVE)
		pps_ping(&chg_drv->pps_data, chg_drv->tcpm_psy);

	pr_debug("chg_work reschedule\n");
	return;

exit_chg_work:
	/* Route adapter details after the roundtrip since google_battery
	 * might overwrite the value when it starts a new cycle.
	 * NOTE: chg_reset_state() must not set chg_drv->adapter_details.v
	 * to zero. Fix the odd dependency when handling failure in setting
	 * POWER_SUPPLY_PROP_ADAPTER_DETAILS.
	 */
	if (rc == 0 && ad.v != chg_drv->adapter_details.v) {

		rc = GPSY_SET_PROP(chg_drv->bat_psy,
				   POWER_SUPPLY_PROP_ADAPTER_DETAILS,
				   (int)ad.v);

		/* TODO: handle failure rescheduling chg_work */
		if (rc < 0)
			pr_err("MSC_CHG no adapter details (%d)\n", rc);
		else
			chg_drv->adapter_details.v = ad.v;
	}

exit_skip:
	pr_debug("chg_work done\n");
	__pm_relax(chg_drv->chg_ws);
}

/* ------------------------------------------------------------------------ */

/* return negative when using ng charging */
static int chg_init_chg_profile(struct chg_drv *chg_drv)
{
	struct device *dev = chg_drv->device;
	struct device_node *node = dev->of_node;
	u32 temp;
	int ret;

	/* chg_work will use the minimum between all votess */
	ret = of_property_read_u32(node, "google,cv-update-interval",
				   &chg_drv->cv_update_interval);
	if (ret < 0 || chg_drv->cv_update_interval == 0)
		chg_drv->cv_update_interval = DRV_DEFAULTCV_UPDATE_INTERVAL;

	ret = of_property_read_u32(node, "google,cc-update-interval",
				   &chg_drv->cc_update_interval);
	if (ret < 0 || chg_drv->cc_update_interval == 0)
		chg_drv->cc_update_interval = DRV_DEFAULTCC_UPDATE_INTERVAL;

	/* when set will reduce cc_max by
	 * 	cc_max = cc_max * (1000 - chg_cc_tolerance) / 1000;
	 *
	 * this adds a "safety" margin for C rates if the charger doesn't do it.
	 */
	ret = of_property_read_u32(node, "google,chg-cc-tolerance", &temp);
	if (ret < 0)
		chg_drv->chg_cc_tolerance = 0;
	else if (temp > CHG_DRV_CC_HW_TOLERANCE_MAX)
		chg_drv->chg_cc_tolerance = CHG_DRV_CC_HW_TOLERANCE_MAX;
	else
		chg_drv->chg_cc_tolerance = temp;

	/* max charging current. This will be programmed to the charger when
	 * there is no charge table.
	 */
	ret = of_property_read_u32(node, "google,fcc-max-ua", &temp);
	if (ret < 0)
		chg_drv->batt_profile_fcc_ua = -EINVAL;
	else
		chg_drv->batt_profile_fcc_ua = temp;

	/* max and default charging voltage: this is what will be programmed to
	 * the charger when fv_uv is invalid.
	 */
	ret = of_property_read_u32(node, "google,fv-max-uv", &temp);
	if (ret < 0)
		chg_drv->batt_profile_fv_uv = -EINVAL;
	else
		chg_drv->batt_profile_fv_uv = temp;

	chg_drv->chg_term.enable =
		of_property_read_bool(node, "google,chg-termination-enable");

	/* fallback to 5V on charge termination */
	chg_drv->chg_term.usb_5v =
		of_property_read_bool(node, "google,chg-termination-5v");
	if (!chg_drv->chg_term.usb_5v) {
		chg_drv->chg_term.usb_5v = -1;
	} else {
		pr_info("renegotiate on full\n");
		chg_drv->chg_term.usb_5v = 0;
	}

	pr_info("charging profile in the battery\n");
	return 0;
}

static ssize_t show_charge_stop_level(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct chg_drv *chg_drv = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", chg_drv->charge_stop_level);
}

static ssize_t set_charge_stop_level(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct chg_drv *chg_drv = dev_get_drvdata(dev);
	int ret = 0, val;

	ret = kstrtoint(buf, 0, &val);
	if (ret < 0)
		return ret;

	if (!chg_drv->bat_psy) {
		pr_err("chg_drv->bat_psy is not ready");
		return -ENODATA;
	}

	if ((val == chg_drv->charge_stop_level) ||
	    (val <= chg_drv->charge_start_level) ||
	    (val > DEFAULT_CHARGE_STOP_LEVEL))
		return count;

	chg_drv->charge_stop_level = val;
	if (chg_drv->bat_psy)
		power_supply_changed(chg_drv->bat_psy);

	return count;
}

static DEVICE_ATTR(charge_stop_level, 0660, show_charge_stop_level,
					    set_charge_stop_level);

static ssize_t
show_charge_start_level(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct chg_drv *chg_drv = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", chg_drv->charge_start_level);
}

static ssize_t set_charge_start_level(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct chg_drv *chg_drv = dev_get_drvdata(dev);
	int ret = 0, val;

	ret = kstrtoint(buf, 0, &val);
	if (ret < 0)
		return ret;

	if (!chg_drv->bat_psy) {
		pr_err("chg_drv->bat_psy is not ready");
		return -ENODATA;
	}

	if ((val == chg_drv->charge_start_level) ||
	    (val >= chg_drv->charge_stop_level) ||
	    (val < DEFAULT_CHARGE_START_LEVEL))
		return count;

	chg_drv->charge_start_level = val;
	if (chg_drv->bat_psy)
		power_supply_changed(chg_drv->bat_psy);

	return count;
}

static DEVICE_ATTR(charge_start_level, 0660,
		   show_charge_start_level, set_charge_start_level);

/* TODO: now created in qcom code, create in chg_create_votables() */
static int chg_find_votables(struct chg_drv *chg_drv)
{
	if (!chg_drv->usb_icl_votable)
		chg_drv->usb_icl_votable = find_votable("USB_ICL");
	if (!chg_drv->dc_suspend_votable)
		chg_drv->dc_suspend_votable = find_votable("DC_SUSPEND");

	return (!chg_drv->usb_icl_votable || !chg_drv->dc_suspend_votable)
		? -EINVAL : 0;
}

/* input suspend votes 0 ICL and call suspend on DC_ICL */
static int chg_vote_input_suspend(struct chg_drv *chg_drv,
				  char *voter, bool suspend)
{
	int rc;

	if (chg_find_votables(chg_drv) < 0)
		return -EINVAL;

	rc = vote(chg_drv->usb_icl_votable, voter, suspend, 0);
	if (rc < 0) {
		dev_err(chg_drv->device, "Couldn't vote to %s USB rc=%d\n",
			suspend ? "suspend" : "resume", rc);
		return rc;
	}

	rc = vote(chg_drv->dc_suspend_votable, voter, suspend, 0);
	if (rc < 0) {
		dev_err(chg_drv->device, "Couldn't vote to %s DC rc=%d\n",
			suspend ? "suspend" : "resume", rc);
		return rc;
	}

	return 0;
}

#ifdef CONFIG_DEBUG_FS

static int chg_get_input_suspend(void *data, u64 *val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	if (chg_find_votables(chg_drv) < 0)
		return -EINVAL;

	*val = (get_client_vote(chg_drv->usb_icl_votable, USER_VOTER) == 0)
	       && get_client_vote(chg_drv->dc_suspend_votable, USER_VOTER);

	return 0;
}

static int chg_set_input_suspend(void *data, u64 val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;
	int rc;

	if (chg_find_votables(chg_drv) < 0)
		return -EINVAL;

	rc = chg_vote_input_suspend(chg_drv, USER_VOTER, val != 0);

	if (chg_drv->chg_psy)
		power_supply_changed(chg_drv->chg_psy);

	return rc;
}
DEFINE_SIMPLE_ATTRIBUTE(chg_is_fops, chg_get_input_suspend,
				     chg_set_input_suspend, "%llu\n");


static int chg_get_chg_suspend(void *data, u64 *val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	if (!chg_drv->msc_fcc_votable)
		return -EINVAL;

	/* can also set POWER_SUPPLY_PROP_CHARGE_DISABLE to charger */
	*val = get_client_vote(chg_drv->msc_fcc_votable, USER_VOTER) == 0;

	return 0;
}

static int chg_set_chg_suspend(void *data, u64 val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;
	int rc;

	if (!chg_drv->msc_fcc_votable)
		return -EINVAL;

	/* can also set POWER_SUPPLY_PROP_CHARGE_DISABLE to charger */
	rc = vote(chg_drv->msc_fcc_votable, USER_VOTER, val != 0, 0);
	if (rc < 0) {
		dev_err(chg_drv->device,
			"Couldn't vote %s to chg_suspend rc=%d\n",
			val ? "suspend" : "resume", rc);
		return rc;
	}

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(chg_cs_fops, chg_get_chg_suspend,
				     chg_set_chg_suspend, "%llu\n");


static int chg_get_update_interval(void *data, u64 *val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	if (!chg_drv->msc_interval_votable)
		return -EINVAL;

	/* can also set POWER_SUPPLY_PROP_CHARGE_DISABLE to charger */
	*val = get_client_vote(chg_drv->msc_interval_votable, USER_VOTER) == 0;

	return 0;
}

static int chg_set_update_interval(void *data, u64 val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;
	int rc;

	if (val < 0)
		return -ERANGE;
	if (!chg_drv->msc_interval_votable)
		return -EINVAL;

	/* can also set POWER_SUPPLY_PROP_CHARGE_DISABLE to charger */
	rc = vote(chg_drv->msc_interval_votable, USER_VOTER, val, 0);
	if (rc < 0) {
		dev_err(chg_drv->device,
			"Couldn't vote %d to update_interval rc=%d\n",
			val, rc);
		return rc;
	}

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(chg_ui_fops, chg_get_update_interval,
				     chg_set_update_interval, "%llu\n");


/* use qcom VS maxim fg and more... */
static int get_chg_mode(void *data, u64 *val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	*val = chg_drv->chg_mode;
	return 0;
}

static int set_chg_mode(void *data, u64 val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	chg_drv->chg_mode = val;
	reschedule_chg_work(chg_drv);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(chg_mode_fops, get_chg_mode, set_chg_mode, "%llu\n");

static int chg_get_fv_uv(void *data, u64 *val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	*val = chg_drv->user_fv_uv;
	return 0;
}

static int chg_set_fv_uv(void *data, u64 val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	if ((((int)val < -1)) || ((chg_drv->batt_profile_fv_uv > 0) &&
			   (val > chg_drv->batt_profile_fv_uv)))
		return -ERANGE;
	if (chg_drv->user_fv_uv == val)
		return 0;

	vote(chg_drv->msc_fv_votable, MSC_USER_VOTER, (val > 0), val);
	chg_drv->user_fv_uv = val;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(fv_uv_fops, chg_get_fv_uv,
				     chg_set_fv_uv, "%d\n");

static int chg_get_cc_max(void *data, u64 *val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	*val = chg_drv->user_cc_max;
	return 0;
}

static int chg_set_cc_max(void *data, u64 val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	if ((((int)val < -1)) || ((chg_drv->batt_profile_fcc_ua > 0) &&
			   (val > chg_drv->batt_profile_fcc_ua)))
		return -ERANGE;
	if (chg_drv->user_cc_max == val)
		return 0;

	vote(chg_drv->msc_fcc_votable, MSC_USER_VOTER, (val >= 0), val);
	chg_drv->user_cc_max = val;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(cc_max_fops, chg_get_cc_max,
				     chg_set_cc_max, "%d\n");


static int chg_get_interval(void *data, u64 *val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	*val = chg_drv->user_interval;
	return 0;
}

static int chg_set_interval(void *data, u64 val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	if (chg_drv->user_interval == val)
		return 0;

	vote(chg_drv->msc_interval_votable, MSC_USER_VOTER, (val >= 0), val);
	chg_drv->user_interval = val;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(chg_interval_fops,
				chg_get_interval,
				chg_set_interval, "%d\n");


static int chg_reschedule_work(void *data, u64 val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	reschedule_chg_work(chg_drv);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(chg_reschedule_work_fops,
					NULL, chg_reschedule_work, "%d\n");

#endif

static int debug_get_pps_cc_tolerance(void *data, u64 *val)
{
	struct chg_drv *chg_drv = data;

	*val = chg_drv->pps_cc_tolerance_pct;
	return 0;
}

static int debug_set_pps_cc_tolerance(void *data, u64 val)
{
	struct chg_drv *chg_drv = data;

	chg_drv->pps_cc_tolerance_pct = val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_pps_cc_tolerance_fops,
					debug_get_pps_cc_tolerance,
					debug_set_pps_cc_tolerance, "%u\n");



static int chg_init_fs(struct chg_drv *chg_drv)
{
	int ret;

	ret = device_create_file(chg_drv->device, &dev_attr_charge_stop_level);
	if (ret != 0) {
		pr_err("Failed to create charge_stop_level files, ret=%d\n",
		       ret);
		return ret;
	}

	ret = device_create_file(chg_drv->device, &dev_attr_charge_start_level);
	if (ret != 0) {
		pr_err("Failed to create charge_start_level files, ret=%d\n",
		       ret);
		return ret;
	}

	chg_drv->debug_entry = debugfs_create_dir("google_charger", 0);
	if (IS_ERR_OR_NULL(chg_drv->debug_entry)) {
		chg_drv->debug_entry = NULL;
		return 0;
	}

	debugfs_create_file("chg_mode", 0644, chg_drv->debug_entry,
			    chg_drv, &chg_mode_fops);
	debugfs_create_file("input_suspend", 0644, chg_drv->debug_entry,
			    chg_drv, &chg_is_fops);
	debugfs_create_file("chg_suspend", 0644, chg_drv->debug_entry,
			    chg_drv, &chg_cs_fops);
	debugfs_create_file("update_interval", 0644, chg_drv->debug_entry,
			    chg_drv, &chg_ui_fops);
	debugfs_create_file("force_reschedule", 0600, chg_drv->debug_entry,
			    chg_drv, &chg_reschedule_work_fops);

	debugfs_create_bool("usb_skip_probe", 0600, chg_drv->debug_entry,
			    &chg_drv->usb_skip_probe);

	debugfs_create_file("pps_cc_tolerance", 0600, chg_drv->debug_entry,
			    chg_drv, &debug_pps_cc_tolerance_fops);


	if (chg_drv->enable_user_fcc_fv) {
		debugfs_create_file("fv_uv", 0644, chg_drv->debug_entry,
					chg_drv, &fv_uv_fops);
		debugfs_create_file("cc_max", 0644,
					chg_drv->debug_entry,
					chg_drv, &cc_max_fops);
		debugfs_create_file("interval", 0644,
					chg_drv->debug_entry,
					chg_drv, &chg_interval_fops);
	}

	return 0;
}


/*
 * barebone initial pps policy.
 * Works only on output voltage keeping output current to the max.
 * Extend to hande CC
 */
static int pps_policy(struct chg_drv *chg_drv, int fv_uv, int cc_max,
		      struct power_supply *tcpm_psy,
		      struct power_supply *bat_psy)
{
	struct pd_pps_data *pps_data = &chg_drv->pps_data;
	const int ratio = 100 - chg_drv->pps_cc_tolerance_pct;
	const uint8_t flags = chg_drv->chg_state.f.flags;
	int ibatt, vbatt, ioerr;
	unsigned long exp_mw;
	int ret = 0;

	/* TODO: policy for negative/invalid targets? */
	if (cc_max <= 0) {
		logbuffer_log(pps_data->log, "negative cc_max=%d", cc_max);
		return 0;
	}

	/*
	 * TODO: Now we only need to adjust the pps in CC state.
	 * Consider CV state in the future.
	 */
	if (!(flags & GBMS_CS_FLAG_CC)) {
		logbuffer_log(pps_data->log, "waiting for CC flags=%x", flags);
		return 0;
	}

	ibatt = GPSY_GET_INT_PROP(bat_psy, POWER_SUPPLY_PROP_CURRENT_NOW,
				  &ioerr);
	vbatt = GPSY_GET_PROP(bat_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW);

	if (ioerr < 0 || vbatt < 0) {
		logbuffer_log(pps_data->log, "Failed to get ibatt (%d) or vbatt=%d",
			      ioerr, vbatt);
		return -EIO;
	}

	/* TODO: should we compensate for the round down here? */
	exp_mw = ((unsigned long)vbatt * (unsigned long)cc_max) * 11 /
		 10000000000;

	logbuffer_log(pps_data->log,
		"ibatt %d, vbatt %d, vbatt*cc_max*1.1 %lu mw, adapter %ld, keep_alive_cnt %d",
		ibatt, vbatt, exp_mw,
		(long)pps_data->out_uv * (long)pps_data->op_ua / 1000000000,
		pps_data->keep_alive_cnt);

	/* negative current is discharging, should chase the load instead? */
	if (ibatt <= 0)
		return 0;

	/* always maximize the input current first */
	/* TODO: b/134799977 adjust the max current */
	if (pps_data->op_ua < pps_data->max_ua) {
		pps_data->op_ua = pps_data->max_ua;
		return 0;
	}

	/* demand more power */
	if ((ibatt < (cc_max * ratio) / 100) || flags & GBMS_CS_FLAG_ILIM) {

		if (pps_data->out_uv == pps_data->max_uv) {
			ret = pps_switch_profile(pps_data, tcpm_psy, true);
			return (ret == 0) ? -ECANCELED : 0;
		}

		pps_adjust_volt(pps_data, 100000);

		/* TODO: b/134799977 adjust the max current */

	/* input power is enough */
	} else {
		/* everything is fine; try to lower the Vout if
		 * battery is satisfied for a period of time */
		if (pps_data->keep_alive_cnt < PPS_KEEP_ALIVE_MAX)
			return 0;

		ret = pps_switch_profile(pps_data, tcpm_psy, false);
		if (ret == 0)
			return -ECANCELED;

		pps_adjust_volt(pps_data, -100000);

		/* TODO: b/134799977 adjust the max current */

	}

	return ret;
}

/*
 * NOTE: when PPS is active we need to ping the adapter or it will revert
 * to fixed profile.
 */
static int msc_update_pps(struct chg_drv *chg_drv, int fv_uv, int cc_max)
{
	struct pd_pps_data *pps_data = &chg_drv->pps_data;
	int rc;

	/* the policy determines the adapter output voltage and current */
	rc = pps_policy(chg_drv, fv_uv, cc_max,
			chg_drv->tcpm_psy, chg_drv->bat_psy);
	if (rc == -ECANCELED)
		return -ECANCELED;

	/* adjust PPS out for target demand */
	rc = pps_update_adapter(pps_data, pps_data->out_uv, pps_data->op_ua,
				chg_drv->tcpm_psy);
	if (rc == -EAGAIN)
		rc = PPS_ERROR_RETRY_MS;

	return rc;
}

/*
 * NOTE: chg_work() vote 0 at the beginning of each loop to gate the updates
 * to the charger
 */
static int msc_update_charger_cb(struct votable *votable,
				 void *data, int interval,
				 const char *client)
{
	int update_interval, rc = -EINVAL, fv_uv = -1, cc_max = -1;
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	__pm_stay_awake(chg_drv->chg_ws);

	update_interval =
		get_effective_result_locked(chg_drv->msc_interval_votable);
	if (update_interval <= 0)
		goto msc_done;
	if (chg_drv->chg_mode == CHG_DRV_MODE_NOOP)
		goto msc_reschedule;

	/*
	 * = fv_uv will be at last charger tier in RL, last tier before JEITA
	 *   when JEITA hits, the default (max) value when temperature fall
	 *   outside JEITA limits on connect or negative when a value is not
	 *   specified in device tree. Setting max on JEITA violation is not
	 *   a problem because HW wil set the charge current to 0, the charger
	 *   driver does sanity checking as well (but we should not rely on it)
	 * = cc_max should not be negative here and is 0 on RL and on JEITA.
	 */
	fv_uv = get_effective_result_locked(chg_drv->msc_fv_votable);
	cc_max = get_effective_result_locked(chg_drv->msc_fcc_votable);

	/* invalid values will cause the adapter to exit PPS */
	if (cc_max < 0 || fv_uv < 0) {
		update_interval = CHG_WORK_ERROR_RETRY_MS;
		goto msc_reschedule;
	}

	if (chg_drv->pps_data.stage == PPS_ACTIVE) {
		int pps_ui;

		pps_ui = msc_update_pps(chg_drv, fv_uv, cc_max);
		/* I am not sure that is the case */
		if (pps_ui == -ECANCELED)
			goto msc_done;
		/* force ping */
		if (pps_ui >= 0 && pps_ui < update_interval)
			update_interval = pps_ui;
	}

	/* adjust charger for target demand */
	rc = chg_update_charger(chg_drv, fv_uv, cc_max);
	if (rc < 0)
		update_interval = CHG_WORK_ERROR_RETRY_MS;

msc_reschedule:
	alarm_try_to_cancel(&chg_drv->chg_wakeup_alarm);
	alarm_start_relative(&chg_drv->chg_wakeup_alarm,
			     ms_to_ktime(update_interval));

	pr_info("MSC_CHG fv_uv=%d, cc_max=%d, rerun in %d ms (%d)\n",
		fv_uv, cc_max, update_interval, rc);

msc_done:
	__pm_relax(chg_drv->chg_ws);
	return 0;
}

/* NOTE: we need a single source of truth. Charging can be disabled via the
 * votable and directy setting the property.
 */
static int msc_chg_disable_cb(struct votable *votable, void *data,
			int chg_disable, const char *client)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;
	int rc;

	if (!chg_drv->chg_psy)
		return 0;

	rc = GPSY_SET_PROP(chg_drv->chg_psy,
			POWER_SUPPLY_PROP_CHARGE_DISABLE, chg_disable);
	if (rc < 0) {
		dev_err(chg_drv->device, "Couldn't %s charging rc=%d\n",
				chg_disable ? "disable" : "enable", rc);
		return rc;
	}

	return 0;
}

static int msc_pwr_disable_cb(struct votable *votable, void *data,
			int pwr_disable, const char *client)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	if (!chg_drv->chg_psy)
		return 0;

	return chg_vote_input_suspend(chg_drv, MSC_CHG_VOTER, pwr_disable);
}

static int chg_disable_std_votables(struct chg_drv *chg_drv)
{
	struct votable *qc_votable;
	bool std_votables;

	std_votables = of_property_read_bool(chg_drv->device->of_node,
					     "google,has-std-votables");
	if (!std_votables)
		return 0;

	qc_votable = find_votable("FV");
	if (!qc_votable)
		return -EPROBE_DEFER;

	vote(qc_votable, MSC_CHG_VOTER, true, -1);

	qc_votable = find_votable("FCC");
	if (!qc_votable)
		return -EPROBE_DEFER;

	vote(qc_votable, MSC_CHG_VOTER, true, -1);

	return 0;
}

/* TODO: qcom/battery.c mostly handles PL charging: we don't need it.
 *  In order to remove and keep using QCOM code, create "USB_ICL",
 *  "PL_DISABLE", "PL_AWAKE" and "PL_ENABLE_INDIRECT" in a new function called
 *  qcom_batt_init(). Also might need to change the names of our votables for
 *  FCC, FV to match QCOM.
 * NOTE: Battery also register "qcom-battery" class so it might not be too
 * straightforward to remove all dependencies.
 */
static int chg_create_votables(struct chg_drv *chg_drv)
{
	int ret;

	chg_drv->msc_fv_votable =
		create_votable(VOTABLE_MSC_FV,
			VOTE_MIN,
			NULL,
			chg_drv);
	if (IS_ERR(chg_drv->msc_fv_votable)) {
		ret = PTR_ERR(chg_drv->msc_fv_votable);
		chg_drv->msc_fv_votable = NULL;
		goto error_exit;
	}

	chg_drv->msc_fcc_votable =
		create_votable(VOTABLE_MSC_FCC,
			VOTE_MIN,
			NULL,
			chg_drv);
	if (IS_ERR(chg_drv->msc_fcc_votable)) {
		ret = PTR_ERR(chg_drv->msc_fcc_votable);
		chg_drv->msc_fcc_votable = NULL;
		goto error_exit;
	}

	chg_drv->msc_interval_votable =
		create_votable(VOTABLE_MSC_INTERVAL,
			VOTE_MIN,
			msc_update_charger_cb,
			chg_drv);
	if (IS_ERR(chg_drv->msc_interval_votable)) {
		ret = PTR_ERR(chg_drv->msc_interval_votable);
		chg_drv->msc_interval_votable = NULL;
		goto error_exit;
	}

	chg_drv->msc_chg_disable_votable =
		create_votable(VOTABLE_MSC_CHG_DISABLE,
			VOTE_SET_ANY,
			msc_chg_disable_cb,
			chg_drv);
	if (IS_ERR(chg_drv->msc_chg_disable_votable)) {
		ret = PTR_ERR(chg_drv->msc_chg_disable_votable);
		chg_drv->msc_chg_disable_votable = NULL;
		goto error_exit;
	}

	chg_drv->msc_pwr_disable_votable =
		create_votable(VOTABLE_MSC_PWR_DISABLE,
			VOTE_SET_ANY,
			msc_pwr_disable_cb,
			chg_drv);
	if (IS_ERR(chg_drv->msc_pwr_disable_votable)) {
		ret = PTR_ERR(chg_drv->msc_pwr_disable_votable);
		chg_drv->msc_pwr_disable_votable = NULL;
		goto error_exit;
	}

	return 0;

error_exit:
	destroy_votable(chg_drv->msc_fv_votable);
	destroy_votable(chg_drv->msc_fcc_votable);
	destroy_votable(chg_drv->msc_interval_votable);
	destroy_votable(chg_drv->msc_chg_disable_votable);
	destroy_votable(chg_drv->msc_pwr_disable_votable);

	chg_drv->msc_fv_votable = NULL;
	chg_drv->msc_fcc_votable = NULL;
	chg_drv->msc_interval_votable = NULL;
	chg_drv->msc_chg_disable_votable = NULL;
	chg_drv->msc_pwr_disable_votable = NULL;

	return ret;
}

static void chg_init_votables(struct chg_drv *chg_drv)
{
	/* prevent all changes until the first roundtrip with real state */
	vote(chg_drv->msc_interval_votable, MSC_CHG_VOTER, true, 0);

	/* will not be applied until we vote non-zero msc_interval */
	vote(chg_drv->msc_fv_votable, MAX_VOTER,
	     chg_drv->batt_profile_fv_uv > 0, chg_drv->batt_profile_fv_uv);
	vote(chg_drv->msc_fcc_votable, MAX_VOTER,
	     chg_drv->batt_profile_fcc_ua > 0, chg_drv->batt_profile_fcc_ua);

}

static int chg_get_max_charge_cntl_limit(struct thermal_cooling_device *tcd,
					 unsigned long *lvl)
{
	struct chg_thermal_device *tdev =
		(struct chg_thermal_device *)tcd->devdata;
	*lvl = tdev->thermal_levels;
	return 0;
}

static int chg_get_cur_charge_cntl_limit(struct thermal_cooling_device *tcd,
					 unsigned long *lvl)
{
	struct chg_thermal_device *tdev =
		(struct chg_thermal_device *)tcd->devdata;
	*lvl = tdev->current_level;
	return 0;
}

/* Wireless and wired limits are linked when therm_wlc_override_fcc is true.
 * This means that charging from WLC (wlc_psy is ONLINE) will disable the
 * the thermal vote on MSC_FCC (b/128350180)
 */
static int chg_therm_update_fcc(struct chg_drv *chg_drv)
{
	struct chg_thermal_device *tdev =
			&chg_drv->thermal_devices[CHG_TERMAL_DEVICE_FCC];
	int ret, wlc_online = 0, fcc = -1;

	if (chg_drv->wlc_psy && chg_drv->therm_wlc_override_fcc)
		wlc_online = GPSY_GET_PROP(chg_drv->wlc_psy,
					POWER_SUPPLY_PROP_ONLINE);

	if (wlc_online <= 0 && tdev->current_level > 0)
		fcc = tdev->thermal_mitigation[tdev->current_level];

	ret = vote(chg_drv->msc_fcc_votable,
			THERMAL_DAEMON_VOTER,
			(fcc != -1),
			fcc);
	return ret;
}

static int chg_set_fcc_charge_cntl_limit(struct thermal_cooling_device *tcd,
					 unsigned long lvl)
{
	int ret = 0;
	struct chg_thermal_device *tdev =
		(struct chg_thermal_device *)tcd->devdata;
	struct chg_drv *chg_drv = tdev->chg_drv;
	const bool changed = (tdev->current_level != lvl);

	if (lvl < 0 || tdev->thermal_levels <= 0 || lvl > tdev->thermal_levels)
		return -EINVAL;

	tdev->current_level = lvl;

	if (tdev->current_level == tdev->thermal_levels) {
		pr_info("MSC_THERM_FCC lvl=%d charge disable\n", lvl);
		return vote(chg_drv->msc_chg_disable_votable,
					THERMAL_DAEMON_VOTER, true, 0);
	}

	vote(chg_drv->msc_chg_disable_votable, THERMAL_DAEMON_VOTER, false, 0);

	ret = chg_therm_update_fcc(chg_drv);
	if (ret < 0 || changed)
		pr_info("MSC_THERM_FCC lvl=%d (%d)\n",
				tdev->current_level,
				ret);

	/* force to apply immediately */
	reschedule_chg_work(chg_drv);
	return ret;
}

static int chg_set_dc_in_charge_cntl_limit(struct thermal_cooling_device *tcd,
					   unsigned long lvl)
{
	struct chg_thermal_device *tdev =
			(struct chg_thermal_device *)tcd->devdata;
	const bool changed = (tdev->current_level != lvl);
	struct chg_drv *chg_drv = tdev->chg_drv;
	union power_supply_propval pval;
	int dc_icl = -1, ret;

	if (lvl < 0 || tdev->thermal_levels <= 0 || lvl > tdev->thermal_levels)
		return -EINVAL;

	if (!chg_drv->dc_icl_votable)
		chg_drv->dc_icl_votable = find_votable("DC_ICL");

	tdev->current_level = lvl;

	if (tdev->current_level == tdev->thermal_levels) {
		if (chg_drv->dc_icl_votable)
			vote(chg_drv->dc_icl_votable,
				THERMAL_DAEMON_VOTER, true, 0);

		/* WLC set the wireless charger offline b/119501863 */
		if (chg_drv->wlc_psy) {
			pval.intval = 0;
			power_supply_set_property(chg_drv->wlc_psy,
				POWER_SUPPLY_PROP_ONLINE, &pval);
		}

		pr_info("MSC_THERM_DC lvl=%d dc disable\n", lvl);

		return 0;
	}

	if (chg_drv->wlc_psy) {
		pval.intval = 1;
		power_supply_set_property(chg_drv->wlc_psy,
				POWER_SUPPLY_PROP_ONLINE, &pval);
	}

	if (!chg_drv->dc_icl_votable)
		return 0;

	if (tdev->current_level != 0)
		dc_icl = tdev->thermal_mitigation[tdev->current_level];

	ret = vote(chg_drv->dc_icl_votable, THERMAL_DAEMON_VOTER,
			(dc_icl != -1),
			dc_icl);

	if (ret < 0 || changed)
		pr_info("MSC_THERM_DC lvl=%d dc_icl=%d (%d)\n",
			lvl, dc_icl, ret);

	/* make sure that fcc is reset to max when charging from WLC*/
	if (ret ==0)
		(void)chg_therm_update_fcc(chg_drv);

	return 0;
}

int chg_tdev_init(struct chg_thermal_device *tdev,
		  const char *name,
		  struct chg_drv *chg_drv)
{
	int rc, byte_len;

	if (!of_find_property(chg_drv->device->of_node, name, &byte_len)) {
		dev_err(chg_drv->device, "No cooling device for %s\n", name);
		return -ENOENT;
	}

	tdev->thermal_mitigation = devm_kzalloc(chg_drv->device, byte_len,
							GFP_KERNEL);
	if (!tdev->thermal_mitigation)
		return -ENOMEM;

	tdev->thermal_levels = byte_len / sizeof(u32);

	rc = of_property_read_u32_array(chg_drv->device->of_node,
			name,
			tdev->thermal_mitigation,
			tdev->thermal_levels);
	if (rc < 0) {
		dev_err(chg_drv->device,
			"Couldn't read limits for %s rc = %d\n", name, rc);
		return -ENODATA;
	}

	tdev->chg_drv = chg_drv;

	return 0;
}

static const struct thermal_cooling_device_ops chg_fcc_tcd_ops = {
	.get_max_state = chg_get_max_charge_cntl_limit,
	.get_cur_state = chg_get_cur_charge_cntl_limit,
	.set_cur_state = chg_set_fcc_charge_cntl_limit,
};

static const struct thermal_cooling_device_ops chg_dc_icl_tcd_ops = {
	.get_max_state = chg_get_max_charge_cntl_limit,
	.get_cur_state = chg_get_cur_charge_cntl_limit,
	.set_cur_state = chg_set_dc_in_charge_cntl_limit,
};

/* ls /sys/devices/virtual/thermal/cdev-by-name/ */
int chg_thermal_device_init(struct chg_drv *chg_drv)
{
	int rc;
	struct device_node *cooling_node = NULL;

	rc = chg_tdev_init(&chg_drv->thermal_devices[CHG_TERMAL_DEVICE_FCC],
				"google,thermal-mitigation", chg_drv);
	if (rc == 0) {
		struct chg_thermal_device *fcc =
			&chg_drv->thermal_devices[CHG_TERMAL_DEVICE_FCC];

		cooling_node = of_find_node_by_name(NULL, FCC_OF_CDEV_NAME);
		if (!cooling_node) {
			pr_err("No %s OF node for cooling device\n",
				FCC_OF_CDEV_NAME);
		}

		fcc->tcd = thermal_of_cooling_device_register(
						cooling_node,
						FCC_CDEV_NAME,
						fcc,
						&chg_fcc_tcd_ops);
		if (!fcc->tcd) {
			pr_err("error registering fcc cooling device\n");
			return -EINVAL;
		}
	}

	rc = chg_tdev_init(&chg_drv->thermal_devices[CHG_TERMAL_DEVICE_DC_IN],
				"google,wlc-thermal-mitigation", chg_drv);
	if (rc == 0) {
		struct chg_thermal_device *dc_icl =
			&chg_drv->thermal_devices[CHG_TERMAL_DEVICE_DC_IN];
		cooling_node = NULL;
		cooling_node = of_find_node_by_name(NULL, WLC_OF_CDEV_NAME);
		if (!cooling_node) {
			pr_err("No %s OF node for cooling device\n",
				WLC_OF_CDEV_NAME);
		}

		dc_icl->tcd = thermal_of_cooling_device_register(
						cooling_node,
						WLC_CDEV_NAME,
						dc_icl,
						&chg_dc_icl_tcd_ops);
		if (!dc_icl->tcd)
			goto error_exit;
	}

	chg_drv->therm_wlc_override_fcc =
		of_property_read_bool(chg_drv->device->of_node,
					"google,therm-wlc-overrides-fcc");
	if (chg_drv->therm_wlc_override_fcc)
		pr_info("WLC overrides FCC\n");

	return 0;

error_exit:
	pr_err("error registering dc_icl cooling device\n");
	thermal_cooling_device_unregister(
		chg_drv->thermal_devices[CHG_TERMAL_DEVICE_FCC].tcd);

	return -EINVAL;
}

static struct power_supply *psy_get_by_name(struct chg_drv *chg_drv,
					    const char *name)
{
	struct power_supply *psy;

	psy = power_supply_get_by_name(name);
	if (!psy && chg_drv->log_psy_ratelimit) {
		pr_warn_ratelimited("failed to get \"%s\" power supply, retrying...\n",
				    name);
		chg_drv->log_psy_ratelimit -= 1;
	}

	return psy;
}

/* TODO: refactor to use pps_get_tcpm_psy() */
static struct power_supply *get_tcpm_psy(struct chg_drv *chg_drv)
{
	struct power_supply *psy[2];
	int i, ret;
	struct power_supply *tcpm_psy = NULL;

	ret = power_supply_get_by_phandle_array(chg_drv->device->of_node,
						"google,tcpm-power-supply", psy,
						ARRAY_SIZE(psy));
	if (ret < 0 && !chg_drv->usb_skip_probe) {
		if (!chg_drv->log_psy_ratelimit)
			return ERR_PTR(ret);

		pr_info("failed to get tcpm power supply, retrying... ret:%d\n",
			ret);
		/*
		 * Accessed from the same execution context i.e.
		 * google_charger_init_workor else needs to be protected
		 * along with access in psy_get_by_name.
		 */
		chg_drv->log_psy_ratelimit -= 1;

		return ERR_PTR(ret);
	} else if (ret > 0) {
		for (i = 0; i < ret; i++) {
			if (psy[i]->desc && psy[i]->desc->name && !strncmp(
					psy[i]->desc->name, "tcpm", strlen(
							"tcpm")))
				tcpm_psy = psy[i];
			else
				power_supply_put(psy[i]);
		}

		chg_drv->tcpm_psy_name = tcpm_psy->desc->name;
		pr_info("tcpm psy_name: %s\n", chg_drv->tcpm_psy_name);
	}

	return tcpm_psy;
}

static void google_charger_init_work(struct work_struct *work)
{
	struct chg_drv *chg_drv = container_of(work, struct chg_drv,
					       init_work.work);
	struct power_supply *chg_psy = NULL, *usb_psy = NULL;
	struct power_supply *wlc_psy = NULL, *bat_psy = NULL;
	struct power_supply *tcpm_psy = NULL;
	bool pps_enable;
	int ret = 0;

	chg_psy = psy_get_by_name(chg_drv, chg_drv->chg_psy_name);
	if (!chg_psy)
		goto retry_init_work;

	bat_psy = psy_get_by_name(chg_drv, chg_drv->bat_psy_name);
	if (!bat_psy)
		goto retry_init_work;

	if (chg_drv->usb_psy_name) {
		usb_psy = psy_get_by_name(chg_drv, chg_drv->usb_psy_name);
		if (!usb_psy && !chg_drv->usb_skip_probe)
			goto retry_init_work;
	}

	if (chg_drv->wlc_psy_name) {
		wlc_psy = psy_get_by_name(chg_drv, chg_drv->wlc_psy_name);
		if (!wlc_psy)
			goto retry_init_work;
	}

	if (chg_drv->tcpm_phandle) {
		tcpm_psy = get_tcpm_psy(chg_drv);
		if (IS_ERR(tcpm_psy)) {
			tcpm_psy = NULL;
			goto retry_init_work;
		}
	}

	/* TODO: make this optional since we don't need to do this anymore */
	ret = chg_disable_std_votables(chg_drv);
	if (ret == -EPROBE_DEFER)
		goto retry_init_work;

	chg_drv->chg_psy = chg_psy;
	chg_drv->wlc_psy = wlc_psy;
	chg_drv->usb_psy = usb_psy;
	chg_drv->bat_psy = bat_psy;
	chg_drv->tcpm_psy = tcpm_psy;

	/* PPS negotiation handled in google_charger */
	pps_enable = of_property_read_bool(chg_drv->device->of_node,
					   "google,pps-enable");
	if (!tcpm_psy) {
		pr_info("PPS not available\n");
	} else if (!pps_enable) {
		pr_info("PPS not enabled\n");
	} else {
		const char *name = tcpm_psy->desc->name;

		ret = pps_init(&chg_drv->pps_data, chg_drv->device);
		if (ret == 0 && chg_drv->debug_entry)
			pps_init_fs(&chg_drv->pps_data, chg_drv->debug_entry);
		if (ret < 0)
			pr_err("PPS init failure for %s (%d)\n", name, ret);
		else
			pr_info("PPS available for %s\n", name);
	}

	ret = chg_thermal_device_init(chg_drv);
	if (ret < 0)
		pr_err("Cannot register thermal devices, ret=%d\n", ret);

	chg_drv->dead_battery = chg_update_dead_battery(chg_drv);
	if (chg_drv->dead_battery)
		pr_info("dead battery mode\n");

	chg_init_state(chg_drv);
	chg_drv->stop_charging = -1;
	chg_drv->charge_stop_level = DEFAULT_CHARGE_STOP_LEVEL;
	chg_drv->charge_start_level = DEFAULT_CHARGE_START_LEVEL;

	/* reset override charging parameters */
	chg_drv->user_fv_uv = -1;
	chg_drv->user_cc_max = -1;

	chg_drv->psy_nb.notifier_call = chg_psy_changed;
	ret = power_supply_reg_notifier(&chg_drv->psy_nb);
	if (ret < 0)
		pr_err("Cannot register power supply notifer, ret=%d\n", ret);

	pr_info("init_work done\n");

	/* catch state changes that happened before registering the notifier */
	schedule_delayed_work(&chg_drv->chg_work,
		msecs_to_jiffies(CHG_DELAY_INIT_DETECT_MS));
	return;

retry_init_work:
	if (chg_psy)
		power_supply_put(chg_psy);
	if (bat_psy)
		power_supply_put(bat_psy);
	if (usb_psy)
		power_supply_put(usb_psy);
	if (wlc_psy)
		power_supply_put(wlc_psy);
	if (tcpm_psy)
		power_supply_put(tcpm_psy);
	schedule_delayed_work(&chg_drv->init_work,
			      msecs_to_jiffies(CHG_DELAY_INIT_MS));
}

static int google_charger_probe(struct platform_device *pdev)
{
	const char *chg_psy_name, *bat_psy_name;
	const char *usb_psy_name = NULL, *wlc_psy_name = NULL;
	struct chg_drv *chg_drv;
	int ret;

	chg_drv = devm_kzalloc(&pdev->dev, sizeof(*chg_drv), GFP_KERNEL);
	if (!chg_drv)
		return -ENOMEM;

	chg_drv->device = &pdev->dev;

	ret = of_property_read_string(pdev->dev.of_node,
				      "google,chg-power-supply",
				      &chg_psy_name);
	if (ret != 0) {
		pr_err("cannot read google,chg-power-supply, ret=%d\n", ret);
		return -EINVAL;
	}
	chg_drv->chg_psy_name =
	    devm_kstrdup(&pdev->dev, chg_psy_name, GFP_KERNEL);
	if (!chg_drv->chg_psy_name)
		return -ENOMEM;

	ret = of_property_read_string(pdev->dev.of_node,
				      "google,bat-power-supply",
				      &bat_psy_name);
	if (ret != 0) {
		pr_err("cannot read google,bat-power-supply, ret=%d\n", ret);
		return -EINVAL;
	}
	chg_drv->bat_psy_name =
	    devm_kstrdup(&pdev->dev, bat_psy_name, GFP_KERNEL);
	if (!chg_drv->bat_psy_name)
		return -ENOMEM;

	ret = of_property_read_string(pdev->dev.of_node,
				      "google,wlc-power-supply",
				      &wlc_psy_name);
	if (ret != 0)
		pr_warn("google,wlc-power-supply not defined\n");
	if (wlc_psy_name) {
		chg_drv->wlc_psy_name =
		    devm_kstrdup(&pdev->dev, wlc_psy_name, GFP_KERNEL);
		if (!chg_drv->wlc_psy_name)
			return -ENOMEM;
	}

	ret = of_property_read_string(pdev->dev.of_node,
				      "google,usb-power-supply",
				      &usb_psy_name);
	if (ret != 0)
		pr_warn("google,usb-power-supply not defined\n");
	if (usb_psy_name) {
		chg_drv->usb_psy_name =
		    devm_kstrdup(&pdev->dev, usb_psy_name, GFP_KERNEL);
		if (!chg_drv->usb_psy_name)
			return -ENOMEM;
	}

	ret = of_property_read_u32(pdev->dev.of_node,
				   "google,tcpm-power-supply",
				   &chg_drv->tcpm_phandle);
	if (ret < 0)
		pr_warn("google,tcpm-power-supply not defined\n");

	/*
	 * when set will reduce the comparison value for ibatt by
	 *         cc_max * (100 - pps_cc_tolerance_pct) / 100
	 *  only used from the PPS policy
	 */
	ret = of_property_read_u32(pdev->dev.of_node,
				   "google,pps-cc-tolerance-pct",
				   &chg_drv->pps_cc_tolerance_pct);
	if (ret < 0)
		chg_drv->pps_cc_tolerance_pct = PPS_CC_TOLERANCE_PCT_DEFAULT;
	else if (chg_drv->pps_cc_tolerance_pct > PPS_CC_TOLERANCE_PCT_MAX)
		chg_drv->pps_cc_tolerance_pct = PPS_CC_TOLERANCE_PCT_MAX;

	/* user fcc, fv uv are bound by battery votes.
	 * set google,disable_votes in battery node to disable battery votes.
	 */
	chg_drv->enable_user_fcc_fv =
		of_property_read_bool(pdev->dev.of_node,
				      "google,enable-user-fcc-fv");
	if (chg_drv->enable_user_fcc_fv)
		pr_info("User can override FCC and FV\n");

	/* NOTE: newgen charging is configured in google_battery */
	ret = chg_init_chg_profile(chg_drv);
	if (ret < 0) {
		pr_err("cannot read charging profile from dt, ret=%d\n", ret);
		return ret;
	}

	if (chg_drv->chg_term.enable) {
		INIT_WORK(&chg_drv->chg_term.work,
			  chg_termination_work);

		if (alarmtimer_get_rtcdev()) {
			alarm_init(&chg_drv->chg_term.alarm,
				   ALARM_BOOTTIME,
				   chg_termination_alarm_cb);
		} else {
			/* keep the driver init even if get alarmtimer fail */
			chg_drv->chg_term.enable = false;
			cancel_work_sync(&chg_drv->chg_term.work);
			pr_err("Couldn't get rtc device\n");
		}
	}



	INIT_DELAYED_WORK(&chg_drv->init_work, google_charger_init_work);
	INIT_DELAYED_WORK(&chg_drv->chg_work, chg_work);
	INIT_WORK(&chg_drv->chg_psy_work, chg_psy_work);
	platform_set_drvdata(pdev, chg_drv);

	alarm_init(&chg_drv->chg_wakeup_alarm, ALARM_BOOTTIME,
		   google_chg_alarm_handler);

	/* votables and chg_work need a wakeup source */
	chg_drv->chg_ws = wakeup_source_register(NULL, "google-charger");
	if (!chg_drv->chg_ws) {
		pr_err("Failed to register wakeup source\n");
		return -ENODEV;
	}

	/* create the votables before talking to google_battery */
	ret = chg_create_votables(chg_drv);
	if (ret < 0)
		pr_err("Failed to create votables, ret=%d\n", ret);
	else
		chg_init_votables(chg_drv);

	/* sysfs & debug */
	chg_init_fs(chg_drv);

	/* ratelimit makes init work quiet */
	chg_drv->log_psy_ratelimit = CHG_LOG_PSY_RATELIMIT_CNT;
	schedule_delayed_work(&chg_drv->init_work,
			      msecs_to_jiffies(CHG_DELAY_INIT_MS));

	dev_info(chg_drv->device, "probe work done");
	return 0;
}

static void chg_destroy_votables(struct chg_drv *chg_drv)
{
	destroy_votable(chg_drv->msc_interval_votable);
	destroy_votable(chg_drv->msc_fv_votable);
	destroy_votable(chg_drv->msc_fcc_votable);
	destroy_votable(chg_drv->msc_chg_disable_votable);
	destroy_votable(chg_drv->msc_pwr_disable_votable);
}

static int google_charger_remove(struct platform_device *pdev)
{
	struct chg_drv *chg_drv = (struct chg_drv *)platform_get_drvdata(pdev);

	if (chg_drv) {
		if (chg_drv->chg_term.enable) {
			alarm_cancel(&chg_drv->chg_term.alarm);
			cancel_work_sync(&chg_drv->chg_term.work);
		}

		chg_destroy_votables(chg_drv);

		if (chg_drv->chg_psy)
			power_supply_put(chg_drv->chg_psy);
		if (chg_drv->bat_psy)
			power_supply_put(chg_drv->bat_psy);
		if (chg_drv->usb_psy)
			power_supply_put(chg_drv->usb_psy);
		if (chg_drv->wlc_psy)
			power_supply_put(chg_drv->wlc_psy);
		if (chg_drv->tcpm_psy)
			power_supply_put(chg_drv->tcpm_psy);

		wakeup_source_unregister(chg_drv->chg_ws);
		wakeup_source_unregister(chg_drv->pps_data.pps_ws);

		alarm_try_to_cancel(&chg_drv->chg_wakeup_alarm);

		if (chg_drv->pps_data.log)
			debugfs_logbuffer_unregister(chg_drv->pps_data.log);
	}

	return 0;
}

#ifdef SUPPORT_PM_SLEEP
static int chg_pm_suspend(struct device *dev)
{

	return 0;
}

static int chg_pm_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct chg_drv *chg_drv = platform_get_drvdata(pdev);

	chg_drv->egain_retries = 0;
	reschedule_chg_work(chg_drv);

	return 0;
}

static const struct dev_pm_ops chg_pm_ops = {
	SET_LATE_SYSTEM_SLEEP_PM_OPS(chg_pm_suspend, chg_pm_resume)
};
#endif


static const struct of_device_id match_table[] = {
	{.compatible = "google,charger"},
	{},
};

static struct platform_driver charger_driver = {
	.driver = {
		   .name = "google,charger",
		   .owner = THIS_MODULE,
		   .of_match_table = match_table,
#ifdef SUPPORT_PM_SLEEP
		   .pm = &chg_pm_ops,
#endif
		   },
	.probe = google_charger_probe,
	.remove = google_charger_remove,
};

static int __init google_charger_init(void)
{
	int ret;

	ret = platform_driver_register(&charger_driver);
	if (ret < 0) {
		pr_err("device registration failed: %d\n", ret);
		return ret;
	}
	return 0;
}

static void __init google_charger_exit(void)
{
	platform_driver_unregister(&charger_driver);
	pr_info("unregistered platform driver\n");
}

module_init(google_charger_init);
module_exit(google_charger_exit);
MODULE_DESCRIPTION("Multi-step battery charger driver");
MODULE_AUTHOR("Thierry Strudel <tstrudel@google.com>");
MODULE_AUTHOR("AleX Pelosi <apelosi@google.com>");
MODULE_LICENSE("GPL");

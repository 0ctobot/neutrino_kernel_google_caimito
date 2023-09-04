// SPDX-License-Identifier: GPL-2.0
/*
 * Synaptics TouchCom touchscreen driver
 *
 * Copyright (C) 2017-2020 Synaptics Incorporated. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * INFORMATION CONTAINED IN THIS DOCUMENT IS PROVIDED "AS-IS," AND SYNAPTICS
 * EXPRESSLY DISCLAIMS ALL EXPRESS AND IMPLIED WARRANTIES, INCLUDING ANY
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE,
 * AND ANY WARRANTIES OF NON-INFRINGEMENT OF ANY INTELLECTUAL PROPERTY RIGHTS.
 * IN NO EVENT SHALL SYNAPTICS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, PUNITIVE, OR CONSEQUENTIAL DAMAGES ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OF THE INFORMATION CONTAINED IN THIS DOCUMENT, HOWEVER CAUSED
 * AND BASED ON ANY THEORY OF LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, AND EVEN IF SYNAPTICS WAS ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE. IF A TRIBUNAL OF COMPETENT JURISDICTION DOES
 * NOT PERMIT THE DISCLAIMER OF DIRECT DAMAGES OR ANY OTHER DAMAGES, SYNAPTICS'
 * TOTAL CUMULATIVE LIABILITY TO ANY PARTY SHALL NOT EXCEED ONE HUNDRED U.S.
 * DOLLARS.
 */

/**
 * @file syna_tcm2.c
 *
 * This file implements the Synaptics device driver running under Linux kernel
 * input device subsystem, and also communicate with Synaptics touch controller
 * through TouchComm command-response protocol.
 */

#include "syna_tcm2.h"
#include "syna_tcm2_cdev.h"
#include "syna_tcm2_platform.h"
#include "synaptics_touchcom_core_dev.h"
#include "synaptics_touchcom_func_base.h"
#include "synaptics_touchcom_func_touch.h"
#ifdef STARTUP_REFLASH
#ifdef HAS_ROMBOOT_REFLASH_FEATURE
#include "synaptics_touchcom_func_romboot.h"
#else
#include "synaptics_touchcom_func_reflash.h"
#endif
#endif

/**
 * @section: USE_CUSTOM_TOUCH_REPORT_CONFIG
 *           Open if willing to set up the format of touch report.
 *           The custom_touch_format[] array can be used to describe the
 *           customized report format.
 */
#ifdef USE_CUSTOM_TOUCH_REPORT_CONFIG
static unsigned char custom_touch_format[] = {
	/* entity code */                    /* bits */
#ifdef ENABLE_WAKEUP_GESTURE
	TOUCH_REPORT_GESTURE_ID,                8,
#endif
	TOUCH_REPORT_NUM_OF_ACTIVE_OBJECTS,     8,
	TOUCH_REPORT_FOREACH_ACTIVE_OBJECT,
	TOUCH_REPORT_OBJECT_N_INDEX,            8,
	TOUCH_REPORT_OBJECT_N_CLASSIFICATION,   8,
	TOUCH_REPORT_OBJECT_N_X_POSITION,       16,
	TOUCH_REPORT_OBJECT_N_Y_POSITION,       16,
	TOUCH_REPORT_FOREACH_END,
	TOUCH_REPORT_END
};
#endif

/**
 * @section: RESET_ON_RESUME_DELAY_MS
 *           The delayed time to issue a reset on resume state.
 *           This configuration depends on RESET_ON_RESUME.
 */
#ifdef RESET_ON_RESUME
#define RESET_ON_RESUME_DELAY_MS (100)
#endif


/**
 * @section: POWER_ALIVE_AT_SUSPEND
 *           indicate that the power is still alive even at
 *           system suspend.
 *           otherwise, there is no power supplied when system
 *           is going to suspend stage.
 */
#define POWER_ALIVE_AT_SUSPEND

/**
 * @section: global variables for an active drm panel
 *           in order to register display notifier
 */
#ifdef USE_DRM_PANEL_NOTIFIER
	struct drm_panel *active_panel;
#endif

#if IS_ENABLED(CONFIG_PM) || IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
static const struct dev_pm_ops syna_dev_pm_ops;
#endif

/**
 * syna_dev_enable_lowpwr_gesture()
 *
 * Enable or disable the low power gesture mode.
 * Furthermore, set up the wake-up irq.
 *
 * @param
 *    [ in] tcm: tcm driver handle
 *    [ in] en:  '1' to enable low power gesture mode; '0' to disable
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_enable_lowpwr_gesture(struct syna_tcm *tcm, bool en)
{
	int retval = 0;
	struct syna_hw_attn_data *attn = &tcm->hw_if->bdata_attn;

	if (!tcm->lpwg_enabled)
		return 0;

	if (attn->irq_id == 0)
		return 0;

	if (en) {
		if (!tcm->irq_wake) {
			enable_irq_wake(attn->irq_id);
			tcm->irq_wake = true;
		}

		/* enable wakeup gesture mode
		 *
		 * the wakeup gesture control may result from by finger event;
		 * therefore, it's better to use ATTN driven mode here
		 */
		retval = syna_tcm_set_dynamic_config(tcm->tcm_dev,
				DC_ENABLE_WAKEUP_GESTURE_MODE,
				1,
				RESP_IN_ATTN);
		if (retval < 0) {
			LOGE("Fail to enable wakeup gesture via DC command\n");
			return retval;
		}
	} else {
		if (tcm->irq_wake) {
			disable_irq_wake(attn->irq_id);
			tcm->irq_wake = false;
		}

		/* disable wakeup gesture mode
		 *
		 * the wakeup gesture control may result from by finger event;
		 * therefore, it's better to use ATTN driven mode here
		 */
		retval = syna_tcm_set_dynamic_config(tcm->tcm_dev,
				DC_ENABLE_WAKEUP_GESTURE_MODE,
				0,
				RESP_IN_ATTN);
		if (retval < 0) {
			LOGE("Fail to disable wakeup gesture via DC command\n");
			return retval;
		}
	}

	return retval;
}

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
static int gti_default_handler(void *private_data, enum gti_cmd_type cmd_type,
	struct gti_union_cmd_data *cmd)
{
	int ret = 0;

	switch (cmd_type) {
	case GTI_CMD_NOTIFY_DISPLAY_STATE:
	case GTI_CMD_NOTIFY_DISPLAY_VREFRESH:
		ret = -EOPNOTSUPP;
		break;
	default:
		ret = -ESRCH;
		break;
	}

	return ret;
}

static int get_fw_version(void *private_data, struct gti_fw_version_cmd *cmd)
{
	struct syna_tcm *tcm = private_data;
	int cmd_buffer_size = sizeof(cmd->buffer);

	syna_get_fw_info(tcm, cmd->buffer, cmd_buffer_size);
	return 0;
}

static int get_irq_mode(void *private_data, struct gti_irq_cmd *cmd)
{
	struct syna_tcm *tcm = private_data;

	cmd->setting = tcm->hw_if->bdata_attn.irq_enabled ?
			GTI_IRQ_MODE_ENABLE : GTI_IRQ_MODE_DISABLE;

	return 0;
}

static int set_irq_mode(void *private_data, struct gti_irq_cmd *cmd)
{
	struct syna_tcm *tcm = private_data;

	return tcm->hw_if->ops_enable_irq(tcm->hw_if, cmd->setting == GTI_IRQ_MODE_ENABLE);
}

static int set_reset(void *private_data, struct gti_reset_cmd *cmd)
{
	struct syna_tcm *tcm = private_data;

	if (goog_pm_wake_get_locks(tcm->gti) == 0 || tcm->pwr_state != PWR_ON) {
		LOGI("Connot trigger reset because touch is off");
		return -EPERM;
	}

	if (cmd->setting == GTI_RESET_MODE_HW || cmd->setting == GTI_RESET_MODE_AUTO) {
		tcm->hw_if->ops_hw_reset(tcm->hw_if);
	} else if (cmd->setting == GTI_RESET_MODE_SW) {
		syna_tcm_reset(tcm->tcm_dev);
		syna_dev_restore_feature_setting(tcm, RESP_IN_ATTN);
	} else {
		return -EOPNOTSUPP;
	}

	return 0;
}

static int syna_set_palm_mode(void *private_data, struct gti_palm_cmd *cmd)
{
	struct syna_tcm *tcm = private_data;

	if (goog_pm_wake_get_locks(tcm->gti) == 0 || tcm->pwr_state != PWR_ON) {
		LOGI("Connot set palm mode because touch is off");
		return -EPERM;
	}

	tcm->enable_fw_palm = cmd->setting == GTI_PALM_ENABLE ? 1 : 0;

	if (tcm->hw_if->bdata_attn.irq_enabled) {
		queue_work(tcm->event_wq, &tcm->set_palm_mode_work);
	} else {
		LOGI("%s firmware palm rejection.\n",
			(tcm->enable_fw_palm & 0x01) ? "Enable" : "Disable");
		syna_tcm_set_dynamic_config(tcm->tcm_dev,
			DC_ENABLE_PALM_REJECTION,
			tcm->enable_fw_palm & 0x01,
			RESP_IN_POLLING);
	}

	return 0;
}

static int syna_get_palm_mode(void *private_data, struct gti_palm_cmd *cmd)
{
	struct syna_tcm *tcm = private_data;
	unsigned short palm_mode;
	int retval;

	if (goog_pm_wake_get_locks(tcm->gti) == 0 || tcm->pwr_state != PWR_ON) {
		LOGI("Connot get palm mode because touch is off");
		return -EPERM;
	}

	retval = syna_tcm_get_dynamic_config(tcm->tcm_dev, DC_ENABLE_PALM_REJECTION,
			&palm_mode, RESP_IN_POLLING);
	if (retval < 0) {
		LOGE("Fail to read palm mode.");
		return retval;
	}

	cmd->setting = palm_mode ? GTI_PALM_ENABLE : GTI_PALM_DISABLE;

	return retval;
}

static void syna_set_palm_mode_work(struct work_struct *work)
{
	struct syna_tcm *tcm = container_of(work, struct syna_tcm, set_palm_mode_work);
	int retval = 0;

	retval = goog_pm_wake_lock(tcm->gti, GTI_PM_WAKELOCK_TYPE_VENDOR_REQUEST, true);
	if (retval) {
		LOGE("Failed to obtain wake lock, ret = %d", retval);
		return;
	}

	LOGI("%s firmware palm rejection.\n",
		(tcm->enable_fw_palm & 0x01) ? "Enable" : "Disable");
	syna_tcm_set_dynamic_config(tcm->tcm_dev,
			DC_ENABLE_PALM_REJECTION,
			tcm->enable_fw_palm & 0x01,
			RESP_IN_ATTN);

	goog_pm_wake_unlock_nosync(tcm->gti, GTI_PM_WAKELOCK_TYPE_VENDOR_REQUEST);
}

static int syna_set_grip_mode(void *private_data, struct gti_grip_cmd *cmd)
{
	struct syna_tcm *tcm = private_data;

	if (goog_pm_wake_get_locks(tcm->gti) == 0 || tcm->pwr_state != PWR_ON) {
		LOGI("Connot set grip mode because touch is off");
		return -EPERM;
	}

	tcm->enable_fw_grip = cmd->setting == GTI_GRIP_ENABLE ? 1 : 0;

	if (tcm->hw_if->bdata_attn.irq_enabled) {
		queue_work(tcm->event_wq, &tcm->set_grip_mode_work);
	} else {
		LOGI("%s firmware grip suppression.\n",
			(tcm->enable_fw_grip & 0x01) ? "Enable" : "Disable");
		syna_tcm_set_dynamic_config(tcm->tcm_dev,
			DC_ENABLE_GRIP_SUPPRESSION,
			tcm->enable_fw_grip & 0x01,
			RESP_IN_POLLING);
	}

	return 0;
}

static int syna_get_grip_mode(void *private_data, struct gti_grip_cmd *cmd)
{
	struct syna_tcm *tcm = private_data;
	unsigned short grip_mode;
	int retval;

	if (goog_pm_wake_get_locks(tcm->gti) == 0 || tcm->pwr_state != PWR_ON) {
		LOGI("Connot get grip mode because touch is off");
		return -EPERM;
	}

	retval = syna_tcm_get_dynamic_config(tcm->tcm_dev, DC_ENABLE_GRIP_SUPPRESSION,
			&grip_mode, RESP_IN_POLLING);
	if (retval < 0) {
		LOGE("Fail to read grip mode.");
		return retval;
	}

	cmd->setting = grip_mode ? GTI_GRIP_ENABLE : GTI_GRIP_DISABLE;

	return retval;
}

static void syna_set_grip_mode_work(struct work_struct *work)
{
	struct syna_tcm *tcm = container_of(work, struct syna_tcm, set_grip_mode_work);
	int retval = 0;

	retval = goog_pm_wake_lock(tcm->gti, GTI_PM_WAKELOCK_TYPE_VENDOR_REQUEST, true);
	if (retval) {
		LOGE("Failed to obtain wake lock, ret = %d", retval);
		return;
	}

	LOGI("%s firmware grip suppression.\n",
		(tcm->enable_fw_grip & 0x01) ? "Enable" : "Disable");
	syna_tcm_set_dynamic_config(tcm->tcm_dev,
			DC_ENABLE_GRIP_SUPPRESSION,
			tcm->enable_fw_grip & 0x01,
			RESP_IN_ATTN);

	goog_pm_wake_unlock_nosync(tcm->gti, GTI_PM_WAKELOCK_TYPE_VENDOR_REQUEST);
}

static int syna_set_heatmap_enabled(void *private_data, struct gti_heatmap_cmd *cmd)
{
	struct syna_tcm *tcm = private_data;

	if (goog_pm_wake_get_locks(tcm->gti) == 0 || tcm->pwr_state != PWR_ON) {
		LOGI("Connot set heatmap mode because touch is off");
		return -EPERM;
	}

	tcm->heatmap_mode = cmd->setting == GTI_HEATMAP_ENABLE ?
			HEATMAP_MODE_COMBINED : HEATMAP_MODE_COORD;

	if (tcm->hw_if->bdata_attn.irq_enabled) {
		queue_work(tcm->event_wq, &tcm->set_heatmap_enabled_work);
	} else {
		LOGI("Set heatmap mode %d.\n", tcm->heatmap_mode);
		syna_tcm_set_dynamic_config(tcm->tcm_dev,
			DC_HEATMAP_MODE,
			tcm->heatmap_mode,
			RESP_IN_POLLING);
	}

	return 0;
}

static void syna_set_heatmap_enabled_work(struct work_struct *work)
{
	struct syna_tcm *tcm = container_of(work, struct syna_tcm, set_heatmap_enabled_work);
	int retval = 0;

	retval = goog_pm_wake_lock(tcm->gti, GTI_PM_WAKELOCK_TYPE_VENDOR_REQUEST, true);
	if (retval) {
		LOGE("Failed to obtain wake lock, ret = %d", retval);
		return;
	}

	LOGI("Set heatmap mode %d.\n", tcm->heatmap_mode);

	syna_tcm_set_dynamic_config(tcm->tcm_dev,
			DC_HEATMAP_MODE,
			tcm->heatmap_mode,
			RESP_IN_ATTN);

	goog_pm_wake_unlock_nosync(tcm->gti, GTI_PM_WAKELOCK_TYPE_VENDOR_REQUEST);
}

static int syna_set_screen_protector_mode(void *private_data,
		struct gti_screen_protector_mode_cmd *cmd)
{
	struct syna_tcm *tcm = private_data;

	if (goog_pm_wake_get_locks(tcm->gti) == 0 || tcm->pwr_state != PWR_ON) {
		LOGI("Connot set screen protector mode because touch is off");
		return -EPERM;
	}

	tcm->high_sensitivity_mode = cmd->setting == GTI_SCREEN_PROTECTOR_MODE_ENABLE ? 1 : 0;

	if (tcm->hw_if->bdata_attn.irq_enabled) {
		queue_work(tcm->event_wq, &tcm->set_screen_protector_mode_work);
	} else {
		LOGI("%s screen protector mode.\n",
				(tcm->enable_fw_grip & 0x01) ? "Enable" : "Disable");
		syna_tcm_set_dynamic_config(tcm->tcm_dev,
				DC_HIGH_SENSITIVIRY_MODE,
				tcm->high_sensitivity_mode,
				RESP_IN_POLLING);
	}

	return 0;
}

static int syna_get_screen_protector_mode(void *private_data,
		struct gti_screen_protector_mode_cmd *cmd)
{
	struct syna_tcm *tcm = private_data;
	unsigned short screen_protector_mode;
	int retval;

	if (goog_pm_wake_get_locks(tcm->gti) == 0 || tcm->pwr_state != PWR_ON) {
		LOGI("Connot get screen protector mode because touch is off");
		return -EPERM;
	}

	retval = syna_tcm_get_dynamic_config(tcm->tcm_dev, DC_HIGH_SENSITIVIRY_MODE,
			&screen_protector_mode, RESP_IN_POLLING);
	if (retval < 0) {
		LOGE("Fail to read screen protector mode.");
		return retval;
	}

	cmd->setting = screen_protector_mode ?
			GTI_SCREEN_PROTECTOR_MODE_ENABLE : GTI_SCREEN_PROTECTOR_MODE_DISABLE;

	return retval;
}

static void syna_set_screen_protector_mode_work(struct work_struct *work)
{
	struct syna_tcm *tcm = container_of(work, struct syna_tcm, set_screen_protector_mode_work);
	int retval = 0;

	retval = goog_pm_wake_lock(tcm->gti, GTI_PM_WAKELOCK_TYPE_VENDOR_REQUEST, true);
	if (retval) {
		LOGE("Failed to obtain wake lock, ret = %d", retval);
		return;
	}

	LOGI("%s screen protector mode.\n",
		tcm->high_sensitivity_mode ? "Enable" : "Disable");
	syna_tcm_set_dynamic_config(tcm->tcm_dev,
			DC_HIGH_SENSITIVIRY_MODE,
			tcm->high_sensitivity_mode,
			RESP_IN_ATTN);

	goog_pm_wake_unlock_nosync(tcm->gti, GTI_PM_WAKELOCK_TYPE_VENDOR_REQUEST);
}

static void syna_gti_init(struct syna_tcm *tcm)
{
	int retval = 0;
	struct gti_optional_configuration *options;
	struct platform_device *pdev = tcm->pdev;
	struct syna_hw_attn_data *attn = &tcm->hw_if->bdata_attn;

	/* release the interrupt and register the gti irq later. */
	syna_dev_release_irq(tcm);

	INIT_WORK(&tcm->set_grip_mode_work, syna_set_grip_mode_work);
	INIT_WORK(&tcm->set_palm_mode_work, syna_set_palm_mode_work);
	INIT_WORK(&tcm->set_heatmap_enabled_work, syna_set_heatmap_enabled_work);
	INIT_WORK(&tcm->set_screen_protector_mode_work, syna_set_screen_protector_mode_work);

	pdev->dev.of_node = pdev->dev.parent->of_node;
	options = devm_kzalloc(&pdev->dev, sizeof(struct gti_optional_configuration), GFP_KERNEL);

	options->get_fw_version = get_fw_version;
	options->get_irq_mode = get_irq_mode;
	options->set_irq_mode = set_irq_mode;
	options->reset = set_reset;
	options->set_grip_mode = syna_set_grip_mode;
	options->get_grip_mode = syna_get_grip_mode;
	options->set_palm_mode = syna_set_palm_mode;
	options->get_palm_mode = syna_get_palm_mode;
	options->set_heatmap_enabled = syna_set_heatmap_enabled;
	options->set_screen_protector_mode = syna_set_screen_protector_mode;
	options->get_screen_protector_mode = syna_get_screen_protector_mode;

	tcm->gti = goog_touch_interface_probe(
		tcm, &pdev->dev, tcm->input_dev, gti_default_handler, options);
	if (!tcm->gti) {
		LOGE("Failed to initialize GTI");
		return;
	}

	retval = goog_pm_register_notification(tcm->gti, &syna_dev_pm_ops);
	if (retval < 0)
		LOGE("Failed to register GTI pm");

	LOGI("Register IRQ by GTI.");
	attn->irq_id = gpio_to_irq(attn->irq_gpio);
	retval = goog_devm_request_threaded_irq(tcm->gti, &tcm->pdev->dev,
			attn->irq_id, syna_dev_isr, syna_dev_interrupt_thread,
			attn->irq_flags, PLATFORM_DRIVER_NAME, tcm);
	if (retval < 0)
		LOGE("Failed to request GTI IRQ");
	else
		attn->irq_enabled = true;

	syna_dev_restore_feature_setting(tcm, RESP_IN_ATTN);
}
#endif

/**
 * syna_dev_restore_feature_setting()
 *
 * Restore the feature settings after the device resume.
 *
 * @param
 *    [ in] tcm: tcm driver handle
 *    [ in] delay_ms_resp: delay time for response reading.
 *                         a positive value presents the time for polling;
 *                         or, set '0' or 'RESP_IN_ATTN' for ATTN driven
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static void syna_dev_restore_feature_setting(struct syna_tcm *tcm, unsigned int delay_ms_resp)
{
	LOGI("Restore touch feature settings.");

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	goog_notify_fw_status_changed(tcm->gti, GTI_FW_STATUS_RESET, NULL);
#endif

	syna_tcm_set_dynamic_config(tcm->tcm_dev,
			DC_COMPRESSION_THRESHOLD,
			tcm->hw_if->compression_threhsold,
			delay_ms_resp);

	syna_tcm_set_dynamic_config(tcm->tcm_dev,
			DC_GRIP_DELTA_THRESHOLD,
			tcm->hw_if->grip_delta_threshold,
			delay_ms_resp);

	syna_tcm_set_dynamic_config(tcm->tcm_dev,
			DC_GRIP_BORDER_THRESHOLD,
			tcm->hw_if->grip_border_threshold,
			delay_ms_resp);

	if (tcm->hw_if->dynamic_report_rate) {
		syna_tcm_set_dynamic_config(tcm->tcm_dev,
				DC_REPORT_RATE_SWITCH,
				tcm->touch_report_rate_config,
				delay_ms_resp);
	}
}

static void syna_motion_filter_work(struct work_struct *work)
{
	struct syna_tcm *tcm = container_of(work, struct syna_tcm, motion_filter_work);

	if (tcm->pwr_state != PWR_ON) {
		LOGI("Touch is already off.");
		return;
	}

	/* Send command to update filter state */
	LOGD("setting motion filter = %s.\n",
		 tcm->set_continuously_report ? "false" : "true");
	syna_tcm_set_dynamic_config(tcm->tcm_dev,
			DC_CONTINUOUSLY_REPORT,
			tcm->set_continuously_report,
			RESP_IN_ATTN);
}

static void syna_set_report_rate_work(struct work_struct *work)
{
	struct syna_tcm *tcm;
	struct delayed_work *delayed_work;
	delayed_work = container_of(work, struct delayed_work, work);
	tcm = container_of(delayed_work, struct syna_tcm, set_report_rate_work);

	if (tcm->pwr_state != PWR_ON) {
		LOGI("Touch is already off.");
		return;
	}

	if (tcm->touch_count != 0) {
		queue_delayed_work(tcm->event_wq, &tcm->set_report_rate_work,
				msecs_to_jiffies(10));
		return;
	}

	tcm->touch_report_rate_config = tcm->next_report_rate_config;
	syna_tcm_set_dynamic_config(tcm->tcm_dev,
			DC_REPORT_RATE_SWITCH,
			tcm->touch_report_rate_config,
			RESP_IN_ATTN);
	LOGI("Set touch report rate as %dHz",
		(tcm->touch_report_rate_config == CONFIG_HIGH_REPORT_RATE) ? 240 : 120);
}

#if defined(ENABLE_HELPER)
/**
 * syna_dev_reset_detected_cb()
 *
 * Callback to assign a task to event workqueue.
 *
 * Please be noted that this function will be invoked in ISR so don't
 * issue another touchcomm command here.
 *
 * @param
 *    [ in] callback_data: pointer to caller data
 *
 * @return
 *    on success, 0 or positive value; otherwise, negative value on error.
 */
static void syna_dev_reset_detected_cb(void *callback_data)
{
	struct syna_tcm *tcm = (struct syna_tcm *)callback_data;

#ifdef RESET_ON_RESUME
	if (tcm->pwr_state != PWR_ON)
		return;
#endif

	if (ATOMIC_GET(tcm->helper.task) == HELP_NONE) {
		ATOMIC_SET(tcm->helper.task, HELP_RESET_DETECTED);

		queue_work(tcm->event_wq, &tcm->helper.work);
	}
}
/**
 * syna_dev_helper_work()
 *
 * According to the given task, perform the delayed work
 *
 * @param
 *    [ in] work: data for work used
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static void syna_dev_helper_work(struct work_struct *work)
{
	unsigned char task;
	struct syna_tcm_helper *helper =
			container_of(work, struct syna_tcm_helper, work);
	struct syna_tcm *tcm =
			container_of(helper, struct syna_tcm, helper);

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	if (goog_pm_wake_get_locks(tcm->gti) == 0 || tcm->pwr_state != PWR_ON) {
#else
	if (tcm->pwr_state != PWR_ON) {
#endif
		LOGI("Touch is already off.");
		goto exit;
	}

	task = ATOMIC_GET(helper->task);

	switch (task) {
	case HELP_RESET_DETECTED:
		LOGI("Reset caught (device mode:0x%x)\n", tcm->tcm_dev->dev_mode);
		syna_dev_restore_feature_setting(tcm, RESP_IN_ATTN);
		break;
	default:
		break;
	}

exit:
	ATOMIC_SET(helper->task, HELP_NONE);
}
#endif

#ifdef ENABLE_CUSTOM_TOUCH_ENTITY
/**
 * syna_dev_parse_custom_touch_data_cb()
 *
 * Callback to parse the custom or non-standard touch entity from the
 * touch report.
 *
 * Please be noted that this function will be invoked in ISR so don't
 * issue another touchcomm command here.
 * If really needed, please assign a task to helper thread.
 *
 * @param
 *    [ in]    code:          the code of current touch entity
 *    [ in]    config:        the report configuration stored
 *    [in/out] config_offset: offset of current position in report config,
 *                            and then return the updated position.
 *    [ in]    report:        touch report given
 *    [in/out] report_offset: offset of current position in touch report,
 *                            the updated position should be returned.
 *    [ in]    report_size:   size of given touch report
 *    [ in]    callback_data: pointer to caller data passed to callback function
 *
 * @return
 *    on success, 0 or positive value; otherwise, negative value on error.
 */
static int syna_dev_parse_custom_touch_data_cb(const unsigned char code,
		const unsigned char *config, unsigned int *config_offset,
		const unsigned char *report, unsigned int *report_offset,
		unsigned int report_size, void *callback_data)
{
	struct syna_tcm *tcm = (struct syna_tcm *)callback_data;
	struct tcm_touch_data_blob *touch_data;
	struct tcm_objects_data_blob *object_data;
	unsigned int data;
	unsigned int bits;

	touch_data = &tcm->tp_data;
	object_data = touch_data->object_data;

	switch (code) {
	case TOUCH_ENTITY_CUSTOM_ANGLE:
		bits = config[(*config_offset)++];
		syna_tcm_get_touch_data(report, report_size,
				*report_offset, bits, &data);

		object_data[touch_data->obji].custom_data[CUSTOM_DATA_ANGLE] = data;

		*report_offset += bits;
		return bits;
	case TOUCH_ENTITY_CUSTOM_MAJOR:
		bits = config[(*config_offset)++];
		syna_tcm_get_touch_data(report, report_size,
				*report_offset, bits, &data);

		object_data[touch_data->obji].custom_data[CUSTOM_DATA_MAJOR] = data;

		*report_offset += bits;
		return bits;
	case TOUCH_ENTITY_CUSTOM_MINOR:
		bits = config[(*config_offset)++];
		syna_tcm_get_touch_data(report, report_size,
				*report_offset, bits, &data);

		object_data[touch_data->obji].custom_data[CUSTOM_DATA_MINOR] = data;
		*report_offset += bits;
		return bits;
	default:
		LOGW("Unknown touch config code (idx:%d 0x%02x)\n",
			*config_offset, code);
		return (-EINVAL);
	}

	return (-EINVAL);
}
#endif

#if defined(ENABLE_WAKEUP_GESTURE)
/**
 * syna_dev_parse_custom_gesture_cb()
 *
 * Callback to parse the custom or non-standard gesture data from the
 * touch report.
 *
 * Please be noted that this function will be invoked in ISR so don't
 * issue another touchcomm command here.
 * If really needed, please assign a task to helper thread.
 *
 * @param
 *    [ in]    code:          the code of current touch entity
 *    [ in]    config:        the report configuration stored
 *    [in/out] config_offset: offset of current position in report config,
 *                            the updated position should be returned.
 *    [ in]    report:        touch report given
 *    [in/out] report_offset: offset of current position in touch report,
 *                            the updated position should be returned.
 *    [ in]    report_size:   size of given touch report
 *    [ in]    callback_data: pointer to caller data
 *
 * @return
 *    on success, 0 or positive value; otherwise, negative value on error.
 */
static int syna_dev_parse_custom_gesture_cb(const unsigned char code,
		const unsigned char *config, unsigned int *config_offset,
		const unsigned char *report, unsigned int *report_offset,
		unsigned int report_size, void *callback_data)
{
	struct syna_tcm *tcm = (struct syna_tcm *)callback_data;
	unsigned int data;
	unsigned int bits;
	unsigned int offset = *report_offset;
	struct custom_gesture_data {
		unsigned short x;
		unsigned short y;
		unsigned char major;
		unsigned char minor;
		unsigned char angle;
	} __packed;
	struct custom_gesture_data g_pos = {0};

	bits = config[(*config_offset)++];

	if (code == TOUCH_REPORT_GESTURE_ID) {
		syna_tcm_get_touch_data(report, report_size, offset, bits, &data);

		switch (data) {
		case GESTURE_SINGLE_TAP:
			LOGI("Gesture single tap detected\n");
			break;
		case GESTURE_LONG_PRESS:
			LOGI("Gesture long press detected\n");
			break;
		default:
			LOGW("Unknown gesture id %d\n", data);
			break;
		}

		*report_offset += bits;

	} else if (code == TOUCH_REPORT_GESTURE_DATA) {
		if (bits != (sizeof(struct custom_gesture_data) * 8)) {
			LOGE("Invalid size of gesture data %d (expected:%d)\n",
				bits, (int)(sizeof(struct custom_gesture_data) * 8));
			return -EINVAL;
		}

		syna_tcm_get_touch_data(report, report_size, offset, 16, &data);
		g_pos.x = (unsigned short)data;
		offset += 16;

		syna_tcm_get_touch_data(report, report_size, offset, 16, &data);
		g_pos.y = (unsigned short)data;
		offset += 16;

		syna_tcm_get_touch_data(report, report_size, offset, 8, &data);
		g_pos.major = (unsigned short)data;
		offset += 8;

		syna_tcm_get_touch_data(report, report_size, offset, 8, &data);
		g_pos.minor = (unsigned short)data;
		offset += 8;

		syna_tcm_get_touch_data(report, report_size, offset, 8, &data);
		g_pos.angle = (unsigned short)data;
		offset += 8;

		*report_offset += bits;

		LOGI("Gesture data x:%d y:%d major:%d minor:%d angle:%d\n",
			g_pos.x, g_pos.y, g_pos.major, g_pos.minor, g_pos.angle);
	} else {
		return -EINVAL;
	}

	return bits;
}
#endif

/**
 * syna_tcm_free_input_events()
 *
 * Clear all relevant touched events.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    none.
 */
#if !IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
static void syna_dev_free_input_events(struct syna_tcm *tcm)
{
	struct input_dev *input_dev = tcm->input_dev;
#ifdef TYPE_B_PROTOCOL
	unsigned int idx;
#endif

	if (input_dev == NULL)
		return;

	syna_pal_mutex_lock(&tcm->tp_event_mutex);

#ifdef TYPE_B_PROTOCOL
	for (idx = 0; idx < MAX_NUM_OBJECTS; idx++) {
		input_mt_slot(input_dev, idx);
		input_report_abs(input_dev, ABS_MT_PRESSURE, 0);
		input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, 0);
	}
#endif
	input_report_key(input_dev, BTN_TOUCH, 0);
	input_report_key(input_dev, BTN_TOOL_FINGER, 0);
#ifndef TYPE_B_PROTOCOL
	input_mt_sync(input_dev);
#endif
	input_sync(input_dev);

	syna_pal_mutex_unlock(&tcm->tp_event_mutex);
}
#endif

/**
 * syna_dev_report_input_events()
 *
 * Report touched events to the input subsystem.
 *
 * After syna_tcm_get_event_data() function and the touched data is ready,
 * this function can be called to report an input event.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    none.
 */
static void syna_dev_report_input_events(struct syna_tcm *tcm)
{
	unsigned int idx;
	unsigned int x;
	unsigned int y;
	unsigned int z;
#ifdef ENABLE_CUSTOM_TOUCH_ENTITY
	int major;
	int minor;
	int angle;
#else
	int wx;
	int wy;
#endif

	unsigned int status;
	unsigned int touch_count;
	struct input_dev *input_dev = tcm->input_dev;
	unsigned int max_objects = tcm->tcm_dev->max_objects;
	struct tcm_touch_data_blob __maybe_unused *touch_data;
	struct tcm_objects_data_blob *object_data;

	if (input_dev == NULL)
		return;

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	goog_input_lock(tcm->gti);
#else
	syna_pal_mutex_lock(&tcm->tp_event_mutex);
#endif

	object_data = &tcm->tp_data.object_data[0];

#ifdef ENABLE_WAKEUP_GESTURE
	touch_data = &tcm->tp_data;
	if ((tcm->pwr_state == LOW_PWR) && tcm->irq_wake) {
		if (touch_data->gesture_id) {
			LOGD("Gesture detected, id:%d\n",
				touch_data->gesture_id);
#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
			goog_input_report_key(tcm->gti, input_dev, KEY_WAKEUP, 1);
			goog_input_sync(tcm->gti, input_dev);
			goog_input_report_key(tcm->gti, input_dev, KEY_WAKEUP, 0);
			goog_input_sync(tcm->gti, input_dev);
#else
			input_report_key(input_dev, KEY_WAKEUP, 1);
			input_sync(input_dev);
			input_report_key(input_dev, KEY_WAKEUP, 0);
			input_sync(input_dev);
#endif
		}
	}
#endif

	if (tcm->pwr_state == LOW_PWR)
		goto exit;

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	goog_input_set_timestamp(tcm->gti, input_dev, tcm->timestamp);
#endif

	touch_count = 0;

	for (idx = 0; idx < max_objects; idx++) {
		if (tcm->prev_obj_status[idx] == LIFT &&
				object_data[idx].status == LIFT)
			status = NOP;
		else
			status = object_data[idx].status;

		switch (status) {
		case LIFT:
#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
			goog_input_mt_slot(tcm->gti, input_dev, idx);
			goog_input_report_abs(tcm->gti, input_dev, ABS_MT_PRESSURE, 0);
			goog_input_mt_report_slot_state(tcm->gti, input_dev,
					MT_TOOL_FINGER, 0);
#else
#ifdef TYPE_B_PROTOCOL
			input_mt_slot(input_dev, idx);
			input_report_abs(input_dev, ABS_MT_PRESSURE, 0);
			input_mt_report_slot_state(input_dev,
					MT_TOOL_FINGER, 0);
#endif
#endif
			break;
		case FINGER:
		case GLOVED_OBJECT:
		case PALM:
			x = object_data[idx].x_pos;
			y = object_data[idx].y_pos;
#ifdef ENABLE_CUSTOM_TOUCH_ENTITY
			major = object_data[idx].custom_data[CUSTOM_DATA_MAJOR];
			minor = object_data[idx].custom_data[CUSTOM_DATA_MINOR];
			angle = object_data[idx].custom_data[CUSTOM_DATA_ANGLE];
			LOGD("Finger %d: major = %d, minor = %d, angle = %d.\n",
				idx, major, minor, (s8) angle);
			/* Report major and minor in display pixels. */
			major = major * tcm->hw_if->pixels_per_mm;
			minor = minor * tcm->hw_if->pixels_per_mm;
#else
			wx = object_data[idx].x_width;
			wy = object_data[idx].y_width;
			/* Report major and minor in display pixels. */
			wx = wx * tcm->hw_if->pixels_per_mm;
			wy = wy * tcm->hw_if->pixels_per_mm;
#endif

			if (object_data[idx].z == 0) {
				z = 1;
				LOGW("Get a touch coordinate with pressure = 0");
			} else {
				z = object_data[idx].z;
			}
#ifdef REPORT_SWAP_XY
			x = x ^ y;
			y = x ^ y;
			x = x ^ y;
#endif
#ifdef REPORT_FLIP_X
			x = tcm->input_dev_params.max_x - x;
#endif
#ifdef REPORT_FLIP_Y
			y = tcm->input_dev_params.max_y - y;
#endif

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
			goog_input_mt_slot(tcm->gti, input_dev, idx);
			goog_input_mt_report_slot_state(tcm->gti, input_dev, MT_TOOL_FINGER, 1);
			goog_input_report_key(tcm->gti, input_dev, BTN_TOUCH, 1);
			goog_input_report_key(tcm->gti, input_dev, BTN_TOOL_FINGER, 1);
			goog_input_report_abs(tcm->gti, input_dev, ABS_MT_POSITION_X, x);
			goog_input_report_abs(tcm->gti, input_dev, ABS_MT_POSITION_Y, y);
			goog_input_report_abs(tcm->gti, input_dev, ABS_MT_PRESSURE, z);
#ifdef ENABLE_CUSTOM_TOUCH_ENTITY
			goog_input_report_abs(tcm->gti, input_dev, ABS_MT_TOUCH_MAJOR, major);
			goog_input_report_abs(tcm->gti, input_dev, ABS_MT_TOUCH_MINOR, minor);
			goog_input_report_abs(tcm->gti, input_dev,
					ABS_MT_ORIENTATION, (s16) (((s8) angle) * 2048 / 45));
#else
			goog_input_report_abs(tcm->gti, input_dev,
					ABS_MT_TOUCH_MAJOR, MAX(wx, wy));
			goog_input_report_abs(tcm->gti, input_dev,
					ABS_MT_TOUCH_MINOR, MIN(wx, wy));
#endif
#else
#ifdef TYPE_B_PROTOCOL
			input_mt_slot(input_dev, idx);
			input_mt_report_slot_state(input_dev,
					MT_TOOL_FINGER, 1);
#endif
			input_report_key(input_dev, BTN_TOUCH, 1);
			input_report_key(input_dev, BTN_TOOL_FINGER, 1);
			input_report_abs(input_dev, ABS_MT_POSITION_X, x);
			input_report_abs(input_dev, ABS_MT_POSITION_Y, y);
			input_report_abs(input_dev, ABS_MT_PRESSURE, z);
#ifdef REPORT_TOUCH_WIDTH
#ifdef ENABLE_CUSTOM_TOUCH_ENTITY
			input_report_abs(input_dev,
					ABS_MT_TOUCH_MAJOR, major);
			input_report_abs(input_dev,
					ABS_MT_TOUCH_MINOR, minor);
			input_report_abs(input_dev,
					ABS_MT_ORIENTATION, (s16) (((s8) angle) * 2048 / 45));
#else
			input_report_abs(input_dev,
					ABS_MT_TOUCH_MAJOR, MAX(wx, wy));
			input_report_abs(input_dev,
					ABS_MT_TOUCH_MINOR, MIN(wx, wy));
#endif
#endif
#ifndef TYPE_B_PROTOCOL
			input_mt_sync(input_dev);
#endif
#endif

			LOGD("Finger %d: x = %d, y = %d, z = %d\n", idx, x, y, z);
			touch_count++;
			break;
		default:
			break;
		}

		tcm->prev_obj_status[idx] = object_data[idx].status;
	}

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	if (touch_count == 0) {
		goog_input_report_key(tcm->gti, input_dev, BTN_TOUCH, 0);
		goog_input_report_key(tcm->gti, input_dev, BTN_TOOL_FINGER, 0);
	}

	goog_input_sync(tcm->gti, input_dev);
#else
	if (touch_count == 0) {
		input_report_key(input_dev, BTN_TOUCH, 0);
		input_report_key(input_dev, BTN_TOOL_FINGER, 0);
#ifndef TYPE_B_PROTOCOL
		input_mt_sync(input_dev);
#endif
	}

	input_set_timestamp(input_dev, tcm->timestamp);
	input_sync(input_dev);
#endif
	tcm->touch_count = touch_count;

exit:
#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	goog_input_unlock(tcm->gti);
#else
	syna_pal_mutex_unlock(&tcm->tp_event_mutex);
#endif
}

/**
 * syna_dev_create_input_device()
 *
 * Allocate an input device and set up relevant parameters to the
 * input subsystem.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_create_input_device(struct syna_tcm *tcm)
{
	int retval = 0;
	struct tcm_dev *tcm_dev = tcm->tcm_dev;
	struct input_dev *input_dev = NULL;
#ifdef DEV_MANAGED_API
	struct device *dev = syna_request_managed_device();

	if (!dev) {
		LOGE("Invalid managed device\n");
		return -EINVAL;
	}

	input_dev = devm_input_allocate_device(dev);
#else /* Legacy API */
	input_dev = input_allocate_device();
#endif
	if (input_dev == NULL) {
		LOGE("Fail to allocate input device\n");
		return -ENODEV;
	}

	input_dev->name = TOUCH_INPUT_NAME;
	input_dev->phys = TOUCH_INPUT_PHYS_PATH;
	input_dev->id.product = SYNAPTICS_TCM_DRIVER_ID;
	input_dev->id.version = SYNAPTICS_TCM_DRIVER_VERSION;
	input_dev->dev.parent = tcm->pdev->dev.parent;
	input_set_drvdata(input_dev, tcm);

	set_bit(EV_SYN, input_dev->evbit);
	set_bit(EV_KEY, input_dev->evbit);
	set_bit(EV_ABS, input_dev->evbit);
	set_bit(BTN_TOUCH, input_dev->keybit);
	set_bit(BTN_TOOL_FINGER, input_dev->keybit);
#ifdef INPUT_PROP_DIRECT
	set_bit(INPUT_PROP_DIRECT, input_dev->propbit);
#endif

#ifdef ENABLE_WAKEUP_GESTURE
	set_bit(KEY_WAKEUP, input_dev->keybit);
	input_set_capability(input_dev, EV_KEY, KEY_WAKEUP);
#endif

	input_set_abs_params(input_dev,
			ABS_MT_POSITION_X, 0, tcm_dev->max_x, 0, 0);
	input_set_abs_params(input_dev,
			ABS_MT_POSITION_Y, 0, tcm_dev->max_y, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 255, 0, 0);

	input_mt_init_slots(input_dev, tcm_dev->max_objects,
			INPUT_MT_DIRECT);

#ifdef REPORT_TOUCH_WIDTH
	input_set_abs_params(input_dev,
			ABS_MT_TOUCH_MAJOR, 0, tcm_dev->max_x, 0, 0);
	input_set_abs_params(input_dev,
			ABS_MT_TOUCH_MINOR, 0, tcm_dev->max_y, 0, 0);
#ifdef ENABLE_CUSTOM_TOUCH_ENTITY
	input_set_abs_params(input_dev,
			ABS_MT_ORIENTATION, -4096, 4096, 0, 0);
#endif
#endif

	input_set_abs_params(input_dev, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER,
			MT_TOOL_PALM, 0, 0);

	tcm->input_dev_params.max_x = tcm_dev->max_x;
	tcm->input_dev_params.max_y = tcm_dev->max_y;
	tcm->input_dev_params.max_objects = tcm_dev->max_objects;

	retval = input_register_device(input_dev);
	if (retval < 0) {
		LOGE("Fail to register input device\n");
		input_free_device(input_dev);
		input_dev = NULL;
		return retval;
	}

	tcm->input_dev = input_dev;

	return 0;
}

/**
 * syna_dev_release_input_device()
 *
 * Release an input device allocated previously.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    none.
 */
static void syna_dev_release_input_device(struct syna_tcm *tcm)
{
	if (!tcm->input_dev)
		return;

	input_unregister_device(tcm->input_dev);

	tcm->input_dev = NULL;
}

/**
 * syna_dev_check_input_params()
 *
 * Check if any of the input parameters registered to the input subsystem
 * has changed.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    positive value to indicate mismatching parameters; otherwise, return 0.
 */
static int syna_dev_check_input_params(struct syna_tcm *tcm)
{
	struct tcm_dev *tcm_dev = tcm->tcm_dev;

	if (tcm_dev->max_x == 0 && tcm_dev->max_y == 0)
		return 0;

	if (tcm->input_dev_params.max_x != tcm_dev->max_x)
		return 1;

	if (tcm->input_dev_params.max_y != tcm_dev->max_y)
		return 1;

	if (tcm->input_dev_params.max_objects != tcm_dev->max_objects)
		return 1;

	if (tcm_dev->max_objects > MAX_NUM_OBJECTS) {
		LOGW("Out of max num objects defined, in app_info: %d\n",
			tcm_dev->max_objects);
		return 0;
	}

	LOGN("Input parameters unchanged\n");

	return 0;
}

/**
 * syna_dev_set_up_input_device()
 *
 * Set up input device to the input subsystem by confirming the supported
 * parameters and creating the device.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_set_up_input_device(struct syna_tcm *tcm)
{
	int retval = 0;

	if (IS_NOT_APP_FW_MODE(tcm->tcm_dev->dev_mode)) {
		LOGN("Application firmware not running, current mode: %02x\n",
			tcm->tcm_dev->dev_mode);
		return 0;
	}

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	if (tcm->gti)
		goog_input_lock(tcm->gti);
#else
	syna_dev_free_input_events(tcm);

	syna_pal_mutex_lock(&tcm->tp_event_mutex);
#endif

	retval = syna_dev_check_input_params(tcm);
	if (retval == 0)
		goto exit;

	if (tcm->input_dev != NULL)
		syna_dev_release_input_device(tcm);

	retval = syna_dev_create_input_device(tcm);
	if (retval < 0) {
		LOGE("Fail to create input device\n");
		goto exit;
	}

exit:
#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	if (tcm->gti)
		goog_input_unlock(tcm->gti);
#else
	syna_pal_mutex_unlock(&tcm->tp_event_mutex);
#endif

	return retval;
}

#if 0
static int syna_dev_ptflib_decoder(struct syna_tcm *tcm, const u16 *in_array,
				   const int in_array_size, u16 *out_array,
				   const int out_array_max_size)
{
	const u16 ESCAPE_MASK = 0xF000;
	const u16 ESCAPE_BIT = 0x8000;

	int i;
	int j;
	int out_array_size = 0;
	u16 prev_word = 0;
	u16 repetition = 0;
	u16 *temp_out_array = out_array;

	for (i = 0; i < in_array_size; i++) {
		u16 curr_word = in_array[i];
		if ((curr_word & ESCAPE_MASK) == ESCAPE_BIT) {
			repetition = (curr_word & ~ESCAPE_MASK);
			if (out_array_size + repetition > out_array_max_size)
				break;
			for (j = 0; j < repetition; j++) {
				*temp_out_array++ = prev_word;
				out_array_size++;
			}
		} else {
			if (out_array_size >= out_array_max_size)
				break;
			*temp_out_array++ = curr_word;
			out_array_size++;
			prev_word = curr_word;
		}
	}

	if (i != in_array_size || out_array_size != out_array_max_size) {
		LOGE("%d (in=%d, out=%d, rep=%d, out_max=%d).\n",
			i, in_array_size, out_array_size,
			repetition, out_array_max_size);
		memset(out_array, 0, out_array_max_size * sizeof(u16));
		return -1;
	}

	return out_array_size;
}
#endif

static irqreturn_t syna_dev_isr(int irq, void *handle)
{
	struct syna_tcm *tcm = handle;

	tcm->timestamp = ktime_get();

	return IRQ_WAKE_THREAD;
}

/**
 * syna_dev_interrupt_thread()
 *
 * This is the function to be called when the interrupt is asserted.
 * The purposes of this handler is to read events generated by device and
 * retrieve all enqueued messages until ATTN is no longer asserted.
 *
 * @param
 *    [ in] irq:  interrupt line
 *    [ in] data: private data being passed to the handler function
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static irqreturn_t syna_dev_interrupt_thread(int irq, void *data)
{
	int retval;
	unsigned char code = 0;
	struct syna_tcm *tcm = data;
	struct custom_fw_status *status;
	struct syna_hw_attn_data *attn = &tcm->hw_if->bdata_attn;
	struct tcm_dev *tcm_dev = tcm->tcm_dev;
	unsigned char *touch_data = 0;
	unsigned short touch_data_size = 0;
	unsigned char *heatmap_data_start = 0;
	unsigned char *heatmap_data = 0;
	unsigned short heatmap_data_size = 0;

	if (unlikely(gpio_get_value(attn->irq_gpio) != attn->irq_on_state))
		goto exit;

	tcm->isr_pid = current->pid;

	/* retrieve the original report date generated by firmware */
	retval = syna_tcm_get_event_data(tcm->tcm_dev,
			&code,
			&tcm->event_data);
	if (retval < 0) {
		LOGE("Fail to get event data\n");
		goto exit;
	}

	tcm->is_attn_asserted = true;

#ifdef ENABLE_EXTERNAL_FRAME_PROCESS
	if (tcm->report_to_queue[code] == EFP_ENABLE) {
		syna_tcm_buf_lock(&tcm->tcm_dev->external_buf);
		syna_cdev_update_report_queue(tcm, code,
		    &tcm->tcm_dev->external_buf);
		syna_tcm_buf_unlock(&tcm->tcm_dev->external_buf);
#ifndef REPORT_CONCURRENTLY
		goto exit;
#endif
	}
#endif
	/* report input event only when receiving a touch report */
	if (code == REPORT_TOUCH) {
		/* parse touch report once received */
		retval = syna_tcm_parse_touch_report(tcm->tcm_dev,
				tcm->event_data.buf,
				tcm->event_data.data_length,
				&tcm->tp_data);
		if (retval < 0) {
			LOGE("Fail to parse touch report\n");
			goto exit;
		}

		/* forward the touch event to system */
		syna_dev_report_input_events(tcm);
	} else if (code == tcm->raw_data_report_code) {
		if (!tcm->raw_data_buffer) {
			tcm->raw_data_buffer = kmalloc(
					       sizeof(u16) * (tcm_dev->rows * tcm_dev->cols +
							      tcm_dev->rows + tcm_dev->cols),
					       GFP_KERNEL);
			if (!tcm->raw_data_buffer) {
				LOGE("Allocate raw_data_buffer failed\n");
				goto exit;
			}
		}
		if (tcm->event_data.data_length == sizeof(u16) * (tcm_dev->rows * tcm_dev->cols +
								  tcm_dev->rows + tcm_dev->cols)) {
			memcpy(tcm->raw_data_buffer, tcm->event_data.buf,
			       tcm->event_data.data_length);
			complete_all(&tcm->raw_data_completion);
		} else {
			LOGE("Raw data length: %d is incorrect.\n", tcm->event_data.data_length);
		}
	}

	/* handling the particular report data */
	switch (code) {
	case REPORT_HEAT_MAP:
		/* for 'heat map' ($c3) report,
		 * report data has been stored at tcm->event_data.buf;
		 * while, tcm->event_data.data_length is the size of data
		 */
		LOGD("Heat map data received, size:%d\n",
			tcm->event_data.data_length);
		break;
	case REPORT_TOUCH_AND_HEATMAP:
		/* parse $c5 report containing touch and heatmap data */

		/* touch data */
		touch_data_size = (tcm->event_data.buf[1] << 8) | tcm->event_data.buf[0];
		touch_data = tcm->event_data.buf + 2;

		retval = syna_tcm_parse_touch_report(tcm->tcm_dev,
				touch_data,
				touch_data_size,
				&tcm->tp_data);
		if (retval < 0) {
			LOGE("Fail to parse touch report\n");
			goto exit;
		}
		/* forward the touch event to system */
		syna_dev_report_input_events(tcm);

		/* heatmap data */
		heatmap_data_start = touch_data + touch_data_size;
		heatmap_data_size = (heatmap_data_start[1] << 8) | heatmap_data_start[0];
		heatmap_data = touch_data + touch_data_size + 2;

		LOGD("$c5 Heat map data received, size:%d\n", heatmap_data_size);

		break;
	case REPORT_FW_STATUS:
		/* for 'fw status' ($c2) report,
		 * report size shall be 2-byte only; the
		 */
		status = (struct custom_fw_status *)&tcm->event_data.buf[0];
		LOGI("Status: moisture:%d noise:%d freq-change:%d, grip:%d, palm:%d, "
			"fast relax:%d\n",
			status->b0_moisture, status->b1_noise_state,
			status->b2_freq_hopping, status->b3_grip, status->b4_palm,
			status->b5_fast_relaxation);
		break;
	default:
		break;
	}

exit:
	return IRQ_HANDLED;
}

/**
 * syna_dev_request_irq()
 *
 * Allocate an interrupt line and register the ISR handler
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_request_irq(struct syna_tcm *tcm)
{
	int retval;
	struct syna_hw_attn_data *attn = &tcm->hw_if->bdata_attn;
#ifdef DEV_MANAGED_API
	struct device *dev = syna_request_managed_device();

	if (!dev) {
		LOGE("Invalid managed device\n");
		retval = -EINVAL;
		goto exit;
	}
#endif

	if (attn->irq_gpio < 0) {
		LOGE("Invalid IRQ GPIO\n");
		retval = -EINVAL;
		goto exit;
	}

	attn->irq_id = gpio_to_irq(attn->irq_gpio);

#ifdef DEV_MANAGED_API
	retval = devm_request_threaded_irq(dev,
			attn->irq_id,
			syna_dev_isr,
			syna_dev_interrupt_thread,
			attn->irq_flags,
			PLATFORM_DRIVER_NAME,
			tcm);
#else /* Legacy API */
	retval = request_threaded_irq(attn->irq_id,
			syna_dev_isr,
			syna_dev_interrupt_thread,
			attn->irq_flags,
			PLATFORM_DRIVER_NAME,
			tcm);
#endif
	if (retval < 0) {
		LOGE("Fail to request threaded irq\n");
		goto exit;
	}

	attn->irq_enabled = true;

	LOGI("Interrupt handler registered\n");

exit:
	return retval;
}

/**
 * syna_dev_release_irq()
 *
 * Release an interrupt line allocated previously
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    none.
 */
static void syna_dev_release_irq(struct syna_tcm *tcm)
{
	struct syna_hw_attn_data *attn = &tcm->hw_if->bdata_attn;
#ifdef DEV_MANAGED_API
	struct device *dev = syna_request_managed_device();

	if (!dev) {
		LOGE("Invalid managed device\n");
		return;
	}
#endif

	if (attn->irq_id <= 0)
		return;

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	syna_pal_mutex_lock(&attn->irq_en_mutex);
	disable_irq(attn->irq_id);
	syna_pal_mutex_unlock(&attn->irq_en_mutex);
#else
	if (tcm->hw_if->ops_enable_irq)
		tcm->hw_if->ops_enable_irq(tcm->hw_if, false);
#endif

#ifdef DEV_MANAGED_API
#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	if (tcm->gti)
		goog_devm_free_irq(tcm->gti, &tcm->pdev->dev, attn->irq_id);
	else
		devm_free_irq(dev, attn->irq_id, tcm);
#else
	devm_free_irq(dev, attn->irq_id, tcm);
#endif
#else
	free_irq(attn->irq_id, tcm);
#endif

	attn->irq_id = 0;
	attn->irq_enabled = false;

	LOGI("Interrupt handler released\n");
}

/**
 * syna_dev_set_up_app_fw()
 *
 * Implement the essential steps for the initialization including the
 * preparation of app info and the configuration of touch report.
 *
 * This function should be called whenever the device initially powers
 * up, resets, or firmware update.
 *
 * @param
 *    [ in] tcm: tcm driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_set_up_app_fw(struct syna_tcm *tcm)
{
	int retval = 0;
	struct tcm_dev *tcm_dev = tcm->tcm_dev;

	if (IS_NOT_APP_FW_MODE(tcm_dev->dev_mode)) {
		LOGN("Application firmware not running, current mode: %02x\n",
			tcm_dev->dev_mode);
		return -EINVAL;
	}

	/* collect app info containing most of sensor information */
	retval = syna_tcm_get_app_info(tcm_dev, &tcm_dev->app_info);
	if (retval < 0) {
		LOGE("Fail to get application info\n");
		return retval;
	}

	/* set up the format of touch report */
#ifdef USE_CUSTOM_TOUCH_REPORT_CONFIG
	retval = syna_tcm_set_touch_report_config(tcm_dev,
			custom_touch_format,
			(unsigned int)sizeof(custom_touch_format));
	if (retval < 0) {
		LOGE("Fail to setup the custom touch report format\n");
		return retval;
	}
#endif
	/* preserve the format of touch report */
	retval = syna_tcm_preserve_touch_report_config(tcm_dev);
	if (retval < 0) {
		LOGE("Fail to preserve touch report config\n");
		return retval;
	}

#ifdef ENABLE_CUSTOM_TOUCH_ENTITY
	/* set up custom touch data parsing method */
	retval = syna_tcm_set_custom_touch_entity_callback(tcm_dev,
			syna_dev_parse_custom_touch_data_cb,
			(void *)tcm);
	if (retval < 0) {
		LOGE("Fail to set up custom touch data parsing method\n");
		return retval;
	}
#endif
#ifdef ENABLE_WAKEUP_GESTURE
	/* set up custom gesture parsing method */
	retval = syna_tcm_set_custom_gesture_callback(tcm_dev,
			syna_dev_parse_custom_gesture_cb,
			(void *)tcm);
	if (retval < 0) {
		LOGE("Fail to set up custom gesture parsing method\n");
		return retval;
	}
#endif
	return 0;
}

#ifdef STARTUP_REFLASH
/**
 * syna_dev_reflash_startup_work()
 *
 * Perform firmware update during system startup.
 * Function is available when the 'STARTUP_REFLASH' configuration
 * is enabled.
 *
 * @param
 *    [ in] work: handle of work structure
 *
 * @return
 *    none.
 */
static void syna_dev_reflash_startup_work(struct work_struct *work)
{
	int retval;
	struct delayed_work *delayed_work;
	struct syna_tcm *tcm;
	struct tcm_dev *tcm_dev;
	const struct firmware *fw_entry = NULL;
	const unsigned char *fw_image = NULL;
	unsigned int fw_image_size;

	delayed_work = container_of(work, struct delayed_work, work);
	tcm = container_of(delayed_work, struct syna_tcm, reflash_work);

	tcm_dev = tcm->tcm_dev;

	pm_stay_awake(&tcm->pdev->dev);

	/* get firmware image */
	retval = request_firmware(&fw_entry,
			tcm->hw_if->fw_name,
			tcm->pdev->dev.parent);
	if (retval < 0) {
		LOGE("Fail to request %s\n", tcm->hw_if->fw_name);
		goto exit;
	}

	fw_image = fw_entry->data;
	fw_image_size = fw_entry->size;

	LOGD("Firmware image size = %d\n", fw_image_size);

	/* perform fw update */
#ifdef MULTICHIP_DUT_REFLASH
	/* do firmware update for the multichip-based device */
	retval = syna_tcm_romboot_do_multichip_reflash(tcm_dev,
			fw_image,
			fw_image_size,
			RESP_IN_ATTN,
			tcm->force_reflash);
#else
	/* do firmware update for the common device */
	retval = syna_tcm_do_fw_update(tcm_dev,
			fw_image,
			fw_image_size,
			RESP_IN_ATTN,
			tcm->force_reflash);
#endif
	if (retval < 0) {
		LOGE("Fail to do reflash, reflash_count = %d\n", tcm->reflash_count);
		tcm->force_reflash = true;
		if (tcm->reflash_count++ < 3) {
			queue_delayed_work(tcm->reflash_workqueue, &tcm->reflash_work,
				msecs_to_jiffies(STARTUP_REFLASH_DELAY_TIME_MS));
		}
		goto exit;
	}

	/* re-initialize the app fw */
	retval = syna_dev_set_up_app_fw(tcm);
	if (retval < 0) {
		LOGE("Fail to set up app fw after fw update\n");
		goto exit;
	}

	/* ensure the settings of input device
	 * if needed, re-create a new input device
	 */
	retval = syna_dev_set_up_input_device(tcm);
	if (retval < 0) {
		LOGE("Fail to register input device\n");
		goto exit;
	}

	syna_gti_init(tcm);
exit:
	fw_image = NULL;

	if (fw_entry) {
		release_firmware(fw_entry);
		fw_entry = NULL;
	}

	pm_relax(&tcm->pdev->dev);
}
#endif
#if defined(POWER_ALIVE_AT_SUSPEND) && !defined(RESET_ON_RESUME)
/**
 * syna_dev_enter_normal_sensing()
 *
 * Helper to enter normal sensing mode
 *
 * @param
 *    [ in] tcm: tcm driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_enter_normal_sensing(struct syna_tcm *tcm)
{
	int retval = 0;
#ifdef GOOG_INT2_FEATURE
	unsigned short enable;
#endif

	if (!tcm)
		return -EINVAL;

	/* bring out of sleep mode. */
	retval = syna_tcm_sleep(tcm->tcm_dev, false);
	if (retval < 0) {
		LOGE("Fail to exit deep sleep\n");
		return retval;
	}

	/* disable low power gesture mode, if needed */
	if (tcm->lpwg_enabled) {
		retval = syna_dev_enable_lowpwr_gesture(tcm, false);
		if (retval < 0) {
			LOGE("Fail to disable low power gesture mode\n");
			return retval;
		}
	}

#ifdef GOOG_INT2_FEATURE
	retval = syna_tcm_get_dynamic_config(tcm->tcm_dev,
				DC_ENABLE_WAKEUP_GESTURE_MODE,
				&enable,
				RESP_IN_POLLING);
	if (retval < 0) {
		LOGE("Fail to get low power gesture mode\n");
		return retval;
	}

	if (enable) {
		retval = syna_tcm_set_dynamic_config(tcm->tcm_dev,
				DC_ENABLE_WAKEUP_GESTURE_MODE,
				0,
				RESP_IN_POLLING);
		if (retval < 0) {
			LOGE("Fail to exit low power gesture mode\n");
			return retval;
		}
		LOGI("Exit gesture mode.");
	}

#endif
	return 0;
}
#endif
#ifdef POWER_ALIVE_AT_SUSPEND
/**
 * syna_dev_enter_lowpwr_sensing()
 *
 * Helper to enter power-saved sensing mode, that
 * may be the lower power gesture mode or deep sleep mode.
 *
 * @param
 *    [ in] tcm: tcm driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_enter_lowpwr_sensing(struct syna_tcm *tcm)
{
	int retval = 0;

	if (!tcm)
		return -EINVAL;

	/* enable low power gesture mode, if needed */
	if (tcm->lpwg_enabled) {
		retval = syna_dev_enable_lowpwr_gesture(tcm, true);
		if (retval < 0) {
			LOGE("Fail to disable low power gesture mode\n");
			return retval;
		}
	} else {
	/* enter sleep mode for non-LPWG cases */
		if (!tcm->slept_in_early_suspend) {
			retval = syna_tcm_sleep(tcm->tcm_dev, true);
			if (retval < 0) {
				LOGE("Fail to enter deep sleep\n");
				return retval;
			}
		}
	}

	return 0;
}
#endif

static int syna_pinctrl_configure(struct syna_tcm *tcm, bool enable)
{
	struct pinctrl_state *state;

	if (IS_ERR_OR_NULL(tcm->pinctrl)) {
		LOGE("Invalid pinctrl!\n");
		return -EINVAL;
	}

	LOGD("%s\n", enable ? "ACTIVE" : "SUSPEND");

	if (enable) {
		state = pinctrl_lookup_state(tcm->pinctrl, "ts_active");
		if (IS_ERR(state))
			LOGE("Could not get ts_active pinstate!\n");
	} else {
		state = pinctrl_lookup_state(tcm->pinctrl, "ts_suspend");
		if (IS_ERR(state))
			LOGE("Could not get ts_suspend pinstate!\n");
	}

	if (!IS_ERR_OR_NULL(state))
		return pinctrl_select_state(tcm->pinctrl, state);

	return 0;
}

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
/**
 * Report a finger down event on the long press gesture area then immediately
 * report a cancel event(MT_TOOL_PALM).
 */
static void syna_report_cancel_event(struct syna_tcm *tcm)
{
	LOGI("Report cancel event for UDFPS");

	goog_input_lock(tcm->gti);

	/* Finger down on UDFPS area. */
	input_mt_slot(tcm->input_dev, 0);
	input_report_key(tcm->input_dev, BTN_TOUCH, 1);
	input_mt_report_slot_state(tcm->input_dev, MT_TOOL_FINGER, 1);
	input_report_abs(tcm->input_dev, ABS_MT_POSITION_X, tcm->hw_if->udfps_x);
	input_report_abs(tcm->input_dev, ABS_MT_POSITION_Y, tcm->hw_if->udfps_y);
	input_report_abs(tcm->input_dev, ABS_MT_TOUCH_MAJOR, 200);
	input_report_abs(tcm->input_dev, ABS_MT_TOUCH_MINOR, 200);
#ifndef SKIP_PRESSURE
	input_report_abs(tcm->input_dev, ABS_MT_PRESSURE, 1);
#endif
	input_report_abs(tcm->input_dev, ABS_MT_ORIENTATION, 0);
	input_sync(tcm->input_dev);

	/* Report MT_TOOL_PALM for canceling the touch event. */
	input_mt_slot(tcm->input_dev, 0);
	input_report_key(tcm->input_dev, BTN_TOUCH, 1);
	input_mt_report_slot_state(tcm->input_dev, MT_TOOL_PALM, 1);
	input_sync(tcm->input_dev);

	/* Release touches. */
	input_mt_slot(tcm->input_dev, 0);
	input_report_abs(tcm->input_dev, ABS_MT_PRESSURE, 0);
	input_mt_report_slot_state(tcm->input_dev, MT_TOOL_FINGER, 0);
	input_report_abs(tcm->input_dev, ABS_MT_TRACKING_ID, -1);
	input_report_key(tcm->input_dev, BTN_TOUCH, 0);
	input_sync(tcm->input_dev);

	goog_input_unlock(tcm->gti);
}

static void syna_check_finger_status(struct syna_tcm *tcm)
{
	int retval = 0;
	unsigned char code = 0;
	u16 touch_mode, x, y, major, minor;
	struct syna_hw_attn_data *attn = &tcm->hw_if->bdata_attn;
	ktime_t timeout = ktime_add_ms(ktime_get(), 500);
	LOGI("Check finger status");

	while (ktime_get() < timeout) {
		/* Clear the FIFO if there is pending data. */
		if (gpio_get_value(attn->irq_gpio) == attn->irq_on_state) {
			retval = tcm->tcm_dev->read_message(tcm->tcm_dev, &code);
			continue;
		}

		retval = syna_tcm_get_dynamic_config(tcm->tcm_dev, DC_TOUCH_SCAN_MODE,
				&touch_mode, RESP_IN_POLLING);
		if (retval < 0)
			continue;

		if (touch_mode != SCAN_LPWG_IDLE && touch_mode != SCAN_LPWG_ACTIVE)
			return;

		LOGI("Poll finger events.");
		while (ktime_get() < timeout) {
			msleep(30);
			syna_tcm_get_event_data(tcm->tcm_dev,
				&code,
				&tcm->event_data);
			if (code == REPORT_TOUCH) {
				x = syna_pal_le2_to_uint(&tcm->event_data.buf[1]);
				y = syna_pal_le2_to_uint(&tcm->event_data.buf[3]);
				major = tcm->event_data.buf[4];
				minor = tcm->event_data.buf[5];
				/* Touch reports coordinates and major/minor 0
				 * when the finger leaves.
				 */
				if (x==0 && y==0 && major==0 && minor==0) {
					syna_report_cancel_event(tcm);
					return;
				}
			} else if (code == STATUS_INVALID) {
				syna_report_cancel_event(tcm);
				return;
			}
		}
	}
}
#endif

/**
 * syna_dev_resume()
 *
 * Resume from the suspend state.
 * If RESET_ON_RESUME is defined, a reset is issued to the touch controller.
 * Otherwise, the touch controller is brought out of sleep mode.
 *
 * @param
 *    [ in] dev: an instance of device
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_resume(struct device *dev)
{
	int retval = 0;
	int retry = 0;
	struct syna_tcm *tcm = dev_get_drvdata(dev);
	struct syna_hw_interface *hw_if = tcm->hw_if;
	bool irq_enabled = true;
#ifdef RESET_ON_RESUME
	unsigned char status;
#endif

	/* exit directly if device isn't in suspend state */
	if (tcm->pwr_state == PWR_ON)
		return 0;

	LOGI("Prepare to resume device\n");

	syna_pinctrl_configure(tcm, true);

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	if (hw_if->udfps_x != 0 && hw_if->udfps_y != 0)
		syna_check_finger_status(tcm);
#endif

#if !IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	/* clear all input events  */
	syna_dev_free_input_events(tcm);
#endif

#ifdef RESET_ON_RESUME
	LOGI("Do reset on resume\n");

	for (retry = 0; retry < 3; retry++) {
		if (hw_if->ops_hw_reset) {
			hw_if->ops_hw_reset(hw_if);
			retval = syna_tcm_get_event_data(tcm->tcm_dev,
				&status, NULL);
			if ((retval < 0) || (status != REPORT_IDENTIFY)) {
				LOGE("Fail to complete hw reset, ret = %d, status = %d\n",
				     retval, status);
				continue;
			}
			break;
		} else {
			retval = syna_tcm_reset(tcm->tcm_dev);
			if (retval < 0) {
				LOGE("Fail to do sw reset, ret = %d\n", retval);
				continue;
			}
			break;
		}
	}
	if (retval < 0 || (hw_if->ops_hw_reset && (status != REPORT_IDENTIFY))) {
		goto exit;
	}
#else
#ifdef POWER_ALIVE_AT_SUSPEND
	/* enter normal power mode */
	retval = syna_dev_enter_normal_sensing(tcm);
	if (retval < 0) {
		LOGE("Fail to enter normal power mode\n");
		goto exit;
	}
#endif
#ifndef GOOG_INT2_FEATURE
	retval = syna_tcm_rezero(tcm->tcm_dev);
	if (retval < 0) {
		LOGE("Fail to rezero\n");
		goto exit;
	}
#endif
#endif
	tcm->pwr_state = PWR_ON;

	LOGI("Prepare to set up application firmware\n");

	/* set up app firmware */
	retval = syna_dev_set_up_app_fw(tcm);
	if (retval < 0) {
		LOGE("Fail to set up app firmware on resume\n");
		goto exit;
	}

	syna_dev_restore_feature_setting(tcm, RESP_IN_POLLING);

	retval = 0;

	LOGI("Device resumed (pwr_state:%d)\n", tcm->pwr_state);
exit:
	/* set irq back to active mode if not enabled yet */
	irq_enabled = (!hw_if->bdata_attn.irq_enabled);

	/* enable irq */
	if (irq_enabled && (hw_if->ops_enable_irq))
		hw_if->ops_enable_irq(hw_if, true);

	tcm->slept_in_early_suspend = false;

	return retval;
}

/**
 * syna_dev_suspend()
 *
 * Put device into suspend state.
 * Enter either the lower power gesture mode or sleep mode.
 *
 * @param
 *    [ in] dev: an instance of device
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_suspend(struct device *dev)
{
#ifdef POWER_ALIVE_AT_SUSPEND
	int retval, retry;
#endif
	struct syna_tcm *tcm = dev_get_drvdata(dev);
	struct syna_hw_interface *hw_if = tcm->hw_if;
	bool irq_disabled = true;
	unsigned char status;

	/* exit directly if device is already in suspend state */
	if (tcm->pwr_state != PWR_ON)
		return 0;

	LOGI("Prepare to suspend device\n");

	if (tcm->hw_if->dynamic_report_rate)
		cancel_delayed_work_sync(&tcm->set_report_rate_work);

#if !IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	/* clear all input events  */
	syna_dev_free_input_events(tcm);
#endif
	/* once lpwg is enabled, irq should be alive.
	 * otherwise, disable irq in suspend.
	 */
	irq_disabled = (!tcm->lpwg_enabled);

	/* disable irq */
	if (irq_disabled && (hw_if->ops_enable_irq))
		hw_if->ops_enable_irq(hw_if, false);

#ifdef POWER_ALIVE_AT_SUSPEND
#ifdef GOOG_INT2_FEATURE
	LOGI("Do reset on suspend\n");

	for (retry = 0; retry < 3; retry++) {
		if (hw_if->ops_hw_reset) {
			hw_if->ops_hw_reset(hw_if);
			retval = syna_tcm_get_event_data(tcm->tcm_dev,
				&status, NULL);
			if ((retval < 0) || (status != REPORT_IDENTIFY)) {
				LOGE("Fail to complete hw reset, ret = %d, status = %d\n",
				     retval, status);
				continue;
			}
			break;
		} else {
			retval = syna_tcm_reset(tcm->tcm_dev);
			if (retval < 0) {
				LOGE("Fail to do sw reset, ret = %d\n", retval);
				continue;
			}
			break;
		}
	}
#endif

	/* enter power saved mode if power is not off */
	retval = syna_dev_enter_lowpwr_sensing(tcm);
	if (retval < 0) {
		LOGE("Fail to enter suspended power mode, reset and retry.\n");
		if (hw_if->ops_hw_reset) {
			hw_if->ops_hw_reset(hw_if);
			retval = syna_tcm_get_event_data(tcm->tcm_dev,
				&status, NULL);
			if ((retval < 0) || (status != REPORT_IDENTIFY)) {
				LOGE("Fail to complete hw reset, ret = %d, status = %d\n",
				     retval, status);
			}
		}
		retval = syna_dev_enter_lowpwr_sensing(tcm);
		if (retval < 0)
			LOGE("Fail to enter suspended power mode after reset.\n");
	}
	tcm->pwr_state = LOW_PWR;
#else
	tcm->pwr_state = PWR_OFF;
#endif

	syna_pinctrl_configure(tcm, false);

	LOGI("Device suspended (pwr_state:%d)\n", tcm->pwr_state);

	return 0;
}

#if defined(ENABLE_DISP_NOTIFIER)
/**
 * syna_dev_early_suspend()
 *
 * If having early suspend support, enter the sleep mode for
 * non-lpwg cases.
 *
 * @param
 *    [ in] dev: an instance of device
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_early_suspend(struct device *dev)
{
	int retval;
	struct syna_tcm *tcm = dev_get_drvdata(dev);

	/* exit directly if device is already in suspend state */
	if (tcm->pwr_state != PWR_ON)
		return 0;

	if (!tcm->lpwg_enabled) {
		retval = syna_tcm_sleep(tcm->tcm_dev, true);
		if (retval < 0) {
			LOGE("Fail to enter deep sleep\n");
			return retval;
		}
	}

	tcm->slept_in_early_suspend = true;

	return 0;
}
/**
 * syna_dev_fb_notifier_cb()
 *
 * Listen the display screen on/off event and perform the corresponding
 * actions.
 *
 * @param
 *    [ in] nb:     instance of notifier_block
 *    [ in] action: fb action
 *    [ in] data:   fb event data
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_fb_notifier_cb(struct notifier_block *nb,
		unsigned long action, void *data)
{
	int retval;
	int transition;
#if defined(USE_DRM_PANEL_NOTIFIER)
	struct drm_panel_notifier *evdata = data;
#else
	struct fb_event *evdata = data;
#endif
	struct syna_tcm *tcm = container_of(nb, struct syna_tcm, fb_notifier);
	int time = 0;
	int disp_blank_powerdown;
	int disp_early_event_blank;
	int disp_blank;
	int disp_blank_unblank;

	if (!evdata || !evdata->data || !tcm)
		return 0;

	retval = 0;

#if defined(USE_DRM_PANEL_NOTIFIER)
	disp_blank_powerdown = DRM_PANEL_BLANK_POWERDOWN;
	disp_early_event_blank = DRM_PANEL_EARLY_EVENT_BLANK;
	disp_blank = DRM_PANEL_EVENT_BLANK;
	disp_blank_unblank = DRM_PANEL_BLANK_UNBLANK;
#else
	disp_blank_powerdown = FB_BLANK_POWERDOWN;
	disp_early_event_blank = FB_EARLY_EVENT_BLANK;
	disp_blank = FB_EVENT_BLANK;
	disp_blank_unblank = FB_BLANK_UNBLANK;
#endif

	transition = *(int *)evdata->data;

	/* confirm the firmware flashing is completed before screen off */
	if (transition == disp_blank_powerdown) {
		while (ATOMIC_GET(tcm->tcm_dev->firmware_flashing)) {
			syna_pal_sleep_ms(500);

			time += 500;
			if (time >= 5000) {
				LOGE("Timed out waiting for re-flashing\n");
				ATOMIC_SET(tcm->tcm_dev->firmware_flashing, 0);
				return -ETIMEDOUT;
			}
		}
	}

	if (action == disp_early_event_blank &&
		transition == disp_blank_powerdown) {
		retval = syna_dev_early_suspend(&tcm->pdev->dev);
	} else if (action == disp_blank) {
		if (transition == disp_blank_powerdown) {
			retval = syna_dev_suspend(&tcm->pdev->dev);
			tcm->fb_ready = 0;
		} else if (transition == disp_blank_unblank) {
#ifndef RESUME_EARLY_UNBLANK
			retval = syna_dev_resume(&tcm->pdev->dev);
			tcm->fb_ready++;
#endif
		} else if (action == disp_early_event_blank &&
			transition == disp_blank_unblank) {
#ifdef RESUME_EARLY_UNBLANK
			retval = syna_dev_resume(&tcm->pdev->dev);
			tcm->fb_ready++;
#endif
		}
	}

	return 0;
}
#endif

/**
 * syna_dev_disconnect()
 *
 * This function will power off the connected device.
 * Then, all the allocated resource will be released.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_disconnect(struct syna_tcm *tcm)
{
	struct syna_hw_interface *hw_if = tcm->hw_if;

	if (tcm->is_connected == false) {
		LOGI("%s already disconnected\n", PLATFORM_DRIVER_NAME);
		return 0;
	}

	if (tcm->pwr_state == BARE_MODE) {
		LOGI("Disconnect from bare mode\n");
		goto exit;
	}

#ifdef STARTUP_REFLASH
	cancel_delayed_work_sync(&tcm->reflash_work);
	flush_workqueue(tcm->reflash_workqueue);
	destroy_workqueue(tcm->reflash_workqueue);
#endif

	/* free interrupt line */
	if (hw_if->bdata_attn.irq_id)
		syna_dev_release_irq(tcm);

	/* unregister input device */
	syna_dev_release_input_device(tcm);

	tcm->input_dev_params.max_x = 0;
	tcm->input_dev_params.max_y = 0;
	tcm->input_dev_params.max_objects = 0;

exit:
#ifdef POWER_SEQUENCE_ON_CONNECT
	/* power off */
	if (hw_if->ops_power_on)
		hw_if->ops_power_on(hw_if, false);
#endif
	tcm->pwr_state = PWR_OFF;
	tcm->is_connected = false;

	LOGI("Device %s disconnected\n", PLATFORM_DRIVER_NAME);

	return 0;
}

/**
 * syna_dev_connect()
 *
 * This function will power on and identify the connected device.
 * At the end of function, the ISR will be registered as well.
 *
 * @param
 *    [ in] tcm: the driver handle
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_connect(struct syna_tcm *tcm)
{
	int retval;
	struct syna_hw_interface *hw_if = tcm->hw_if;
	struct tcm_dev *tcm_dev = tcm->tcm_dev;

	if (!tcm_dev) {
		LOGE("Invalid tcm_dev\n");
		return -EINVAL;
	}

	if (tcm->is_connected) {
		LOGI("Device %s already connected\n", PLATFORM_DRIVER_NAME);
		return 0;
	}
#ifdef POWER_SEQUENCE_ON_CONNECT
	/* power on the connected device */
	if (hw_if->ops_power_on) {
		retval = hw_if->ops_power_on(hw_if, true);
		if (retval < 0)
			return -ENODEV;
	}
#endif
#ifdef RESET_ON_CONNECT
	/* perform a hardware reset */
	if (hw_if->ops_hw_reset)
		hw_if->ops_hw_reset(hw_if);
#endif
	/* detect which modes of touch controller is running
	 *
	 * this function will handle the startup packet once
	 * powering on the ASIC
	 */
	retval = syna_tcm_detect_device(tcm->tcm_dev, 0, true);
	if (retval < 0) {
		LOGE("Fail to detect the device\n");
		goto err_detect_dev;
	}
	/* 'Bare' mode is a special software mode to bypass all
	 * driver control for the specific user scenario
	 */
	if (tcm->pwr_state == BARE_MODE) {
		LOGI("Device %s config into bare mode\n", PLATFORM_DRIVER_NAME);
		tcm->is_connected = true;
		return 0;
	}

#ifdef FORCE_CONNECTION
	goto request_irq;
#endif

	switch (retval) {
	case MODE_APPLICATION_FIRMWARE:
		retval = syna_dev_set_up_app_fw(tcm);
		if (retval < 0) {
			LOGE("Fail to set up application firmware\n");

			/* switch to bootloader mode when failed */
			LOGI("Switch device to bootloader mode instead\n");
			syna_tcm_switch_fw_mode(tcm_dev,
					MODE_BOOTLOADER,
					FW_MODE_SWITCH_DELAY_MS);
		} else {
			/* allocate and register to input device subsystem */
			retval = syna_dev_set_up_input_device(tcm);
			if (retval < 0) {
				LOGE("Fail to set up input device\n");
				goto err_setup_input_dev;
			}
		}

		break;
	default:
		LOGN("Application firmware not running, current mode: %02x\n",
			retval);
		break;
	}

#ifdef FORCE_CONNECTION
request_irq:
#endif

	LOGI("TCM packrat: %d\n", tcm->tcm_dev->packrat_number);
	LOGI("Config: lpwg mode(%s), custom tp config(%s) helper work(%s)\n",
		(tcm->lpwg_enabled) ? "yes" : "no",
		(tcm->has_custom_tp_config) ? "yes" : "no",
		(tcm->helper_enabled) ? "yes" : "no");
	LOGI("Config: startup reflash(%s), hw reset(%s), rst on resume(%s)\n",
		(tcm->startup_reflash_enabled) ? "yes" : "no",
		(hw_if->ops_hw_reset) ? "yes" : "no",
		(tcm->rst_on_resume_enabled) ? "yes" : "no");
	LOGI("Config: max. write size(%d), max. read size(%d), irq ctrl(%s)\n",
		tcm_dev->max_wr_size, tcm_dev->max_rd_size,
		(hw_if->ops_enable_irq) ? "yes" : "no");

	LOGI("Device %s connected\n", PLATFORM_DRIVER_NAME);

	tcm->pwr_state = PWR_ON;
	tcm->is_connected = true;

	return 0;

err_setup_input_dev:
err_detect_dev:
#ifdef POWER_SEQUENCE_ON_CONNECT
	if (hw_if->ops_power_on)
		hw_if->ops_power_on(hw_if, false);
#endif
	return retval;
}

#ifdef USE_DRM_PANEL_NOTIFIER
static struct drm_panel *syna_dev_get_panel(struct device_node *np)
{
	int i;
	int count;
	struct device_node *node;
	struct drm_panel *panel;

	count = of_count_phandle_with_args(np, "panel", NULL);
	if (count <= 0)
		return NULL;

	for (i = 0; i < count; i++) {
		node = of_parse_phandle(np, "panel", i);
		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (!IS_ERR(panel)) {
			LOGI("Find available panel\n");
			return panel;
		}
	}

	return NULL;
}
#endif

/**
 * syna_dev_probe()
 *
 * Install the TouchComm device driver
 *
 * @param
 *    [ in] pdev: an instance of platform device
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_probe(struct platform_device *pdev)
{
	int retval;
	struct syna_tcm *tcm = NULL;
	struct tcm_dev *tcm_dev = NULL;
	struct syna_hw_interface *hw_if = NULL;
#if defined(USE_DRM_PANEL_NOTIFIER)
	struct device *dev;
#endif

	hw_if = pdev->dev.platform_data;
	if (!hw_if) {
		LOGE("Fail to find hardware configuration\n");
		return -EINVAL;
	}

	tcm = syna_pal_mem_alloc(1, sizeof(struct syna_tcm));
	if (!tcm) {
		LOGE("Fail to create the instance of syna_tcm\n");
		return -ENOMEM;
	}

	tcm->pinctrl = devm_pinctrl_get(pdev->dev.parent);
	if (IS_ERR_OR_NULL(tcm->pinctrl)) {
		LOGE("Could not get pinctrl!\n");
	} else {
		syna_pinctrl_configure(tcm, true);
	}

	/* allocate the TouchCom device handle
	 * recommend to set polling mode here because isr is not registered yet
	 */
	retval = syna_tcm_allocate_device(&tcm_dev, hw_if, RESP_IN_POLLING);
	if ((retval < 0) || (!tcm_dev)) {
		LOGE("Fail to allocate TouchCom device handle\n");
		goto err_allocate_cdev;
	}

	tcm->tcm_dev = tcm_dev;
	tcm->pdev = pdev;
	tcm->hw_if = hw_if;

	syna_tcm_buf_init(&tcm->event_data);

#if !IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	syna_pal_mutex_alloc(&tcm->tp_event_mutex);
#endif

#ifdef USE_CUSTOM_TOUCH_REPORT_CONFIG
	tcm->has_custom_tp_config = true;
#else
	tcm->has_custom_tp_config = false;
#endif
#ifdef STARTUP_REFLASH
	tcm->startup_reflash_enabled = true;
#else
	tcm->startup_reflash_enabled = false;
#endif
#ifdef RESET_ON_RESUME
	tcm->rst_on_resume_enabled = true;
#else
	tcm->rst_on_resume_enabled = false;
#endif
#ifdef ENABLE_HELPER
	tcm->helper_enabled = true;
#else
	tcm->helper_enabled = false;
#endif
#ifdef ENABLE_WAKEUP_GESTURE
	tcm->lpwg_enabled = true;
#else
	tcm->lpwg_enabled = false;
#endif
	tcm->irq_wake = false;

	tcm->is_connected = false;
	tcm->pwr_state = PWR_OFF;

	tcm->dev_connect = syna_dev_connect;
	tcm->dev_disconnect = syna_dev_disconnect;
	tcm->dev_set_up_app_fw = syna_dev_set_up_app_fw;
	tcm->dev_resume = syna_dev_resume;
	tcm->dev_suspend = syna_dev_suspend;

	tcm->userspace_app_info = NULL;

	platform_set_drvdata(pdev, tcm);

	device_init_wakeup(&pdev->dev, 1);

	tcm->event_wq = alloc_workqueue("syna_wq", WQ_UNBOUND |
					WQ_HIGHPRI | WQ_CPU_INTENSIVE, 1);

	if (!tcm->event_wq) {
		LOGE("Cannot create work thread\n");
		retval = -ENOMEM;
		goto err_alloc_workqueue;
	}

#if defined(TCM_CONNECT_IN_PROBE)
	/* connect to target device */
	retval = tcm->dev_connect(tcm);
	if (retval < 0) {
#ifdef FORCE_CONNECTION
		LOGW("Device detection is failed somehow\n");
		LOGW("Install driver anyway due to force connect\n");
#else
		LOGE("Fail to connect to the device\n");
		retval = -EPROBE_DEFER;
#if !IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
		syna_pal_mutex_free(&tcm->tp_event_mutex);
#endif
		goto err_connect;
#endif
	}
#endif

	tcm->raw_data_report_code = 0;
	init_completion(&tcm->raw_data_completion);
	complete_all(&tcm->raw_data_completion);

	INIT_WORK(&tcm->motion_filter_work, syna_motion_filter_work);

	tcm->touch_report_rate_config = CONFIG_HIGH_REPORT_RATE;
	INIT_DELAYED_WORK(&tcm->set_report_rate_work, syna_set_report_rate_work);

	tcm->enable_fw_grip = 0x02;
	tcm->enable_fw_palm = 0x02;

	retval = syna_dev_request_irq(tcm);
	if (retval < 0) {
		LOGE("Fail to request the interrupt line\n");
		goto err_request_irq;
	}

	/* for the reference,
	 * create a delayed work to perform fw update during the startup time
	 */
#ifdef STARTUP_REFLASH
	tcm->force_reflash = false;
	tcm->reflash_count = 0;
	tcm->reflash_workqueue =
			create_singlethread_workqueue("syna_reflash");
	INIT_DELAYED_WORK(&tcm->reflash_work, syna_dev_reflash_startup_work);
	queue_delayed_work(tcm->reflash_workqueue, &tcm->reflash_work,
			msecs_to_jiffies(STARTUP_REFLASH_DELAY_TIME_MS));
#endif

#ifdef HAS_SYSFS_INTERFACE
	/* create the device file and register to char device classes */
	retval = syna_cdev_create(tcm, pdev);
	if (retval < 0) {
		LOGE("Fail to create the device sysfs\n");
#if !IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
		syna_pal_mutex_free(&tcm->tp_event_mutex);
#endif
		goto err_create_cdev;
	}
#endif

#if defined(ENABLE_DISP_NOTIFIER)
#if defined(USE_DRM_PANEL_NOTIFIER)
	dev = syna_request_managed_device();
	active_panel = syna_dev_get_panel(dev->of_node);
	if (active_panel) {
		tcm->fb_notifier.notifier_call = syna_dev_fb_notifier_cb;
		retval = drm_panel_notifier_register(active_panel,
				&tcm->fb_notifier);
		if (retval < 0) {
			LOGE("Fail to register FB notifier client\n");
			goto err_create_cdev;
		}
	} else {
		LOGE("No available drm panel\n");
	}
#else
	tcm->fb_notifier.notifier_call = syna_dev_fb_notifier_cb;
	retval = fb_register_client(&tcm->fb_notifier);
	if (retval < 0) {
		LOGE("Fail to register FB notifier client\n");
		goto err_create_cdev;
	}
#endif
#endif

#if defined(ENABLE_HELPER)
	ATOMIC_SET(tcm->helper.task, HELP_NONE);
	INIT_WORK(&tcm->helper.work, syna_dev_helper_work);
	/* set up custom touch data parsing method */
	syna_tcm_set_reset_occurrence_callback(tcm_dev,
			syna_dev_reset_detected_cb,
			(void *)tcm);
#endif

	LOGI("TouchComm driver, %s ver.: %d.%s, installed\n",
		PLATFORM_DRIVER_NAME,
		SYNAPTICS_TCM_DRIVER_VERSION,
		SYNAPTICS_TCM_DRIVER_SUBVER);

	return 0;

#ifdef HAS_SYSFS_INTERFACE
err_create_cdev:
	syna_tcm_remove_device(tcm->tcm_dev);
#endif
err_request_irq:
#if defined(TCM_CONNECT_IN_PROBE)
	tcm->dev_disconnect(tcm);

err_connect:
#endif

	if (tcm->event_wq)
		destroy_workqueue(tcm->event_wq);
err_alloc_workqueue:
	syna_tcm_buf_release(&tcm->event_data);
#if !IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	syna_pal_mutex_free(&tcm->tp_event_mutex);
#endif
err_allocate_cdev:
	syna_pal_mem_free((void *)tcm);

	return retval;
}

/**
 * syna_dev_remove()
 *
 * Release all allocated resources and remove the TouchCom device handle
 *
 * @param
 *    [ in] pdev: an instance of platform device
 *
 * @return
 *    on success, 0; otherwise, negative value on error.
 */
static int syna_dev_remove(struct platform_device *pdev)
{
	struct syna_tcm *tcm = platform_get_drvdata(pdev);

	if (!tcm) {
		LOGW("Invalid handle to remove\n");
		return 0;
	}
#if defined(ENABLE_HELPER)
	cancel_work_sync(&tcm->helper.work);
#endif

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	cancel_work_sync(&tcm->set_grip_mode_work);
	cancel_work_sync(&tcm->set_palm_mode_work);
#endif
	cancel_work_sync(&tcm->motion_filter_work);
	cancel_delayed_work_sync(&tcm->set_report_rate_work);

#if defined(ENABLE_DISP_NOTIFIER)
#if defined(USE_DRM_PANEL_NOTIFIER)
	if (active_panel)
		drm_panel_notifier_unregister(active_panel,
				&tcm->fb_notifier);
#else
	fb_unregister_client(&tcm->fb_notifier);
#endif
#endif

#ifdef HAS_SYSFS_INTERFACE
	/* remove the cdev and sysfs nodes */
	syna_cdev_remove(tcm);
#endif

	/* check the connection status, and do disconnection */
	if (tcm->dev_disconnect(tcm) < 0)
		LOGE("Fail to do device disconnection\n");

	if (tcm->userspace_app_info != NULL)
		syna_pal_mem_free(tcm->userspace_app_info);

	if (tcm->raw_data_buffer) {
		kfree(tcm->raw_data_buffer);
		tcm->raw_data_buffer = NULL;
	}

	syna_tcm_buf_release(&tcm->event_data);

#if !IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	syna_pal_mutex_free(&tcm->tp_event_mutex);
#endif

	/* remove the allocated tcm device */
	syna_tcm_remove_device(tcm->tcm_dev);

	/* release the device context */
	syna_pal_mem_free((void *)tcm);

	return 0;
}

/**
 * syna_dev_shutdown()
 *
 * Call syna_dev_remove() to release all resources
 *
 * @param
 *    [in] pdev: an instance of platform device
 *
 * @return
 *    none.
 */
static void syna_dev_shutdown(struct platform_device *pdev)
{
	syna_dev_remove(pdev);
}

/**
 * Declare a TouchComm platform device
 */
#if IS_ENABLED(CONFIG_PM) || IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
static const struct dev_pm_ops syna_dev_pm_ops = {
#if !defined(ENABLE_DISP_NOTIFIER) || IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	.suspend = syna_dev_suspend,
	.resume = syna_dev_resume,
#endif
};
#endif

static struct platform_driver syna_dev_driver = {
	.driver = {
		.name = PLATFORM_DRIVER_NAME,
		.owner = THIS_MODULE,
#if IS_ENABLED(CONFIG_PM) && !IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
		.pm = &syna_dev_pm_ops,
#endif
	},
	.probe = syna_dev_probe,
	.remove = syna_dev_remove,
	.shutdown = syna_dev_shutdown,
};


/**
 * syna_dev_module_init()
 *
 * The entry function of the reference driver, which initialize the
 * lower-level bus and register a platform driver.
 *
 * @param
 *    void.
 *
 * @return
 *    0 if the driver registered and bound to a device,
 *    else returns a negative error code and with the driver not registered.
 */
static int __init syna_dev_module_init(void)
{
	int retval;

	retval = syna_hw_interface_init();
	if (retval < 0)
		return retval;

	return platform_driver_register(&syna_dev_driver);
}

/**
 * syna_dev_module_exit()
 *
 * Function is called when un-installing the driver.
 * Remove the registered platform driver and the associated bus driver.
 *
 * @param
 *    void.
 *
 * @return
 *    none.
 */
static void __exit syna_dev_module_exit(void)
{
	platform_driver_unregister(&syna_dev_driver);

	syna_hw_interface_exit();
}

module_init(syna_dev_module_init);
module_exit(syna_dev_module_exit);

MODULE_AUTHOR("Synaptics, Inc.");
MODULE_DESCRIPTION("Synaptics TCM Touch Driver");
MODULE_LICENSE("GPL v2");


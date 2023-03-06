/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Header file for Samsung DisplayPort driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __EXYNOS_DRM_DP_H__
#define __EXYNOS_DRM_DP_H__

#include <drm/drm_encoder.h>
#include <drm/drm_connector.h>
#include <drm/drm_dp_helper.h>
#include <linux/extcon.h>

#include "exynos_drm_drv.h"
#include "exynos_drm_crtc.h"

#include "dp_cal.h"


int get_dp_log_level(void);

#define dp_info(dp, fmt, ...)	\
pr_info("%s: "fmt, dp->dev->driver->name, ##__VA_ARGS__)

#define dp_warn(dp, fmt, ...)	\
pr_warn("%s: "fmt, dp->dev->driver->name, ##__VA_ARGS__)

#define dp_err(dp, fmt, ...)	\
pr_err("%s: "fmt, dp->dev->driver->name, ##__VA_ARGS__)

#define dp_debug(dp, fmt, ...)	\
pr_debug("%s: "fmt, dp->dev->driver->name, ##__VA_ARGS__)

extern struct dp_device *dp_drvdata;

enum dp_state {
	DP_STATE_INIT,
	DP_STATE_ON,
	DP_STATE_RUN,
};

enum hotplug_state {
	EXYNOS_HPD_UNPLUG = 0,
	EXYNOS_HPD_PLUG,
	EXYNOS_HPD_IRQ,
};

struct dp_link {
	u32  link_rate;
	u8   num_lanes;
	u8   support_tps;
	bool fast_training;
	bool enhanced_frame;
	bool ssc;
};

struct dp_host {
	u32  link_rate;
	u8   num_lanes;
	u8   support_tps;
	bool fast_training;
	bool enhanced_frame;
	bool ssc;
	bool scrambler;

	/* Link Training */
	u8  volt_swing_max;
	u8  pre_emphasis_max;
	u8  vol_swing_level[MAX_LANE_CNT];
	u8  pre_empha_level[MAX_LANE_CNT];
	u8  max_reach_value[MAX_LANE_CNT];
};

#define SINK_NAME_LEN	14	/* monitor name */
struct dp_sink {
	u8   revision;
	u32  link_rate;
	u8   num_lanes;
	u8   support_tps;
	bool fast_training;
	bool enhanced_frame;
	bool ssc;

	/* From EDID */
	char sink_name[SINK_NAME_LEN];
	u8   edid_manufacturer[4];
	u32  edid_product;
	u32  edid_serial;

	u32 audio_ch_num;
	u32 audio_sample_rates;
	u32 audio_bit_rates;
};

struct dp_resources {
	int aux_ch_mux_gpio;
	int irq;
	void __iomem *link_regs;
	void __iomem *phy_regs;
#ifdef CONFIG_SOC_ZUMA
	void __iomem *phy_tca_regs;
	struct clk *dposc_clk;
#endif
};

enum dp_audio_state {
	DP_AUDIO_DISABLE = 0,
	DP_AUDIO_ENABLE,
	DP_AUDIO_START,
	DP_AUDIO_REQ_BUF_READ,
	DP_AUDIO_WAIT_BUF_FULL,
	DP_AUDIO_STOP,
};

struct dp_audio_config {
	enum dp_audio_state audio_state;
	u32 num_audio_ch;
	enum audio_sampling_frequency audio_fs;
	enum audio_bit_per_channel audio_bit;
	enum audio_16bit_dma_mode audio_packed_mode;
	enum audio_dma_word_length audio_word_length;
};

/* HDCP */
#define HDCP_BKSV_SIZE 5
#define HDCP_AN_SIZE 8
#define HDCP_AKSV_SIZE 5
#define HDCP_BINFO_SIZE 2
#define HDCP_KSV_SIZE 5
#define HDCP_R0_SIZE 2
#define HDCP_M0_SIZE 8
#define HDCP_SHA1_SIZE 20
#define HDCP_SHA1_INPUT_SIZE (HDCP_BINFO_SIZE + HDCP_KSV_SIZE + HDCP_M0_SIZE)

#define HDCP_RETRY_AUTH_COUNT 1
#define V_READ_RETRY_CNT 3
#define RI_READ_RETRY_CNT 3
#define RI_AVAILABLE_WAITING 2
#define RI_DELAY 100
#define RI_WAIT_COUNT (RI_DELAY / RI_AVAILABLE_WAITING)
#define REPEATER_READY_MAX_WAIT_DELAY 4000

#define BINFO_DEVICE_COUNT (0x7F << 0)

enum dp_hdcp_version {
	HDCP_VERSION_NONE = 0,
	HDCP_VERSION_1_3 = 0x13,
	HDCP_VERSION_2_2 = 0x22,
	HDCP_VERSION_2_3 = 0x23,
};

enum dp_hdcp13_link_check {
	LINK_CHECK_PASS = 0,
	LINK_CHECK_NEED,
	LINK_CHECK_FAIL,
};

enum dp_hdcp13_state {
	HDCP13_STATE_NOT_AUTHENTICATED = 0,
	HDCP13_STATE_AUTH_PROCESS,
	HDCP13_STATE_SECOND_AUTH_DONE,
	HDCP13_STATE_AUTHENTICATED,
	HDCP13_STATE_FAIL,
};

struct dp_hdcp13_info {
	u8 cp_irq_flag;
	u8 is_repeater;
	u8 device_cnt;
	u8 revocation_check;
	u8 r0_read_flag;
	enum dp_hdcp13_link_check link_check;
	enum dp_hdcp13_state auth_state;
};

enum dp_state_for_hdcp22 {
	DP_DISCONNECT,
	DP_CONNECT,
	DP_HDCP_READY,
};

enum auth_signal {
	HDCP_DRM_OFF	= 0x100,
	HDCP_DRM_ON	= 0x200,
	HDCP_RP_READY	= 0x300,
};

struct dp_hdcp22_info {
	bool reauth_trigger;
};

struct dp_hdcp_config {
	enum dp_hdcp_version hdcp_ver;
	struct dp_hdcp13_info hdcp13_info;
	struct dp_hdcp22_info hdcp22_info;
};

/* DisplayPort Device */
struct dp_device {
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_dp_aux dp_aux;

	enum exynos_drm_output_type output_type;

	struct device *dev;
	struct dp_resources res;

	struct workqueue_struct *dp_wq;
	struct delayed_work hpd_plug_work;
	struct delayed_work hpd_unplug_work;
	struct delayed_work hpd_irq_work;

	struct mutex cmd_lock;
	struct mutex hpd_lock;
	struct mutex training_lock;

	/* HPD State */
	enum hotplug_state hpd_current_state;
	struct mutex hpd_state_lock;

	/* DP Driver State */
	enum dp_state state;

	/* DRM Mode */
	int cur_mode_vic; /* VIC number of cur_mode */
	struct drm_display_mode cur_mode;
	struct drm_display_mode pref_mode;
	bool fail_safe;

	/* DP Capabilities */
	struct dp_link link;
	struct dp_host host;
	struct dp_sink sink;

	/* PDIC / ExtCon */
	struct extcon_dev *edev;
	struct notifier_block dp_typec_nb;
	int notifier_registered;

	/* BIST */
	bool bist_used;

	/* Audio */
	enum dp_audio_state audio_state;
	struct mutex audio_lock;

	/* HDCP */
	struct dp_hdcp_config hdcp_config;
	struct mutex hdcp_lock;
	struct workqueue_struct *hdcp_wq;
	struct delayed_work hdcp13_work;
	struct delayed_work hdcp22_work;

	/* DP HW Configurations */
	struct dp_hw_config hw_config;
};

static inline struct dp_device *get_dp_drvdata(void)
{
	return dp_drvdata;
}

/* Prototypes of export symbol to handshake other modules */
/* DP Audio Prototype */
int dp_audio_config(struct dp_audio_config *audio_config);

/* HDCP 2.2 Prototype */
void dp_hdcp22_enable(u32 en);
int  dp_dpcd_read_for_hdcp22(u32 address, u32 length, u8 *data);
int  dp_dpcd_write_for_hdcp22(u32 address, u32 length, u8 *data);

// From Exynos-HDCP2-DPLink-Inter.c
extern int  hdcp_dplink_auth_check(enum auth_signal);
extern int  hdcp_dplink_get_rxstatus(uint8_t *status);
extern int  hdcp_dplink_set_paring_available(void);
extern int  hdcp_dplink_set_hprime_available(void);
extern int  hdcp_dplink_set_rp_ready(void);
extern int  hdcp_dplink_set_reauth(void);
extern int  hdcp_dplink_set_integrity_fail(void);
extern int  hdcp_dplink_cancel_auth(void);
extern void hdcp_dplink_clear_all(void);
extern void hdcp_dplink_connect_state(enum dp_state_for_hdcp22 state);

extern void dp_register_func_for_hdcp22(void (*func0)(u32 en),
	int (*func1)(u32 address, u32 length, u8 *data),
	int (*func2)(u32 address, u32 length, u8 *data));

// External functions
void dp_enable_dposc(struct dp_device *dp);
void dp_disable_dposc(struct dp_device *dp);
int dp_get_clock(struct dp_device *dp);

#endif // __EXYNOS_DRM_DP_H__

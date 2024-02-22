/* SPDX-License-Identifier: MIT */
/*
 * MIPI-DSI based CM4 panel driver.
 *
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_vblank.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/thermal.h>
#include <video/mipi_display.h>

#include "trace/dpu_trace.h"
#include "trace/panel_trace.h"

#include "gs_panel/drm_panel_funcs_defaults.h"
#include "gs_panel/gs_panel.h"
#include "gs_panel/gs_panel_funcs_defaults.h"

/**
 * struct cm4_panel - panel specific info
 *
 * This struct maintains cm4 panel specific info. The variables with the prefix hw_ keep
 * track of the features that were actually committed to hardware, and should be modified
 * after sending cmds to panel, i.e. updating hw state.
 */
struct cm4_panel {
	/** @base: base panel struct */
	struct gs_panel base;
	/** @force_changeable_te: force changeable TE (instead of fixed) during early exit */
	bool force_changeable_te;
	/** @force_changeable_te2: force changeable TE for monitoring refresh rate */
	bool force_changeable_te2;
	/** @force_za_off: force to turn off zonal attenuation */
	bool force_za_off;
	/**
	 * @is_pixel_off: pixel-off command is sent to panel. Only sending normal-on or resetting
	 *		  panel can recover to normal mode after entering pixel-off state.
	 */
	bool is_pixel_off;
};

#define to_spanel(ctx) container_of(ctx, struct cm4_panel, base)

/* DSCv1.2a 1280x2856 */
static const struct drm_dsc_config wqhd_pps_config = {
	.line_buf_depth = 9,
	.bits_per_component = 8,
	.convert_rgb = true,
	.slice_count = 2,
	.slice_width = 640,
	.slice_height = 42,
	.simple_422 = false,
	.pic_width = 1280,
	.pic_height = 2856,
	.rc_tgt_offset_high = 3,
	.rc_tgt_offset_low = 3,
	.bits_per_pixel = 128,
	.rc_edge_factor = 6,
	.rc_quant_incr_limit1 = 11,
	.rc_quant_incr_limit0 = 11,
	.initial_xmit_delay = 512,
	.initial_dec_delay = 577,
	.block_pred_enable = true,
	.first_line_bpg_offset = 12,
	.initial_offset = 6144,
	.rc_buf_thresh = {
		14, 28, 42, 56,
		70, 84, 98, 105,
		112, 119, 121, 123,
		125, 126
	},
	.rc_range_params = {
		{.range_min_qp = 0, .range_max_qp = 4, .range_bpg_offset = 2},
		{.range_min_qp = 0, .range_max_qp = 4, .range_bpg_offset = 0},
		{.range_min_qp = 1, .range_max_qp = 5, .range_bpg_offset = 0},
		{.range_min_qp = 1, .range_max_qp = 6, .range_bpg_offset = 62},
		{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 60},
		{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 58},
		{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 56},
		{.range_min_qp = 3, .range_max_qp = 8, .range_bpg_offset = 56},
		{.range_min_qp = 3, .range_max_qp = 9, .range_bpg_offset = 56},
		{.range_min_qp = 3, .range_max_qp = 10, .range_bpg_offset = 54},
		{.range_min_qp = 5, .range_max_qp = 11, .range_bpg_offset = 54},
		{.range_min_qp = 5, .range_max_qp = 12, .range_bpg_offset = 52},
		{.range_min_qp = 5, .range_max_qp = 13, .range_bpg_offset = 52},
		{.range_min_qp = 7, .range_max_qp = 13, .range_bpg_offset = 52},
		{.range_min_qp = 13, .range_max_qp = 15, .range_bpg_offset = 52}
	},
	.rc_model_size = 8192,
	.flatness_min_qp = 3,
	.flatness_max_qp = 12,
	.initial_scale_value = 32,
	.scale_decrement_interval = 8,
	.scale_increment_interval = 1130,
	.nfl_bpg_offset = 600,
	.slice_bpg_offset = 522,
	.final_offset = 4336,
	.vbr_enable = false,
	.slice_chunk_size = 640,
	.dsc_version_minor = 2,
	.dsc_version_major = 1,
	.native_422 = false,
	.native_420 = false,
	.second_line_bpg_offset = 0,
	.nsl_bpg_offset = 0,
	.second_line_offset_adj = 0,
};

/* DSC v1.2a 960x2142 */
static const struct drm_dsc_config fhd_pps_config = {
	.line_buf_depth = 9,
	.bits_per_component = 8,
	.convert_rgb = true,
	.slice_count = 2,
	.slice_width = 480,
	.slice_height = 42,
	.simple_422 = false,
	.pic_width = 960,
	.pic_height = 2142,
	.rc_tgt_offset_high = 3,
	.rc_tgt_offset_low = 3,
	.bits_per_pixel = 128,
	.rc_edge_factor = 6,
	.rc_quant_incr_limit1 = 11,
	.rc_quant_incr_limit0 = 11,
	.initial_xmit_delay = 512,
	.initial_dec_delay = 496,
	.block_pred_enable = true,
	.first_line_bpg_offset = 12,
	.initial_offset = 6144,
	.rc_buf_thresh = {
		14, 28, 42, 56,
		70, 84, 98, 105,
		112, 119, 121, 123,
		125, 126
	},
	.rc_range_params = {
		{.range_min_qp = 0, .range_max_qp = 4, .range_bpg_offset = 2},
		{.range_min_qp = 0, .range_max_qp = 4, .range_bpg_offset = 0},
		{.range_min_qp = 1, .range_max_qp = 5, .range_bpg_offset = 0},
		{.range_min_qp = 1, .range_max_qp = 6, .range_bpg_offset = 62},
		{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 60},
		{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 58},
		{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 56},
		{.range_min_qp = 3, .range_max_qp = 8, .range_bpg_offset = 56},
		{.range_min_qp = 3, .range_max_qp = 9, .range_bpg_offset = 56},
		{.range_min_qp = 3, .range_max_qp = 10, .range_bpg_offset = 54},
		{.range_min_qp = 5, .range_max_qp = 11, .range_bpg_offset = 54},
		{.range_min_qp = 5, .range_max_qp = 12, .range_bpg_offset = 52},
		{.range_min_qp = 5, .range_max_qp = 13, .range_bpg_offset = 52},
		{.range_min_qp = 7, .range_max_qp = 13, .range_bpg_offset = 52},
		{.range_min_qp = 13, .range_max_qp = 15, .range_bpg_offset = 52}
	},
	.rc_model_size = 8192,
	.flatness_min_qp = 3,
	.flatness_max_qp = 12,
	.initial_scale_value = 32,
	.scale_decrement_interval = 6,
	.scale_increment_interval = 977,
	.nfl_bpg_offset = 600,
	.slice_bpg_offset = 698,
	.final_offset = 4336,
	.vbr_enable = false,
	.slice_chunk_size = 480,
	.dsc_version_minor = 2,
	.dsc_version_major = 1,
	.native_422 = false,
	.native_420 = false,
	.second_line_bpg_offset = 0,
	.nsl_bpg_offset = 0,
	.second_line_offset_adj = 0,
};

#define CM4_WRCTRLD_DIMMING_BIT 0x08
#define CM4_WRCTRLD_BCTRL_BIT 0x20
#define CM4_WRCTRLD_HBM_BIT 0xC0

#define CM4_TE2_CHANGEABLE 0x04
#define CM4_TE2_FIXED 0x51

#define CM4_TE2_RISING_EDGE_OFFSET 0x20
#define CM4_TE2_FALLING_EDGE_OFFSET 0x57

#define CM4_TE_USEC_120HZ_HS 273
#define CM4_TE_USEC_60HZ_HS 8500
#define CM4_TE_USEC_60HZ_NS 546

#define CM4_TE_USEC_VRR_HS 273
#define CM4_TE_USEC_VRR_NS 546

#define WIDTH_MM 66
#define HEIGHT_MM 147

#define PROJECT "CM4"

static const u8 unlock_cmd_f0[] = { 0xF0, 0x5A, 0x5A };
static const u8 lock_cmd_f0[] = { 0xF0, 0xA5, 0xA5 };
static const u8 freq_update[] = { 0xF7, 0x0F };
static const u8 aod_on[] = { MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24 };
static const u8 aod_off[] = { MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20 };
static const u8 pixel_off[] = { 0x22 };

static const struct gs_dsi_cmd cm4_lp_low_cmds[] = {
	/* AOD Low Mode, 2nit */
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00, 0xB0),
};

static const struct gs_dsi_cmd cm4_lp_med_cmds[] = {
	/* AOD Med Mode, 10nit */
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x01, 0x6D),
};

static const struct gs_dsi_cmd cm4_lp_high_cmds[] = {
	/* AOD High Mode, 50nit */
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x02, 0xF6),
};

static const struct gs_binned_lp cm4_binned_lp[] = {
	/* low threshold 4 nits */
	BINNED_LP_MODE_TIMING("low", 240, cm4_lp_low_cmds, CM4_TE2_RISING_EDGE_OFFSET,
			      CM4_TE2_FALLING_EDGE_OFFSET),
	/* low threshold 40 nits */
	BINNED_LP_MODE_TIMING("med", 686, cm4_lp_med_cmds, CM4_TE2_RISING_EDGE_OFFSET,
			      CM4_TE2_FALLING_EDGE_OFFSET),
	BINNED_LP_MODE_TIMING("high", 3271, cm4_lp_high_cmds, CM4_TE2_RISING_EDGE_OFFSET,
			      CM4_TE2_FALLING_EDGE_OFFSET),
};

static inline bool is_in_comp_range(int temp)
{
	return (temp >= 10 && temp <= 49);
}

/* Read temperature and apply appropriate gain into DDIC for burn-in compensation if needed */
static void cm4_update_disp_therm(struct gs_panel *ctx)
{
	/* temperature*1000 in celsius */
	int temp, ret;
	struct device *dev = ctx->dev;

	if (!ctx->thermal || IS_ERR_OR_NULL(ctx->thermal->tz))
		return;

	if (ctx->panel_state != GPANEL_STATE_NORMAL)
		return;

	ctx->thermal->pending_temp_update = false;

	ret = thermal_zone_get_temp(ctx->thermal->tz, &temp);
	if (ret) {
		dev_err(dev, "%s: fail to read temperature ret:%d\n", __func__, ret);
		return;
	}

	temp = DIV_ROUND_CLOSEST(temp, 1000);
	dev_dbg(dev, "%s: temp=%d\n", __func__, temp);
	if (temp == ctx->thermal->hw_temp || !is_in_comp_range(temp))
		return;

	DPU_ATRACE_BEGIN(__func__);
	GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x03, 0x67);
	GS_DCS_BUF_ADD_CMD(dev, 0x67, temp);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);
	DPU_ATRACE_END(__func__);

	ctx->thermal->hw_temp = temp;
}

static u8 cm4_get_te2_option(struct gs_panel *ctx)
{
	struct cm4_panel *spanel = to_spanel(ctx);

	if (!ctx->current_mode || spanel->force_changeable_te2)
		return CM4_TE2_CHANGEABLE;

	if (ctx->current_mode->gs_mode.is_lp_mode ||
	    (test_bit(FEAT_EARLY_EXIT, ctx->sw_status.feat) && ctx->throttled_min_vrefresh < 30))
		return CM4_TE2_FIXED;

	return CM4_TE2_CHANGEABLE;
}

static void cm4_update_te2_internal(struct gs_panel *ctx, bool lock)
{
	struct gs_panel_te2_timing timing = {
		.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
		.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
	};
	u32 rising, falling;
	struct device *dev = ctx->dev;
	u8 option = cm4_get_te2_option(ctx);
	u8 idx;

	/* skip TE2 update if going through RRS */
	if (ctx->mode_in_progress == MODE_RES_IN_PROGRESS ||
	    ctx->mode_in_progress == MODE_RES_AND_RR_IN_PROGRESS) {
		dev_dbg(dev, "%s: RRS in progress, skip\n", __func__);
		return;
	}

	if (gs_panel_get_current_mode_te2(ctx, &timing)) {
		dev_dbg(dev, "failed to get TE2 timng\n");
		return;
	}
	rising = timing.rising_edge;
	falling = timing.falling_edge;

	ctx->te2.option = (option == CM4_TE2_FIXED) ? GTE2_OPT_FIXED : GTE2_OPT_CHANGEABLE;

	dev_dbg(dev, "TE2 updated: %s mode, option %s, idle %s, rising=0x%X falling=0x%X\n",
		test_bit(FEAT_OP_NS, ctx->sw_status.feat) ? "NS" : "HS",
		(option == CM4_TE2_CHANGEABLE) ? "changeable" : "fixed",
		ctx->idle_data.panel_idle_vrefresh ? "active" : "inactive", rising, falling);

	if (lock)
		GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x42, 0xF2);
	GS_DCS_BUF_ADD_CMD(dev, 0xF2, 0x0D); // TE2 ON
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x01, 0xB9);
	GS_DCS_BUF_ADD_CMD(dev, 0xB9, option); // TE2 type setting
	idx = option == CM4_TE2_FIXED ? 0x22 : 0x1E;
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, idx, 0xB9);
	if (option == CM4_TE2_FIXED) {
		GS_DCS_BUF_ADD_CMD(dev, 0xB9, (rising >> 8) & 0xF, rising & 0xFF,
				   (falling >> 8) & 0xF, falling & 0xFF, (rising >> 8) & 0xF,
				   rising & 0xFF, (falling >> 8) & 0xF, falling & 0xFF);
	} else {
		GS_DCS_BUF_ADD_CMD(dev, 0xB9, (rising >> 8) & 0xF, rising & 0xFF,
				   (falling >> 8) & 0xF, falling & 0xFF);
	}
	if (lock)
		GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);
}

static void cm4_update_te2(struct gs_panel *ctx)
{
	cm4_update_te2_internal(ctx, true);
}

static inline bool is_auto_mode_allowed(struct gs_panel *ctx)
{
	/* don't want to enable auto mode/early exit during dimming on */
	if (ctx->dimming_on)
		return false;

	if (ctx->idle_data.idle_delay_ms) {
		const unsigned int delta_ms = gs_panel_get_idle_time_delta(ctx);

		if (delta_ms < ctx->idle_data.idle_delay_ms)
			return false;
	}

	return ctx->idle_data.panel_idle_enabled;
}

static u32 cm4_get_min_idle_vrefresh(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	const int vrefresh = drm_mode_vrefresh(&pmode->mode);
	int min_idle_vrefresh = ctx->min_vrefresh;

	if ((min_idle_vrefresh < 0) || !is_auto_mode_allowed(ctx))
		return 0;

	if (min_idle_vrefresh <= 1)
		min_idle_vrefresh = 1;
	else if (min_idle_vrefresh <= 10)
		min_idle_vrefresh = 10;
	else if (min_idle_vrefresh <= 30)
		min_idle_vrefresh = 30;
	else
		return 0;

	if (min_idle_vrefresh >= vrefresh) {
		dev_dbg(ctx->dev, "min idle vrefresh (%d) higher than target (%d)\n",
			min_idle_vrefresh, vrefresh);
		return 0;
	}

	dev_dbg(ctx->dev, "%s: min_idle_vrefresh %d\n", __func__, min_idle_vrefresh);

	return min_idle_vrefresh;
}

static void cm4_set_panel_feat_auto_fi(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	bool enabled;
	u8 val;

	enabled = test_bit(FEAT_FI_AUTO, ctx->sw_status.feat);
	val = enabled ? 0x33 : 0x00;

	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x10, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, val);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x82, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, val, val);

	if (!enabled) {
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x80, 0xBD);
		GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x16);
	}

	dev_dbg(ctx->dev, "%s: auto fi %s\n", __func__, enabled ? "enabled" : "disabled");
}

/**
 * cm4_set_panel_feat - configure panel features
 * @ctx: gs_panel struct
 * @pmode: gs_panel_mode struct, target panel mode
 * @idle_vrefresh: target vrefresh rate in auto mode, 0 if disabling auto mode
 * @enforce: force to write all of registers even if no feature state changes
 *
 * Configure panel features based on the context.
 */
static void cm4_set_panel_feat(struct gs_panel *ctx, const struct gs_panel_mode *pmode,
			       u32 idle_vrefresh, bool enforce)
{
	struct cm4_panel *spanel = to_spanel(ctx);
	struct device *dev = ctx->dev;
	unsigned long *feat = ctx->sw_status.feat;
	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);
	u32 te_freq = gs_drm_mode_te_freq(&pmode->mode);
	bool is_vrr = gs_is_vrr_mode(pmode);
	bool irc_mode_changed;
	u8 val;
	DECLARE_BITMAP(changed_feat, FEAT_MAX);

#ifndef PANEL_FACTORY_BUILD
	vrefresh = 1;
	idle_vrefresh = 0;
	set_bit(FEAT_EARLY_EXIT, feat);
	clear_bit(FEAT_FRAME_AUTO, feat);
	if (is_vrr) {
		if (pmode->mode.flags & DRM_MODE_FLAG_NS)
			set_bit(FEAT_OP_NS, feat);
		else
			clear_bit(FEAT_OP_NS, feat);
	}
#endif

	if (enforce) {
		bitmap_fill(changed_feat, FEAT_MAX);
		irc_mode_changed = true;
	} else {
		bitmap_xor(changed_feat, feat, ctx->hw_status.feat, FEAT_MAX);
		if (bitmap_empty(changed_feat, FEAT_MAX) && vrefresh == ctx->hw_status.vrefresh &&
		    idle_vrefresh == ctx->hw_status.idle_vrefresh &&
		    te_freq == ctx->hw_status.te_freq) {
			dev_dbg(dev, "%s: no changes, skip update\n", __func__);
			return;
		}
		irc_mode_changed = (ctx->sw_status.irc_mode == ctx->hw_status.irc_mode);
	}

	dev_dbg(dev, "ns=%d ee=%d hbm=%d irc=%d auto=%d fi=%d fps=%u idle_fps=%u te=%u vrr=%d\n",
		test_bit(FEAT_OP_NS, feat), test_bit(FEAT_EARLY_EXIT, feat),
		test_bit(FEAT_HBM, feat), ctx->sw_status.irc_mode, test_bit(FEAT_FRAME_AUTO, feat),
		test_bit(FEAT_FI_AUTO, feat), vrefresh, idle_vrefresh, te_freq, is_vrr);

	GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);

	/* TE setting */
	if (test_bit(FEAT_EARLY_EXIT, changed_feat) || test_bit(FEAT_OP_NS, changed_feat) ||
	    ctx->hw_status.te_freq != te_freq) {
		if (test_bit(FEAT_EARLY_EXIT, feat) && !spanel->force_changeable_te) {
			if (is_vrr && te_freq == 240) {
				/* 240Hz multi TE */
				GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x61);
				/* TE width */
				GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x14, 0xB9);
				GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x05, 0xA0, 0x00, 0x28, 0x05, 0x80);
				GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x08, 0xB9);
				if (test_bit(FEAT_OP_NS, feat))
					GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x0B, 0x4A, 0x00, 0x1F, 0x02, 0xC2,
							   0x00, 0x1F);
				else
					GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x0B, 0x1F, 0x00, 0x1F, 0x05, 0x6F,
							   0x00, 0x1F);
			} else {
				/* Fixed TE */
				GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x51);
				/* TE width */
				GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x08, 0xB9);
				if (ctx->panel_rev >= PANEL_REV_EVT1)
					GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x0B, 0x1E, 0x00, 0x1F, 0x0B, 0x1E,
							   0x00, 0x1F);
				else
					GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x0B, 0x0E, 0x00, 0x1F, 0x0B, 0x0E,
							   0x00, 0x1F);
#ifndef PANEL_FACTORY_BUILD
				val = (drm_mode_vrefresh(&pmode->mode) == 120) ||
						test_bit(FEAT_OP_NS, feat) ? 0x00 : 0x01;
				GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x02, 0xB9);
				GS_DCS_BUF_ADD_CMD(dev, 0xB9, val);
#endif
			}

		} else {
			/* Changeable TE */
			GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x04);
			/* TE width */
			GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x04, 0xB9);
			if (ctx->panel_rev >= PANEL_REV_EVT1)
				GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x0B, 0x1E, 0x00, 0x1F);
			else
				GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x0B, 0x0E, 0x00, 0x1F);
		}
	}

	/* TE2 setting */
	if (test_bit(FEAT_OP_NS, changed_feat))
		cm4_update_te2_internal(ctx, false);

	/*
	 * HBM IRC setting
	 *
	 * "Flat mode" is used to replace IRC on for normal mode and HDR video,
	 * and "Flat Z mode" is used to replace IRC off for sunlight
	 * environment.
	 */
	if (irc_mode_changed) {
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x01, 0x9B, 0x92);
		if (unlikely(ctx->sw_status.irc_mode == IRC_OFF))
			GS_DCS_BUF_ADD_CMD(dev, 0x92, 0x07);
		else /* IRC_FLAT_DEFAULT or IRC_FLAT_Z */
			GS_DCS_BUF_ADD_CMD(dev, 0x92, 0x27);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x02, 0x00, 0x92);
		if (ctx->sw_status.irc_mode == IRC_FLAT_Z)
			GS_DCS_BUF_ADD_CMD(dev, 0x92, 0x00, 0x00, 0xFF, 0xD0);
		else /* IRC_OFF or IRC_FLAT_DEFAULT */
			GS_DCS_BUF_ADD_CMD(dev, 0x92, 0x70, 0x26, 0xFF, 0xDC);
	}

	/*
	 * Operating Mode: NS or HS
	 *
	 * Description: the configs could possibly be overrided by frequency setting,
	 * depending on FI mode.
	 */
	if (test_bit(FEAT_OP_NS, changed_feat)) {
		/* mode set */
		GS_DCS_BUF_ADD_CMD(dev, 0xF2, 0x01);
		val = test_bit(FEAT_OP_NS, feat) ? 0x18 : 0x00;
		GS_DCS_BUF_ADD_CMD(dev, 0x60, val);
	}

	/*
	 * Early-exit: enable or disable
	 */
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x01, 0xBD);
	val = (test_bit(FEAT_EARLY_EXIT, feat) && vrefresh != 80) ? 0x01 : 0x81;
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, val);
#ifdef PANEL_FACTORY_BUILD
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x10, 0xBD);
	val = test_bit(FEAT_EARLY_EXIT, feat) ? 0x22 : 0x00;
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, val);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x82, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, val, val, val, val);
#endif


	/*
	 * Auto FI: enable or disable
	 */
	if (test_bit(FEAT_FI_AUTO, changed_feat))
		cm4_set_panel_feat_auto_fi(ctx);

	/*
	 * Frequency setting: FI, frequency, idle frequency
	 *
	 * Description: this sequence possibly overrides some configs early-exit
	 * and operation set, depending on FI mode.
	 */
	if (test_bit(FEAT_FRAME_AUTO, feat)) {
		if (test_bit(FEAT_OP_NS, feat)) {
			/* threshold setting */
			GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x0C, 0xBD);
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x00);
		} else {
			/* initial frequency */
			GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x92, 0xBD);
			if (vrefresh == 60) {
				val = test_bit(FEAT_HBM, feat) ? 0x01 : 0x02;
			} else {
				if (vrefresh != 120)
					dev_warn(dev, "%s: unsupported init freq %d (hs)\n",
						 __func__, vrefresh);
				/* 120Hz */
				val = 0x00;
			}
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, val);
		}
		/* target frequency */
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x12, 0xBD);
		if (test_bit(FEAT_OP_NS, feat)) {
			if (idle_vrefresh == 30) {
				val = test_bit(FEAT_HBM, feat) ? 0x02 : 0x04;
			} else if (idle_vrefresh == 10) {
				val = test_bit(FEAT_HBM, feat) ? 0x0A : 0x14;
			} else {
				if (idle_vrefresh != 1)
					dev_warn(dev, "%s: unsupported target freq %d (ns)\n",
						 __func__, idle_vrefresh);
				/* 1Hz */
				val = test_bit(FEAT_HBM, feat) ? 0x76 : 0xEC;
			}
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x00, val);
		} else {
			if (idle_vrefresh == 30) {
				val = test_bit(FEAT_HBM, feat) ? 0x03 : 0x06;
			} else if (idle_vrefresh == 10) {
				val = test_bit(FEAT_HBM, feat) ? 0x0B : 0x16;
			} else {
				if (idle_vrefresh != 1)
					dev_warn(dev, "%s: unsupported target freq %d (hs)\n",
						 __func__, idle_vrefresh);
				/* 1Hz */
				val = test_bit(FEAT_HBM, feat) ? 0x77 : 0xEE;
			}
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x00, val);
		}
		/* step setting */
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x9E, 0xBD);
		if (test_bit(FEAT_OP_NS, feat)) {
			if (test_bit(FEAT_HBM, feat))
				GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x02, 0x00, 0x0A, 0x00, 0x00);
			else
				GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x04, 0x00, 0x14, 0x00, 0x00);
		} else {
			if (test_bit(FEAT_HBM, feat))
				GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x01, 0x00, 0x03, 0x00, 0x0B);
			else
				GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x02, 0x00, 0x06, 0x00, 0x16);
		}
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0xAE, 0xBD);
		if (test_bit(FEAT_OP_NS, feat)) {
			if (idle_vrefresh == 30) {
				/* 60Hz -> 30Hz idle */
				GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x00, 0x00);
			} else if (idle_vrefresh == 10) {
				/* 60Hz -> 10Hz idle */
				GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x01, 0x00, 0x00);
			} else {
				if (idle_vrefresh != 1)
					dev_warn(dev, "%s: unsupported freq step to %d (ns)\n",
						 __func__, idle_vrefresh);
				/* 60Hz -> 1Hz idle */
				GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x01, 0x03, 0x00);
			}
		} else {
			if (vrefresh == 60) {
				if (idle_vrefresh == 30) {
					/* 60Hz -> 30Hz idle */
					GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x01, 0x00, 0x00);
				} else if (idle_vrefresh == 10) {
					/* 60Hz -> 10Hz idle */
					GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x01, 0x01, 0x00);
				} else {
					if (idle_vrefresh != 1)
						dev_warn(dev,
							 "%s: unsupported freq step to %d (hs)\n",
							 __func__, vrefresh);
					/* 60Hz -> 1Hz idle */
					GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x01, 0x01, 0x03);
				}
			} else {
				if (vrefresh != 120)
					dev_warn(dev, "%s: unsupported freq step from %d (hs)\n",
						 __func__, vrefresh);
				if (idle_vrefresh == 30) {
					/* 120Hz -> 30Hz idle */
					GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x00, 0x00);
				} else if (idle_vrefresh == 10) {
					/* 120Hz -> 10Hz idle */
					GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x03, 0x00);
				} else {
					if (idle_vrefresh != 1)
						dev_warn(dev,
							 "%s: unsupported freq step to %d (hs)\n",
							 __func__, idle_vrefresh);
					/* 120Hz -> 1Hz idle */
					GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x01, 0x03);
				}
			}
		}
		GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0xA3);
	} else { /* manual */
		if (is_vrr) {
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x21, 0x41);
		} else {
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x21);
		}
		if (test_bit(FEAT_OP_NS, feat)) {
			if (vrefresh == 1) {
				val = 0x1F;
			} else if (vrefresh == 10) {
				val = 0x1B;
			} else if (vrefresh == 30) {
				val = 0x19;
			} else {
				if (vrefresh != 60)
					dev_warn(dev, "%s: unsupported manual freq %d (ns)\n",
						 __func__, vrefresh);
				/* 60Hz */
				val = 0x18;
			}
		} else {
			if (vrefresh == 1) {
				val = 0x07;
			} else if (vrefresh == 10) {
				val = 0x03;
			} else if (vrefresh == 30) {
				val = 0x02;
			} else if (vrefresh == 60) {
				val = 0x01;
			} else if (vrefresh == 80) {
				val = 0x04;
			} else {
				if (vrefresh != 120)
					dev_warn(dev, "%s: unsupported manual freq %d (hs)\n",
						 __func__, vrefresh);
				/* 120Hz */
				val = 0x00;
			}
		}
		GS_DCS_BUF_ADD_CMD(dev, 0x60, val);
	}

	GS_DCS_BUF_ADD_CMDLIST(dev, freq_update);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);

	ctx->hw_status.vrefresh = vrefresh;
	ctx->hw_status.idle_vrefresh = idle_vrefresh;
	ctx->hw_status.te_freq = te_freq;
	bitmap_copy(ctx->hw_status.feat, feat, FEAT_MAX);
}

/**
 * cm4_update_panel_feat - configure panel features with current refresh rate
 * @ctx: gs_panel struct
 * @enforce: force to write all of registers even if no feature state changes
 *
 * Configure panel features based on the context without changing current refresh rate
 * and idle setting.
 */
static void cm4_update_panel_feat(struct gs_panel *ctx, bool enforce)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;
	u32 idle_vrefresh = ctx->throttled_min_vrefresh;

	cm4_set_panel_feat(ctx, pmode, idle_vrefresh, enforce);
	schedule_work(&ctx->state_notify);
}

static void cm4_update_refresh_mode(struct gs_panel *ctx, const struct gs_panel_mode *pmode,
				    const u32 idle_vrefresh)
{
	/* TODO: b/308978878 - move refresh control logic to HWC */

	/*
	 * Skip idle update if going through RRS without refresh rate change. If
	 * we're switching resolution and refresh rate in the same atomic commit
	 * (MODE_RES_AND_RR_IN_PROGRESS), we shouldn't skip the update to
	 * ensure the refresh rate will be set correctly to avoid problems.
	 */
	if (ctx->mode_in_progress == MODE_RES_IN_PROGRESS) {
		dev_dbg(ctx->dev, "%s: RRS in progress without RR change, skip\n", __func__);
		return;
	}

	dev_dbg(ctx->dev, "%s: mode: %s set idle_vrefresh: %u\n", __func__, pmode->mode.name,
		idle_vrefresh);

	ctx->throttled_min_vrefresh = idle_vrefresh;
	/*
	 * Note: when mode is explicitly set, panel performs early exit to get out
	 * of idle at next vsync, and will not back to idle until not seeing new
	 * frame traffic for a while. If idle_vrefresh != 0, try best to guess what
	 * panel_idle_vrefresh will be soon, and cm4_update_idle_state() in
	 * new frame commit will correct it if the guess is wrong.
	 */
	ctx->idle_data.panel_idle_vrefresh = idle_vrefresh;
	cm4_set_panel_feat(ctx, pmode, idle_vrefresh, false);
	schedule_work(&ctx->state_notify);

	dev_dbg(ctx->dev, "%s: display state is notified\n", __func__);
}

static void cm4_change_frequency(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);
	u32 idle_vrefresh = 0;

	if (vrefresh > ctx->op_hz) {
		dev_err(ctx->dev, "invalid freq setting: op_hz=%u, vrefresh=%u\n", ctx->op_hz,
			vrefresh);
		return;
	}

	if (pmode->idle_mode == GIDLE_MODE_ON_INACTIVITY)
		idle_vrefresh = cm4_get_min_idle_vrefresh(ctx, pmode);

	cm4_update_refresh_mode(ctx, pmode, idle_vrefresh);

	dev_dbg(ctx->dev, "change to %u hz\n", vrefresh);
}

// todo: understand how idle works
static void cm4_panel_idle_notification(struct gs_panel *ctx, u32 display_id, u32 vrefresh,
					u32 idle_te_vrefresh)
{
	char event_string[64];
	char *envp[] = { event_string, NULL };
	struct drm_device *dev = ctx->bridge.dev;

	if (!dev) {
		dev_dbg(ctx->dev, "%s: drm_device is null\n", __func__);
	} else {
		snprintf(event_string, sizeof(event_string), "PANEL_IDLE_ENTER=%u,%u,%u",
			 display_id, vrefresh, idle_te_vrefresh);
		kobject_uevent_env(&dev->primary->kdev->kobj, KOBJ_CHANGE, envp);
	}
}

static void cm4_wait_one_vblank(struct gs_panel *ctx)
{
	struct drm_crtc *crtc = NULL;

	if (ctx->gs_connector->base.state)
		crtc = ctx->gs_connector->base.state->crtc;

	DPU_ATRACE_BEGIN(__func__);
	if (crtc) {
		int ret = drm_crtc_vblank_get(crtc);

		if (!ret) {
			drm_crtc_wait_one_vblank(crtc);
			drm_crtc_vblank_put(crtc);
		} else {
			usleep_range(8350, 8500);
		}
	} else {
		usleep_range(8350, 8500);
	}
	DPU_ATRACE_END(__func__);
}

static void cm4_refresh_ctrl(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	u32 ctrl = ctx->refresh_ctrl;

	DPU_ATRACE_BEGIN(__func__);

	if (ctrl & GS_PANEL_REFRESH_CTRL_FI_AUTO) {
		set_bit(FEAT_FI_AUTO, ctx->sw_status.feat);
		cm4_update_panel_feat(ctx, false);
	} else {
		clear_bit(FEAT_FI_AUTO, ctx->sw_status.feat);
		cm4_update_panel_feat(ctx, false);

		if (ctrl & GS_PANEL_REFRESH_CTRL_FI_REFRESH_RATE_MASK) {
			/* TODO(b/323251635): support setting frame insertion rate */
			dev_warn(dev, "%s: setting frame insertion rate unsupported\n", __func__);
		} else {
			/* TODO(b/323251635): parse frame count for inserting multiple frames */

			dev_dbg(dev, "%s: manually inserting frame\n", __func__);
			GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);
			GS_DCS_BUF_ADD_CMD(dev, 0xF7, 0x02);
			GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);
		}
	}

	DPU_ATRACE_END(__func__);
}

static bool cm4_set_self_refresh(struct gs_panel *ctx, bool enable)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;
	u32 idle_vrefresh;

	dev_dbg(ctx->dev, "%s: %d\n", __func__, enable);

	if (unlikely(!pmode))
		return false;

	/* self refresh is not supported in lp mode since that always makes use of early exit */
	if (pmode->gs_mode.is_lp_mode) {
		/* set 1Hz while self refresh is active, otherwise clear it */
		ctx->idle_data.panel_idle_vrefresh = enable ? 1 : 0;
		schedule_work(&ctx->state_notify);
		return false;
	}

	if (ctx->thermal && ctx->thermal->pending_temp_update && enable)
		cm4_update_disp_therm(ctx);

	idle_vrefresh = cm4_get_min_idle_vrefresh(ctx, pmode);

	if (pmode->idle_mode != GIDLE_MODE_ON_SELF_REFRESH) {
		/*
		 * if idle mode is on inactivity, may need to update the target fps for auto mode,
		 * or switch to manual mode if idle should be disabled (idle_vrefresh=0)
		 */
		if ((pmode->idle_mode == GIDLE_MODE_ON_INACTIVITY) &&
		    (ctx->throttled_min_vrefresh != idle_vrefresh)) {
			cm4_update_refresh_mode(ctx, pmode, idle_vrefresh);
			return true;
		}
		return false;
	}

	if (!enable)
		idle_vrefresh = 0;

	/* if there's no change in idle state then skip cmds */
	if (ctx->idle_data.panel_idle_vrefresh == idle_vrefresh)
		return false;

	DPU_ATRACE_BEGIN(__func__);
	cm4_update_refresh_mode(ctx, pmode, idle_vrefresh);

	if (idle_vrefresh) {
		const int vrefresh = drm_mode_vrefresh(&pmode->mode);

		cm4_panel_idle_notification(ctx, 0, vrefresh, 120);
	} else if (ctx->idle_data.panel_need_handle_idle_exit) {
		/*
		 * after exit idle mode with fixed TE at non-120hz, TE may still keep at 120hz.
		 * If any layer that already be assigned to DPU that can't be handled at 120hz,
		 * panel_need_handle_idle_exit will be set then we need to wait one vblank to
		 * avoid underrun issue.
		 */
		dev_dbg(ctx->dev, "wait one vblank after exit idle\n");
		cm4_wait_one_vblank(ctx);
	}

	DPU_ATRACE_END(__func__);

	return true;
}

static int cm4_atomic_check(struct gs_panel *ctx, struct drm_atomic_state *state)
{
	struct drm_connector *conn = &ctx->gs_connector->base;
	struct drm_connector_state *new_conn_state =
		drm_atomic_get_new_connector_state(state, conn);
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;

	if (!ctx->current_mode || drm_mode_vrefresh(&ctx->current_mode->mode) == 120 ||
	    !new_conn_state || !new_conn_state->crtc)
		return 0;

	new_crtc_state = drm_atomic_get_new_crtc_state(state, new_conn_state->crtc);
	old_crtc_state = drm_atomic_get_old_crtc_state(state, new_conn_state->crtc);
	if (!old_crtc_state || !new_crtc_state || !new_crtc_state->active)
		return 0;

	if ((ctx->throttled_min_vrefresh && old_crtc_state->self_refresh_active) ||
	    !drm_atomic_crtc_effectively_active(old_crtc_state)) {
		struct drm_display_mode *mode = &new_crtc_state->adjusted_mode;

		/* set clock to max refresh rate on self refresh exit or resume due to early exit */
		mode->clock = mode->htotal * mode->vtotal * 120 / 1000;

		if (mode->clock != new_crtc_state->mode.clock) {
			new_crtc_state->mode_changed = true;
			dev_dbg(ctx->dev, "raise mode (%s) clock to 120hz on %s\n", mode->name,
				old_crtc_state->self_refresh_active ? "self refresh exit" :
								      "resume");
		}
	} else if (old_crtc_state->active_changed &&
		   (old_crtc_state->adjusted_mode.clock != old_crtc_state->mode.clock)) {
		/* clock hacked in last commit due to self refresh exit or resume, undo that */
		new_crtc_state->mode_changed = true;
		new_crtc_state->adjusted_mode.clock = new_crtc_state->mode.clock;
		dev_dbg(ctx->dev, "restore mode (%s) clock after self refresh exit or resume\n",
			new_crtc_state->mode.name);
	}

	return 0;
}

static void cm4_write_display_mode(struct gs_panel *ctx, const struct drm_display_mode *mode)
{
	struct device *dev = ctx->dev;
	u8 val = CM4_WRCTRLD_BCTRL_BIT;

	if (GS_IS_HBM_ON(ctx->hbm_mode))
		val |= CM4_WRCTRLD_HBM_BIT;

	if (ctx->dimming_on)
		val |= CM4_WRCTRLD_DIMMING_BIT;

	dev_dbg(dev, "%s(wrctrld:%#x, hbm: %d, dimming: %d)\n", __func__, val,
		GS_IS_HBM_ON(ctx->hbm_mode), ctx->dimming_on);

	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY, val);
}

static void cm4_update_za(struct gs_panel *ctx)
{
	struct cm4_panel *spanel = to_spanel(ctx);
	struct device *dev = ctx->dev;
	bool enable_za = false;

	if ((ctx->hw_status.acl_mode > ACL_OFF) && !spanel->force_za_off)
		enable_za = true;

	if (test_bit(FEAT_ZA, ctx->hw_status.feat) != enable_za) {
		/* LP setting - 0x21 or 0x11: 7.5%, 0x00: off */
		u8 val = 0;

		GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x01, 0x6C, 0x92);
		if (enable_za)
			val = 0x11;
		GS_DCS_BUF_ADD_CMD(dev, 0x92, val);
		GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);

		if (enable_za)
			set_bit(FEAT_ZA, ctx->hw_status.feat);
		else
			clear_bit(FEAT_ZA, ctx->hw_status.feat);
		dev_dbg(dev, "%s: %d\n", __func__, enable_za);
	}
}

#define CM4_ACL_ENHANCED_THRESHOLD_DBV 3726

static u8 get_acl_mode_setting(enum gs_acl_mode acl_mode, u32 panel_rev)
{
	/*
	 * CM4 ACL mode and setting:
	 *
	 * ENHANCED is default mode
	 *
	 * Till EVT1_1
	 *    ENHANCED   - 12%   (0x03)
	 * DVT1
	 *    NORMAL     - 12.5% (0x02)
	 * LATER
	 *    NORMAL     - 15%   (0x02)
	 */

	if (acl_mode != ACL_OFF && panel_rev >= PANEL_REV_DVT1) {
		acl_mode = ACL_NORMAL;
	}

	switch (acl_mode) {
	case ACL_OFF:
		return 0x00;
	case ACL_NORMAL:
		return 0x02;
	case ACL_ENHANCED:
		return 0x03;
	}
}

/* updated za when acl mode changed */
static void cm4_set_acl_mode(struct gs_panel *ctx, enum gs_acl_mode mode)
{
	struct device *dev = ctx->dev;
	u16 dbv_th = CM4_ACL_ENHANCED_THRESHOLD_DBV;
	bool can_enable_acl = (ctx->hw_status.dbv >= dbv_th && GS_IS_HBM_ON(ctx->hbm_mode));

	if (can_enable_acl && mode != ACL_OFF)
		ctx->sw_status.acl_mode = mode;
	else
		ctx->sw_status.acl_mode = ACL_OFF;

	if (ctx->sw_status.acl_mode != ctx->hw_status.acl_mode) {
		u8 setting = get_acl_mode_setting(ctx->sw_status.acl_mode, ctx->panel_rev);

		GS_DCS_WRITE_CMD(dev, 0x55, setting);
		ctx->hw_status.acl_mode = ctx->sw_status.acl_mode;
		dev_dbg(dev, "%s: %d\n", __func__, setting);
	}
}

static int cm4_set_brightness(struct gs_panel *ctx, u16 br)
{
	int ret;
	u16 brightness;
	struct cm4_panel *spanel = to_spanel(ctx);
	struct device *dev = ctx->dev;

	if (ctx->current_mode->gs_mode.is_lp_mode) {
		/* don't stay at pixel-off state in AOD, or black screen is possibly seen */
		if (spanel->is_pixel_off) {
			GS_DCS_WRITE_CMD(dev, MIPI_DCS_ENTER_NORMAL_MODE);
			spanel->is_pixel_off = false;
		}

		if (gs_panel_has_func(ctx, set_binned_lp))
			ctx->desc->gs_panel_func->set_binned_lp(ctx, br);
		return 0;
	}

	/* Use pixel off command instead of setting DBV 0 */
	if (!br) {
		if (!spanel->is_pixel_off) {
			GS_DCS_WRITE_CMDLIST(dev, pixel_off);
			spanel->is_pixel_off = true;
			dev_dbg(dev, "%s: pixel off instead of dbv 0\n", __func__);
		}
		return 0;
	}
	if (spanel->is_pixel_off) {
		GS_DCS_WRITE_CMD(dev, MIPI_DCS_ENTER_NORMAL_MODE);
		spanel->is_pixel_off = false;
	}

	brightness = swab16(br);
	ret = gs_dcs_set_brightness(ctx, brightness);
	if (!ret) {
		ctx->hw_status.dbv = br;
		cm4_set_acl_mode(ctx, ctx->sw_status.acl_mode);
	}

	return ret;
}

static unsigned int cm4_get_te_usec(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	const int vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (vrefresh != 60 || gs_is_vrr_mode(pmode))
		return pmode->gs_mode.te_usec;
#ifdef PANEL_FACTORY_BUILD
	return (test_bit(FEAT_OP_NS, ctx->sw_status.feat) ? CM4_TE_USEC_60HZ_NS :
							    CM4_TE_USEC_60HZ_HS);
#else
	return (test_bit(FEAT_OP_NS, ctx->sw_status.feat) ? CM4_TE_USEC_VRR_NS :
							    CM4_TE_USEC_VRR_HS);
#endif
}

static void cm4_wait_for_vsync_done(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	DPU_ATRACE_BEGIN(__func__);
	gs_panel_wait_for_vsync_done(ctx, cm4_get_te_usec(ctx, pmode),
				     GS_VREFRESH_TO_PERIOD_USEC(ctx->hw_status.vrefresh));
	DPU_ATRACE_END(__func__);
}

static void cm4_enforce_manual_and_peak(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;

	if (!ctx->current_mode)
		return;

	dev_dbg(dev, "%s\n", __func__);

	GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);
	/* manual mode */
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x21);
	/* peak refresh rate */
	if (ctx->current_mode->gs_mode.is_lp_mode) {
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x01, 0x60);
		GS_DCS_BUF_ADD_CMD(dev, 0x60, 0x00);
	} else {
		GS_DCS_BUF_ADD_CMD(dev, 0x60,
				   !test_bit(FEAT_OP_NS, ctx->sw_status.feat) ? 0x00 : 0x18);
	}
	GS_DCS_BUF_ADD_CMDLIST(dev, freq_update);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);
}

static void cm4_set_lp_mode(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;

	dev_dbg(dev, "%s\n", __func__);

	DPU_ATRACE_BEGIN(__func__);

	/* enforce manual and peak to have a smooth transition */
	cm4_enforce_manual_and_peak(ctx);

	cm4_wait_for_vsync_done(ctx, pmode);
	GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);
	GS_DCS_BUF_ADD_CMDLIST(dev, aod_on);
	/* Fixed TE: sync on */
	GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x51);
	/* Auto frame insertion: 1Hz */
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x18, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x04, 0x00, 0x74);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0xB8, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x08);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0xC8, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x03);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0xA7);
	/* Enable early exit */
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0xE8, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x10, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x22);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x82, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x22, 0x22, 0x22, 0x22);
	GS_DCS_BUF_ADD_CMDLIST(dev, freq_update);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);

	ctx->hw_status.vrefresh = 30;
	ctx->hw_status.te_freq = 30;

	DPU_ATRACE_END(__func__);

	dev_info(dev, "enter %dhz LP mode\n", drm_mode_vrefresh(&pmode->mode));
}

static void cm4_set_nolp_mode(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;
	u32 idle_vrefresh = ctx->throttled_min_vrefresh;

	dev_dbg(dev, "%s\n", __func__);

	DPU_ATRACE_BEGIN(__func__);

	cm4_wait_for_vsync_done(ctx, pmode);
	/* manual mode 30Hz */
	cm4_enforce_manual_and_peak(ctx);

	cm4_wait_for_vsync_done(ctx, pmode);
	GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);
	GS_DCS_BUF_ADD_CMDLIST(dev, aod_off);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);

	cm4_wait_for_vsync_done(ctx, pmode);
	cm4_set_panel_feat(ctx, pmode, idle_vrefresh, true);
	/* backlight control and dimming */
	cm4_write_display_mode(ctx, &pmode->mode);
	cm4_change_frequency(ctx, pmode);

	DPU_ATRACE_END(__func__);

	dev_info(dev, "exit LP mode\n");
}

static const struct gs_dsi_cmd cm4_init_cmds[] = {
	/* Enable TE*/
	GS_DSI_CMD(MIPI_DCS_SET_TEAR_ON),

	/* CASET: 1279 */
	GS_DSI_CMD(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x04, 0xFF),
	/* PASET: 2855 */
	GS_DSI_CMD(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x0B, 0x27),

	GS_DSI_CMDLIST(unlock_cmd_f0),
	/* TSP sync enable */
	GS_DSI_CMD(0xB0, 0x00, 0x3C, 0xB9), /* Global para */
	GS_DSI_CMD(0xB9, 0x19, 0x09), /* Sync On */

	/* FFC: 165MHz, MIPI Speed 1368 Mbps */
	GS_DSI_CMD(0xB0, 0x00, 0x36, 0xC5),
	GS_DSI_CMD(0xC5, 0x11, 0x10, 0x50, 0x05, 0x4D, 0x31, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00,
		   0x4D, 0x31, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x4D, 0x31, 0x40, 0x00, 0x40,
		   0x00, 0x40, 0x00, 0x4D, 0x31, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00),

	/* enable OPEC (auto still IMG detect off) */
	GS_DSI_CMD(0xB0, 0x00, 0x1D, 0x63),
	GS_DSI_CMD(0x63, 0x02, 0x18),

	/* PMIC Fast Discharge off */
	GS_DSI_CMD(0xB0, 0x00, 0x13, 0xB1),
	GS_DSI_CMD(0xB1, 0x80),
	GS_DSI_CMDLIST(freq_update),
	GS_DSI_CMDLIST(lock_cmd_f0),
};
static DEFINE_GS_CMDSET(cm4_init);

static int cm4_enable(struct drm_panel *panel)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *pmode = ctx->current_mode;
	const struct drm_display_mode *mode;
	const bool needs_reset = !gs_is_panel_enabled(ctx);
	bool is_fhd;

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return -EINVAL;
	}
	mode = &pmode->mode;
	is_fhd = mode->hdisplay == 960;

	dev_info(dev, "%s fhd=%d\n", __func__, is_fhd);

	DPU_ATRACE_BEGIN(__func__);

	if (needs_reset)
		gs_panel_reset_helper(ctx);

	/* wait TE falling for RRS since DSC and framestart must in the same VSYNC */
	if (ctx->mode_in_progress == MODE_RES_IN_PROGRESS ||
	    ctx->mode_in_progress == MODE_RES_AND_RR_IN_PROGRESS)
		cm4_wait_for_vsync_done(ctx, pmode);

	/* 8bit config for QHD/FHD */
	GS_DCS_WRITE_CMD(dev, 0xF2, 0x01, 0x01);
	/* DSC related configuration */
	gs_dcs_write_dsc_config(dev, is_fhd ? &fhd_pps_config : &wqhd_pps_config);
	/* compression enable */
	GS_DCS_WRITE_CMD(dev, 0x9D, 0x01);

	if (needs_reset) {
		struct cm4_panel *spanel = to_spanel(ctx);

		GS_DCS_WRITE_DELAY_CMD(dev, 120, MIPI_DCS_EXIT_SLEEP_MODE);
		gs_panel_send_cmdset(ctx, &cm4_init_cmdset);
		spanel->is_pixel_off = false;
	}

	GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);
	GS_DCS_BUF_ADD_CMD(dev, 0xC3, is_fhd ? 0x0D : 0x0C);
	/* 8/10bit config for QHD/FHD */
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x01, 0xF2);
	GS_DCS_BUF_ADD_CMD(dev, 0xF2, is_fhd ? 0x81 : 0x01);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);

	cm4_update_panel_feat(ctx, true);
	cm4_write_display_mode(ctx, mode); /* dimming and HBM */
	cm4_change_frequency(ctx, pmode);

	if (pmode->gs_mode.is_lp_mode) {
		cm4_set_lp_mode(ctx, pmode);
		GS_DCS_WRITE_CMD(dev, MIPI_DCS_SET_DISPLAY_ON);
	} else if (needs_reset || (ctx->panel_state == GPANEL_STATE_BLANK)) {
		GS_DCS_WRITE_CMD(dev, MIPI_DCS_SET_DISPLAY_ON);
	}

	DPU_ATRACE_END(__func__);

	return 0;
}

static int cm4_disable(struct drm_panel *panel)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	struct device *dev = ctx->dev;
	int ret;

	dev_info(dev, "%s\n", __func__);

	/* skip disable sequence if going through RRS */
	if (ctx->mode_in_progress == MODE_RES_IN_PROGRESS ||
	    ctx->mode_in_progress == MODE_RR_IN_PROGRESS ||
	    ctx->mode_in_progress == MODE_RES_AND_RR_IN_PROGRESS) {
		dev_dbg(dev, "%s: RRS in progress, skip\n", __func__);
		return 0;
	}

	ret = gs_panel_disable(panel);
	if (ret)
		return ret;

	/* panel register state gets reset after disabling hardware */
	bitmap_clear(ctx->hw_status.feat, 0, FEAT_MAX);
	ctx->hw_status.vrefresh = 60;
	ctx->hw_status.te_freq = 60;
	ctx->hw_status.idle_vrefresh = 0;
	ctx->hw_status.acl_mode = 0;
	ctx->hw_status.dbv = 0;
	ctx->hw_status.irc_mode = IRC_FLAT_DEFAULT;

	/* set manual and peak before turning off display */
	cm4_enforce_manual_and_peak(ctx);

	GS_DCS_WRITE_DELAY_CMD(dev, 20, MIPI_DCS_SET_DISPLAY_OFF);

	if (ctx->panel_state == GPANEL_STATE_OFF)
		GS_DCS_WRITE_DELAY_CMD(dev, 100, MIPI_DCS_ENTER_SLEEP_MODE);

	return 0;
}

/*
 * 120hz auto mode takes at least 2 frames to start lowering refresh rate in addition to
 * time to next vblank. Use just over 2 frames time to consider worst case scenario
 */
#define EARLY_EXIT_THRESHOLD_US 17000

/**
 * cm4_update_idle_state - update panel auto frame insertion state
 * @ctx: panel struct
 *
 * - update timestamp of switching to manual mode in case its been a while since the
 *   last frame update and auto mode may have started to lower refresh rate.
 * - trigger early exit by command if it's changeable TE and no switching delay, which
 *   could result in fast 120 Hz boost and seeing 120 Hz TE earlier, otherwise disable
 *   auto refresh mode to avoid lowering frequency too fast.
 */
static void cm4_update_idle_state(struct gs_panel *ctx)
{
	s64 delta_us;
	struct cm4_panel *spanel = to_spanel(ctx);
	struct device *dev = ctx->dev;

	ctx->idle_data.panel_idle_vrefresh = 0;
	if (!test_bit(FEAT_FRAME_AUTO, ctx->sw_status.feat))
		return;

	delta_us = ktime_us_delta(ktime_get(), ctx->timestamps.last_commit_ts);
	if (delta_us < EARLY_EXIT_THRESHOLD_US) {
		dev_dbg(dev, "skip early exit. %lldus since last commit\n", delta_us);
		return;
	}

	/* triggering early exit causes a switch to 120hz */
	ctx->timestamps.last_mode_set_ts = ktime_get();

	DPU_ATRACE_BEGIN(__func__);

	if (!ctx->idle_data.idle_delay_ms && spanel->force_changeable_te) {
		dev_dbg(dev, "sending early exit out cmd\n");
		GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);
		GS_DCS_BUF_ADD_CMDLIST(dev, freq_update);
		GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);
	} else {
		/* turn off auto mode to prevent panel from lowering frequency too fast */
		cm4_update_refresh_mode(ctx, ctx->current_mode, 0);
	}

	DPU_ATRACE_END(__func__);
}

static void cm4_commit_done(struct gs_panel *ctx)
{
	if (ctx->current_mode->gs_mode.is_lp_mode)
		return;

	/* skip idle update if going through RRS */
	if (ctx->mode_in_progress == MODE_RES_IN_PROGRESS ||
	    ctx->mode_in_progress == MODE_RES_AND_RR_IN_PROGRESS) {
		dev_dbg(ctx->dev, "%s: RRS in progress, skip\n", __func__);
		return;
	}

	cm4_update_idle_state(ctx);

	cm4_update_za(ctx);

	if (ctx->thermal && ctx->thermal->pending_temp_update)
		cm4_update_disp_therm(ctx);
}

static void cm4_set_hbm_mode(struct gs_panel *ctx, enum gs_hbm_mode mode)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;

	if (mode == ctx->hbm_mode)
		return;

	if (unlikely(!pmode))
		return;

	ctx->hbm_mode = mode;

	if (GS_IS_HBM_ON(mode)) {
		set_bit(FEAT_HBM, ctx->sw_status.feat);
		/* enforce IRC on for factory builds */
#ifndef PANEL_FACTORY_BUILD
		if (mode == GS_HBM_ON_IRC_ON)
			ctx->sw_status.irc_mode = IRC_FLAT_DEFAULT;
		else
			ctx->sw_status.irc_mode = IRC_FLAT_Z;
#endif
		cm4_update_panel_feat(ctx, false);
		cm4_write_display_mode(ctx, &pmode->mode);
	} else {
		clear_bit(FEAT_HBM, ctx->sw_status.feat);
		ctx->sw_status.irc_mode = IRC_FLAT_DEFAULT;
		cm4_write_display_mode(ctx, &pmode->mode);
		cm4_update_panel_feat(ctx, false);
	}
}

static void cm4_set_dimming(struct gs_panel *ctx, bool dimming_on)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;

	ctx->dimming_on = dimming_on;
	if (pmode->gs_mode.is_lp_mode) {
		dev_info(ctx->dev, "in lp mode, skip to update");
		return;
	}
	cm4_write_display_mode(ctx, &pmode->mode);
}

static void cm4_mode_set(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	cm4_change_frequency(ctx, pmode);
}

static bool cm4_is_mode_seamless(const struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	const struct drm_display_mode *c = &ctx->current_mode->mode;
	const struct drm_display_mode *n = &pmode->mode;

	/* seamless mode set can happen if active region resolution is same */
	return (c->vdisplay == n->vdisplay) && (c->hdisplay == n->hdisplay);
}

//todo: understand this better (I think this is just NS and HS changing)
static int cm4_set_op_hz(struct gs_panel *ctx, unsigned int hz)
{
	u32 vrefresh = drm_mode_vrefresh(&ctx->current_mode->mode);

	if (gs_is_vrr_mode(ctx->current_mode)) {
		dev_warn(ctx->dev, "%s: should be set via mode switch\n", __func__);
		return -EINVAL;
	}

	if (vrefresh > hz || (hz != 60 && hz != 120)) {
		dev_err(ctx->dev, "invalid op_hz=%d for vrefresh=%d\n", hz, vrefresh);
		return -EINVAL;
	}

	DPU_ATRACE_BEGIN(__func__);

	ctx->op_hz = hz;
	if (hz == 60)
		set_bit(FEAT_OP_NS, ctx->sw_status.feat);
	else
		clear_bit(FEAT_OP_NS, ctx->sw_status.feat);

	if (gs_is_panel_active(ctx))
		cm4_update_panel_feat(ctx, false);
	dev_info(ctx->dev, "%s op_hz at %d\n", gs_is_panel_active(ctx) ? "set" : "cache", hz);

	DPU_ATRACE_END(__func__);

	return 0;
}

static int cm4_read_id(struct gs_panel *ctx)
{
	return gs_panel_read_slsi_ddic_id(ctx);
}

static void cm4_get_panel_rev(struct gs_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	u8 build_code = (id & 0xFF00) >> 8;
	u8 rev = ((build_code & 0xE0) >> 3) | ((build_code & 0x0C) >> 2);

	/* b/306527241 - Ensure EVT 1.0 panels use the correct revision */
	if (id == 0x22A4040A)
		rev = 8;

	gs_panel_get_panel_rev(ctx, rev);
}

static void cm4_normal_mode_work(struct gs_panel *ctx)
{
	if (ctx->idle_data.self_refresh_active) {
		cm4_update_disp_therm(ctx);
	} else {
		if (ctx->thermal)
			ctx->thermal->pending_temp_update = true;
	}
}

static const struct gs_display_underrun_param underrun_param = {
	.te_idle_us = 350,
	.te_var = 1,
};

static const u32 cm4_bl_range[] = {
	94, 180, 270, 360, 3271
};

#define CM4_WQHD_DSC {\
	.enabled = true,\
	.dsc_count = 2,\
	.cfg = &wqhd_pps_config,\
}
#define CM4_FHD_DSC {\
	.enabled = true,\
	.dsc_count = 2,\
	.cfg = &fhd_pps_config,\
}

static const struct gs_panel_mode_array cm4_modes = {
#ifdef PANEL_FACTORY_BUILD
	.num_modes = 6,
#else
	.num_modes = 10,
#endif
	.modes = {
		/* MRR modes */
#ifdef PANEL_FACTORY_BUILD
		{
			.mode = {
				.name = "1280x2856@1:1",
				DRM_MODE_TIMING(1, 1280, 80, 24, 46, 2856, 12, 4, 28),
				.flags = 0,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CM4_WQHD_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
				.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "1280x2856@10:10",
				DRM_MODE_TIMING(10, 1280, 80, 24, 46, 2856, 12, 4, 28),
				.flags = 0,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CM4_WQHD_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
				.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "1280x2856@30:30",
				DRM_MODE_TIMING(30, 1280, 80, 24, 46, 2856, 12, 4, 28),
				.flags = 0,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CM4_WQHD_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
				.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "1280x2856@80:80",
				DRM_MODE_TIMING(80, 1280, 80, 24, 46, 2856, 12, 4, 28),
				.flags = 0,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CM4_WQHD_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
				.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
#endif /* PANEL_FACTORY_BUILD */
		{
			.mode = {
				/* 60Hz supports HS/NS, see cm4_get_te_usec for widths used */
				.name = "1280x2856@60:60",
				DRM_MODE_TIMING(60, 1280, 80, 24, 46, 2856, 12, 4, 28),
				.flags = DRM_MODE_FLAG_BTS_OP_RATE,
				.type = DRM_MODE_TYPE_PREFERRED,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CM4_WQHD_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
				.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "1280x2856@120:120",
				DRM_MODE_TIMING(120, 1280, 80, 24, 46, 2856, 12, 4, 28),
				.flags = DRM_MODE_FLAG_BTS_OP_RATE,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = CM4_TE_USEC_120HZ_HS,
				.bpc = 8,
				.dsc = CM4_WQHD_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
				.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
#ifndef PANEL_FACTORY_BUILD
		{
			.mode = {
				/* 60Hz supports HS/NS, see cm4_get_te_usec for widths used */
				.name = "960x2142@60:60",
				DRM_MODE_TIMING(60, 960, 80, 24, 46, 2142, 12, 4, 32),
				.flags = DRM_MODE_FLAG_BTS_OP_RATE,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CM4_FHD_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
				.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "960x2142@120:120",
				DRM_MODE_TIMING(120, 960, 80, 24, 46, 2142, 12, 4, 32),
				.flags = DRM_MODE_FLAG_BTS_OP_RATE,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = CM4_TE_USEC_120HZ_HS,
				.bpc = 8,
				.dsc = CM4_FHD_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
				.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		/* VRR modes */
		{
			.mode = {
				.name = "1280x2856@120:240",
				DRM_MODE_TIMING(120, 1280, 80, 24, 46, 2856, 12, 4, 28),
				.flags = DRM_MODE_FLAG_TE_FREQ_X2,
				/* aligned to bootloader resolution */
				.type = DRM_MODE_TYPE_VRR | DRM_MODE_TYPE_PREFERRED,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = CM4_TE_USEC_VRR_HS,
				.bpc = 8,
				.dsc = CM4_WQHD_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
				.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "960x2142@120:240",
				DRM_MODE_TIMING(120, 960, 80, 24, 46, 2142, 12, 4, 32),
				.flags = DRM_MODE_FLAG_TE_FREQ_X2,
				.type = DRM_MODE_TYPE_VRR,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = CM4_TE_USEC_VRR_HS,
				.bpc = 8,
				.dsc = CM4_FHD_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
				.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "1280x2856@120:120",
				DRM_MODE_TIMING(120, 1280, 80, 24, 46, 2856, 12, 4, 28),
				.flags = DRM_MODE_FLAG_TE_FREQ_X1,
				.type = DRM_MODE_TYPE_VRR,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = CM4_TE_USEC_VRR_HS,
				.bpc = 8,
				.dsc = CM4_WQHD_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
				.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "960x2142@120:120",
				DRM_MODE_TIMING(120, 960, 80, 24, 46, 2142, 12, 4, 32),
				.flags = DRM_MODE_FLAG_TE_FREQ_X1,
				.type = DRM_MODE_TYPE_VRR,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = CM4_TE_USEC_VRR_HS,
				.bpc = 8,
				.dsc = CM4_FHD_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
				.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "1280x2856@60:240",
				DRM_MODE_TIMING(60, 1280, 80, 24, 46, 2856, 12, 4, 28),
				.flags = DRM_MODE_FLAG_TE_FREQ_X4 | DRM_MODE_FLAG_NS,
				.type = DRM_MODE_TYPE_VRR,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = CM4_TE_USEC_VRR_NS,
				.bpc = 8,
				.dsc = CM4_WQHD_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
				.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "960x2142@60:240",
				DRM_MODE_TIMING(60, 960, 80, 24, 46, 2142, 12, 4, 32),
				.flags = DRM_MODE_FLAG_TE_FREQ_X4 | DRM_MODE_FLAG_NS,
				.type = DRM_MODE_TYPE_VRR,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = CM4_TE_USEC_VRR_NS,
				.bpc = 8,
				.dsc = CM4_FHD_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = CM4_TE2_RISING_EDGE_OFFSET,
				.falling_edge = CM4_TE2_FALLING_EDGE_OFFSET,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
#endif /* !PANEL_FACTORY_BUILD */
	},/* .modes */
}; /* cm4_modes */

/* change hsa and hbp to avoid conflicting to normal mode 30Hz */
static const struct gs_panel_mode_array cm4_lp_modes = {
#ifdef PANEL_FACTORY_BUILD
	.num_modes = 1,
#else
	.num_modes = 2,
#endif
	.modes = {
		{
			.mode = {
				.name = "1280x2856@30:30",
				/* hsa and hbp are different from normal 30 Hz */
				DRM_MODE_TIMING(30, 1280, 80, 26, 44, 2856, 12, 4, 28),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 693,
				.bpc = 8,
				.dsc = CM4_WQHD_DSC,
				.underrun_param = &underrun_param,
				.is_lp_mode = true,
			},
		},
#ifndef PANEL_FACTORY_BUILD
		{
			.mode = {
				.name = "960x2142@30:30",
				DRM_MODE_TIMING(30, 960, 80, 26, 44, 2142, 12, 4, 32),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 693,
				.bpc = 8,
				.dsc = CM4_FHD_DSC,
				.underrun_param = &underrun_param,
				.is_lp_mode = true,
			},
		},
#endif
	}, /* .modes */
}; /* cm4_lp_modes */

static void cm4_debugfs_init(struct drm_panel *panel, struct dentry *root)
{
#if IS_ENABLED(CONFIG_DEBUG_FS)
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	struct dentry *panel_root, *csroot;
	struct cm4_panel *spanel;

	if (!ctx)
		return;

	panel_root = debugfs_lookup("panel", root);
	if (!panel_root)
		return;

	csroot = debugfs_lookup("cmdsets", panel_root);
	if (!csroot) {
		goto panel_out;
	}

	spanel = to_spanel(ctx);

	gs_panel_debugfs_create_cmdset(csroot, &cm4_init_cmdset, "init");
	debugfs_create_bool("force_changeable_te", 0644, panel_root, &spanel->force_changeable_te);
	debugfs_create_bool("force_changeable_te2", 0644, panel_root,
			    &spanel->force_changeable_te2);
	debugfs_create_bool("force_za_off", 0644, panel_root, &spanel->force_za_off);
	debugfs_create_u32("hw_acl_setting", 0644, panel_root, &ctx->hw_status.acl_mode);
	dput(csroot);
panel_out:
	dput(panel_root);
#endif
}

static void cm4_panel_init(struct gs_panel *ctx)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;

#ifdef PANEL_FACTORY_BUILD
	ctx->idle_data.panel_idle_enabled = false;
	set_bit(FEAT_FI_AUTO, ctx->sw_status.feat);
#else
	clear_bit(FEAT_FI_AUTO, ctx->sw_status.feat);
	clear_bit(FEAT_FRAME_AUTO, ctx->sw_status.feat);
#endif

	if (!ctx->thermal) {
		dev_err(ctx->dev, "%s: error retrieving thermal data\n", __func__);
		return;
	}

	ctx->thermal->tz = thermal_zone_get_zone_by_name("disp_therm");
	if (IS_ERR_OR_NULL(ctx->thermal->tz))
		dev_err(ctx->dev, "%s: failed to get thermal zone disp_therm\n", __func__);
	/* re-init panel to decouple bootloader settings */
	if (pmode) {
		dev_info(ctx->dev, "%s: set mode: %s\n", __func__, pmode->mode.name);
		cm4_set_panel_feat(ctx, pmode, 0, true);
	}
}

static int cm4_panel_probe(struct mipi_dsi_device *dsi)
{
	struct cm4_panel *spanel;
	struct gs_panel *ctx;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;
	ctx = &spanel->base;
	ctx->thermal = devm_kzalloc(&dsi->dev, sizeof(*ctx->thermal), GFP_KERNEL);
	if (!ctx->thermal) {
		devm_kfree(&dsi->dev, spanel);
		return -ENOMEM;
	}

	spanel->base.op_hz = 120;
	ctx->hw_status.vrefresh = 60;
	ctx->hw_status.acl_mode = ACL_OFF;
	clear_bit(FEAT_ZA, ctx->hw_status.feat);
	ctx->hw_status.dbv = 0;
	/* ddic default temp */
	ctx->thermal->hw_temp = 25;
	ctx->thermal->pending_temp_update = false;
	spanel->is_pixel_off = false;

	return gs_dsi_panel_common_init(dsi, &spanel->base);
}

static int cm4_panel_config(struct gs_panel *ctx);

static const struct drm_panel_funcs cm4_drm_funcs = {
	.disable = cm4_disable,
	.unprepare = gs_panel_unprepare,
	.prepare = gs_panel_prepare,
	.enable = cm4_enable,
	.get_modes = gs_panel_get_modes,
	.debugfs_init = cm4_debugfs_init,
};

static const struct gs_panel_funcs cm4_gs_funcs = {
	.set_brightness = cm4_set_brightness,
	.set_lp_mode = cm4_set_lp_mode,
	.set_nolp_mode = cm4_set_nolp_mode,
	.set_binned_lp = gs_panel_set_binned_lp_helper,
	.set_hbm_mode = cm4_set_hbm_mode,
	.set_dimming = cm4_set_dimming,
	.is_mode_seamless = cm4_is_mode_seamless,
	.mode_set = cm4_mode_set,
	.panel_init = cm4_panel_init,
	.panel_config = cm4_panel_config,
	.get_panel_rev = cm4_get_panel_rev,
	.get_te2_edges = gs_panel_get_te2_edges_helper,
	.set_te2_edges = gs_panel_set_te2_edges_helper,
	.update_te2 = cm4_update_te2,
	.commit_done = cm4_commit_done,
	.atomic_check = cm4_atomic_check,
	.refresh_ctrl = cm4_refresh_ctrl,
	.set_self_refresh = cm4_set_self_refresh,
	.set_op_hz = cm4_set_op_hz,
	.read_id = cm4_read_id,
	.get_te_usec = cm4_get_te_usec,
	.set_acl_mode = cm4_set_acl_mode,
	.run_normal_mode_work = cm4_normal_mode_work,
};

static const struct gs_brightness_configuration cm4_btr_configs[] = {
	{
		.panel_rev = PANEL_REV_EVT1 | PANEL_REV_LATEST,
		.default_brightness = 1209, /* dbv_for_140_nits */
		.brt_capability = {
			.normal = {
				.nits = {
					.min = 2,
					.max = 1250,
				},
				.level = {
					.min = 176,
					.max = 3271,
				},
				.percentage = {
					.min = 0,
					.max = 61,
				},
			},
			.hbm = {
				.nits = {
					.min = 1250,
					.max = 2050,
				},
				.level = {
					.min = 3272,
					.max = 4095,
				},
				.percentage = {
					.min = 61,
					.max = 100,
				},
			},
		},
	},
	{
		.panel_rev = PANEL_REV_PROTO1_1,
		.default_brightness = 1209, /* dbv_for_140_nits */
		.brt_capability = {
			.normal = {
				.nits = {
					.min = 2,
					.max = 1250,
				},
				.level = {
					.min = 184,
					.max = 3427,
				},
				.percentage = {
					.min = 0,
					.max = 68,
				},
			},
			.hbm = {
				.nits = {
					.min = 1250,
					.max = 1850,
				},
				.level = {
					.min = 3428,
					.max = 4095,
				},
				.percentage = {
					.min = 68,
					.max = 100,
				},
			},
		},
	},
	{
		.panel_rev = PANEL_REV_PROTO1,
		.default_brightness = 1209, /* dbv_for_140_nits */
		.brt_capability = {
			.normal = {
				.nits = {
					.min = 2,
					.max = 1200,
				},
				.level = {
					.min = 186,
					.max = 3406,
				},
				.percentage = {
					.min = 0,
					.max = 67,
				},
			},
			.hbm = {
				.nits = {
					.min = 1200,
					.max = 1800,
				},
				.level = {
					.min = 3407,
					.max = 4095,
				},
				.percentage = {
					.min = 67,
					.max = 100,
				},
			},
		},
	},
};

static struct gs_panel_brightness_desc cm4_brightness_desc = {
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
};

static struct gs_panel_reg_ctrl_desc cm4_reg_ctrl_desc = {
	.reg_ctrl_enable = {
		{PANEL_REG_ID_VDDI, 1},
		{PANEL_REG_ID_VCI, 10},
	},
	.reg_ctrl_post_enable = {
		{PANEL_REG_ID_VDDD, 1},
	},
	.reg_ctrl_pre_disable = {
		{PANEL_REG_ID_VDDD, 1},
	},
	.reg_ctrl_disable = {
		{PANEL_REG_ID_VCI, 1},
		{PANEL_REG_ID_VDDI, 1},
	},
};

static struct gs_panel_desc google_cm4 = {
	.data_lane_cnt = 4,
	.dbv_extra_frame = true,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.brightness_desc = &cm4_brightness_desc,
	.bl_range = cm4_bl_range,
	.bl_num_ranges = ARRAY_SIZE(cm4_bl_range),
	.modes = &cm4_modes,
	.lp_modes = &cm4_lp_modes,
	.binned_lp = cm4_binned_lp,
	.num_binned_lp = ARRAY_SIZE(cm4_binned_lp),
	.has_off_binned_lp_entry = false,
	.is_idle_supported = true,
	.panel_func = &cm4_drm_funcs,
	.gs_panel_func = &cm4_gs_funcs,
	.reset_timing_ms = { 1, 1, 5 },
	.reg_ctrl_desc = &cm4_reg_ctrl_desc,
};

static int cm4_panel_config(struct gs_panel *ctx)
{
	gs_panel_model_init(ctx, PROJECT, 0);

	return gs_panel_update_brightness_desc(&cm4_brightness_desc, cm4_btr_configs,
					       ARRAY_SIZE(cm4_btr_configs), ctx->panel_rev);
}

static const struct of_device_id gs_panel_of_match[] = {
	{ .compatible = "google,gs-cm4", .data = &google_cm4 },
	{ }
};
MODULE_DEVICE_TABLE(of, gs_panel_of_match);

static struct mipi_dsi_driver gs_panel_driver = {
	.probe = cm4_panel_probe,
	.remove = gs_dsi_panel_common_remove,
	.driver = {
		.name = "panel-gs-cm4",
		.of_match_table = gs_panel_of_match,
	},
};
module_mipi_dsi_driver(gs_panel_driver);

MODULE_AUTHOR("Taylor Nelms <tknelms@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google CM4 panel driver");
MODULE_LICENSE("Dual MIT/GPL");

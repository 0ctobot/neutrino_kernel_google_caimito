/* SPDX-License-Identifier: MIT */
/*
 * MIPI-DSI based tg4a panel driver.
 *
 * Copyright (c) 2024 Google LLC
 */

#include <drm/display/drm_dsc_helper.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <video/mipi_display.h>

#include "trace/panel_trace.h"

#include "gs_panel/drm_panel_funcs_defaults.h"
#include "gs_panel/gs_panel.h"
#include "gs_panel/gs_panel_funcs_defaults.h"

/**
 * enum tg4a_lhbm_brt - local hbm brightness
 * @LHBM_R_COARSE: red coarse
 * @LHBM_GB_COARSE: green and blue coarse
 * @LHBM_R_FINE: red fine
 * @LHBM_G_FINE: green fine
 * @LHBM_B_FINE: blue fine
 * @LHBM_BRT_LEN: local hbm brightness array length
 */
enum tg4a_lhbm_brt {
	LHBM_R_COARSE,
	LHBM_GB_COARSE,
	LHBM_R_FINE,
	LHBM_G_FINE,
	LHBM_B_FINE,
	LHBM_BRT_LEN
};

#define LHBM_BRT_CMD_LEN (LHBM_BRT_LEN + 1)
#define LHBM_BRIGHTNESS_INDEX_SIZE 4
#define LHBM_GAMMA_CMD_SIZE 6
#define LHBM_RATIO_SIZE 3
#define LHBM_RGB_RATIO_SIZE 3

/* DSC1.2 */
static const struct drm_dsc_config pps_config = {
	.line_buf_depth = 9,
	.bits_per_component = 8,
	.convert_rgb = true,
	.slice_width = 540,
	.slice_height = 101,
	.slice_count = 2,
	.simple_422 = false,
	.pic_width = 1080,
	.pic_height = 2424,
	.rc_tgt_offset_high = 3,
	.rc_tgt_offset_low = 3,
	.bits_per_pixel = 128,
	.rc_edge_factor = 6,
	.rc_quant_incr_limit1 = 11,
	.rc_quant_incr_limit0 = 11,
	.initial_xmit_delay = 512,
	.initial_dec_delay = 526,
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
	.scale_decrement_interval = 7,
	.scale_increment_interval = 2517,
	.nfl_bpg_offset = 246,
	.slice_bpg_offset = 258,
	.final_offset = 4336,
	.vbr_enable = false,
	.slice_chunk_size = 540,
	.dsc_version_minor = 2,
	.dsc_version_major = 1,
	.native_422 = false,
	.native_420 = false,
	.second_line_bpg_offset = 0,
	.nsl_bpg_offset = 0,
	.second_line_offset_adj = 0,
};


#define TG4A_WRCTRLD_DIMMING_BIT	0x08
#define TG4A_WRCTRLD_BCTRL_BIT		0x20
#define TG4A_WRCTRLD_GLOCAL_HBM_BIT	0x10

#define FREQUENCY_COUNT 2

#define MIPI_DSI_FREQ_DEFAULT 1102
#define MIPI_DSI_FREQ_ALTERNATIVE 1000

static const u8 test_key_enable[] = { 0xF0, 0x5A, 0x5A };
static const u8 test_key_disable[] = { 0xF0, 0xA5, 0xA5 };
static const u8 test_key_fc_enable[] = { 0xFC, 0x5A, 0x5A };
static const u8 test_key_fc_disable[] = { 0xFC, 0xA5, 0xA5 };
static const u8 pixel_off[] = { 0x22 };

static const u8 lhbm_brightness_write_index[FREQUENCY_COUNT][LHBM_BRIGHTNESS_INDEX_SIZE] = {
	{ 0xB0, 0x00, 0xAE, 0x6A }, /* HS120 */
	{ 0xB0, 0x00, 0xB3, 0x6A }, /* HS60 */
};

static const u8 lhbm_brightness_read_index[FREQUENCY_COUNT][LHBM_BRIGHTNESS_INDEX_SIZE] = {
	{ 0xB0, 0x02, 0x06, 0xBD }, /* HS120 */
	{ 0xB0, 0x02, 0x0B, 0xBD }, /* HS60 */
};

static const u8 lhbm_brightness_write_reg = 0x6A;
static const u8 lhbm_brightness_read_reg = 0xBD;

/**
 * enum tg4a_lhbm_brt_overdrive_group - lhbm brightness overdrive group number
 * @LHBM_OVERDRIVE_GRP_0_NIT: group number for 0 nit
 * @LHBM_OVERDRIVE_GRP_6_NIT: group number for 0-6 nits
 * @LHBM_OVERDRIVE_GRP_50_NIT: group number for 6-50 nits
 * @LHBM_OVERDRIVE_GRP_300_NIT: group number for 50-300 nits
 * @LHBM_OVERDRIVE_GRP_MAX: maximum group number
 */
enum tg4a_lhbm_brt_overdrive_group {
	LHBM_OVERDRIVE_GRP_0_NIT,
	LHBM_OVERDRIVE_GRP_6_NIT,
	LHBM_OVERDRIVE_GRP_50_NIT,
	LHBM_OVERDRIVE_GRP_300_NIT,
	LHBM_OVERDRIVE_GRP_MAX
};

/* actual ratio = value / (10^9) */
u32 lhbm_rgb_ratio[LHBM_OVERDRIVE_GRP_MAX][LHBM_RATIO_SIZE] = {
	{1068287156, 1062530559, 1068418009},
	{1032101895, 1029395710, 1032163409},
	{1021689393, 1019860980, 1021730955},
	{1018697351, 1017121167, 1018733179},
};

static const struct gs_dsi_cmd tg4a_off_cmds[] = {
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_OFF),
	GS_DSI_DELAY_CMD(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_GS_CMDSET(tg4a_off);

static const struct gs_dsi_cmd tg4a_lp_cmds[] = {
	/* AOD Power Setting */
	GS_DSI_CMDLIST(test_key_enable),
	GS_DSI_CMD(0xB0, 0x00, 0x04, 0xF6),
	GS_DSI_CMD(0xF6, 0x25), /* Default */
	GS_DSI_CMDLIST(test_key_disable),

	/* AOD Mode On Setting */
	GS_DSI_CMD(MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24),
};
static DEFINE_GS_CMDSET(tg4a_lp);

static const struct gs_dsi_cmd tg4a_lp_night_cmd[] = {
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00, 0xB8),
};

static const struct gs_dsi_cmd tg4a_lp_low_cmds[] = {
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x01, 0x7E),
};

static const struct gs_dsi_cmd tg4a_lp_high_cmds[] = {
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x03, 0x1A),
};

static const struct gs_binned_lp tg4a_binned_lp[] = {
	/* night threshold 4 nits */
	BINNED_LP_MODE_TIMING("night", 252, tg4a_lp_night_cmd,
				12, 12 + 50),
	/* low threshold 40 nits */
	BINNED_LP_MODE_TIMING("low", 717, tg4a_lp_low_cmds,
				12, 12 + 50),
	BINNED_LP_MODE_TIMING("high", 4095, tg4a_lp_high_cmds,
				12, 12 + 50),
};

static const struct gs_dsi_cmd tg4a_init_cmds[] = {
	/* TE on */
	GS_DSI_CMD(MIPI_DCS_SET_TEAR_ON),

	/* TE2 setting, only for Proto 1.1 */
	GS_DSI_CMDLIST(test_key_enable),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x26, 0xB9),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB9, 0x00, 0x00, 0x10, 0x00, 0x00,
				0x3D, 0x00, 0x09, 0x90, 0x00, 0x09, 0x90),

	/* CASET: 1080 */
	GS_DSI_CMD(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x04, 0x37),

	/* PASET: 2424 */
	GS_DSI_CMD(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x09, 0x77),

	/* FFC Off (1102Mbps) Set */
	GS_DSI_CMDLIST(test_key_fc_enable),
	GS_DSI_CMD(0xB0, 0x00, 0x3C, 0xC5),
	GS_DSI_CMD(0xC5, 0x45, 0xDE),
	GS_DSI_CMD(0xB0, 0x00, 0x36, 0xC5),
	GS_DSI_CMD(0xC5, 0x10),

	/* VDDD LDO Setting, only for Proto 1.1 */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x58, 0xD7),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xD7, 0x0A),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x5B, 0xD7),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xD7, 0x0A),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xFE, 0x80),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xFE, 0x00),
	GS_DSI_CMDLIST(test_key_fc_disable),

	/* TSP HSYNC setting, only for Proto 1.1 */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x42, 0xB9),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB9, 0x19),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x46, 0xB9),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB9, 0xB0),

	/* FGZ common setting, only for Proto 1.1 */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x30, 0x68),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0x68, 0x32, 0xFF, 0x04, 0x08, 0x10,
				0x15, 0x29, 0x67, 0xA5),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x1C, 0x62),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0x62, 0x1D, 0x5F),

	/* AVC Class AB setting Code, only for Proto 1.1 */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x02, 0x94, 0xF4),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xF4, 0x47),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xF7, 0x2F),

	/* Set back correct OSC setting, only for Proto 1.1 */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x0C, 0xB5),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB5, 0xC0, 0x00, 0x60, 0x00, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xF7, 0x2F),
	GS_DSI_CMDLIST(test_key_disable),
};
static DEFINE_GS_CMDSET(tg4a_init);

static const struct gs_dsi_cmd tg4a_lhbm_on_cmds[] = {
	/* Local HBM Mode AID Setting, only for Proto 1.1 */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x01, 0xF2, 0x69),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0x69, 0x10),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x01, 0xFF, 0x69),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0x69, 0x00, 0x4B, 0x00, 0x4B),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x02, 0x09, 0x69),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0x69, 0x01, 0x01),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x02, 0x0E, 0x69),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0x69, 0x00, 0x40, 0x00, 0x40),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xF7, 0x2F),

	/* global para */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x01, 0xBD),
	/* EM CYC Setting */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xBD, 0x81),
	/* global para */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x2E, 0xBD),
	/* EM CYC Setting */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xBD, 0x00, 0x02),
	/* update key */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xF7, 0x2F),
};
static DEFINE_GS_CMDSET(tg4a_lhbm_on);

static const struct gs_dsi_cmd tg4a_lhbm_location_cmds[] = {
	/* global para */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0xBC, 0x65),
	/* box location */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0x65, 0x00, 0x00, 0x00, 0x43, 0x79, 0x77),
	/* global para */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0xC2, 0x65),
	/* center position set, x: 0x21C, y: 0x6DD, size: 0x64 */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0x65, 0x21, 0xC6, 0xDD,
					0x64, 0x00, 0x00, 0x00, 0x00),
};
static DEFINE_GS_CMDSET(tg4a_lhbm_location);

#define LHBM_GAMMA_CMD_SIZE 6

struct tg4a_lhbm_ctl {
	/** @brt_normal: normal LHBM brightness parameters */
	u8 brt_normal[FREQUENCY_COUNT][LHBM_BRT_LEN];
	/** @brt_overdrive: overdrive LHBM brightness parameters */
	u8 brt_overdrive[FREQUENCY_COUNT][LHBM_OVERDRIVE_GRP_MAX][LHBM_BRT_LEN];
	/** @overdrived: whether LHBM is overdrived */
	bool overdrived;
	/** @hist_roi_configured: whether LHBM histogram configuration is done */
	bool hist_roi_configured;
};

/**
 * struct tg4a_panel - panel specific runtime info
 *
 * This struct maintains tg4a panel specific runtime info, any fixed details about panel
 * should most likely go into struct gs_panel_desc
 */
struct tg4a_panel {
	/** @base: base panel struct */
	struct gs_panel base;

	/** @local_hbm_gamma: lhbm gamma data */
	struct local_hbm_gamma {
		u8 hs120_cmd[LHBM_GAMMA_CMD_SIZE];
		u8 hs60_cmd[LHBM_GAMMA_CMD_SIZE];
	} local_hbm_gamma;

	/**
	 * @is_pixel_off: pixel-off command is sent to panel. Only sending normal-on or resetting
	 *		  panel can recover to normal mode after entering pixel-off state.
	 */
	bool is_pixel_off;

	/** @lhbm_ctl: lhbm brightness control */
	struct tg4a_lhbm_ctl lhbm_ctl;
};
#define to_spanel(ctx) container_of(ctx, struct tg4a_panel, base)

enum frequency { HS120, HS60 };
static const char *frequency_str[] = { "HS120", "HS60" };

static void read_lhbm_gamma(struct gs_panel *ctx, u8 *cmd, enum frequency freq)
{
	struct device *dev = ctx->dev;
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	int ret;

	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lhbm_brightness_read_index[freq]); /* global para */
	ret = mipi_dsi_dcs_read(dsi, 0xBD, cmd + 1, LHBM_GAMMA_CMD_SIZE - 1);

	if (ret != (LHBM_GAMMA_CMD_SIZE - 1)) {
		dev_err(dev, "fail to read LHBM gamma for %s\n", frequency_str[freq]);
		return;
	}

	/* fill in gamma write command 0x6A in offset 0 */
	cmd[0] = lhbm_brightness_write_reg;
	dev_dbg(dev, "%s_gamma: %*ph\n", frequency_str[freq],
		LHBM_GAMMA_CMD_SIZE - 1, cmd + 1);
}

static void tg4a_lhbm_gamma_read(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	struct tg4a_panel *spanel = to_spanel(ctx);

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);

	read_lhbm_gamma(ctx, spanel->local_hbm_gamma.hs120_cmd, HS120);
	read_lhbm_gamma(ctx, spanel->local_hbm_gamma.hs60_cmd, HS60);

	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);
}

static void tg4a_lhbm_gamma_write(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	struct tg4a_panel *spanel = to_spanel(ctx);
	const u8 hs120_cmd = spanel->local_hbm_gamma.hs120_cmd[0];
	const u8 hs60_cmd = spanel->local_hbm_gamma.hs60_cmd[0];

	if (!hs120_cmd && !hs60_cmd) {
		dev_err(dev, "%s: no lhbm gamma!\n", __func__);
		return;
	}

	dev_dbg(dev, "%s\n", __func__);
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);

	GS_DCS_BUF_ADD_CMDLIST(dev, lhbm_brightness_write_index[HS120]); /* global para */
	GS_DCS_BUF_ADD_CMDLIST(dev, spanel->local_hbm_gamma.hs120_cmd); /* write gamma */
	GS_DCS_BUF_ADD_CMDLIST(dev, lhbm_brightness_write_index[HS60]); /* global para */
	GS_DCS_BUF_ADD_CMDLIST(dev, spanel->local_hbm_gamma.hs60_cmd); /* write gamma */

	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);
}

static void tg4a_change_frequency(struct gs_panel *ctx,
					const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;
	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (!ctx || (vrefresh != 60 && vrefresh != 120))
		return;

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0x83, (vrefresh == 120) ? 0x00 : 0x08);
	GS_DCS_BUF_ADD_CMD(dev, 0xF7, 0x2F);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	dev_dbg(dev, "%s: change to %uHz\n", __func__, vrefresh);
	return;
}

static void tg4a_update_lhbm_hist_config(struct gs_panel *ctx, struct drm_atomic_state *state)
{
	struct tg4a_panel *spanel = to_spanel(ctx);
	struct tg4a_lhbm_ctl *ctl = &spanel->lhbm_ctl;
	const struct gs_panel_mode *pmode = ctx->current_mode;
	const struct drm_display_mode *mode;

	const int d = 540, r = 100;

	if (ctl->hist_roi_configured)
		return;

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return;
	}

	mode = &pmode->mode;
	gs_panel_update_lhbm_hist_data_helper(ctx, state, true, GS_HIST_ROI_CIRCLE, d, r);
	ctl->hist_roi_configured = true;
}

static int tg4a_atomic_check(struct gs_panel *ctx, struct drm_atomic_state *state)
{
	struct drm_connector *conn = &ctx->gs_connector->base;
	struct drm_connector_state *new_conn_state =
		drm_atomic_get_new_connector_state(state, conn);
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;

	tg4a_update_lhbm_hist_config(ctx, state);

	if (!ctx->current_mode || drm_mode_vrefresh(&ctx->current_mode->mode) == 120 ||
	    !new_conn_state || !new_conn_state->crtc)
		return 0;

	new_crtc_state = drm_atomic_get_new_crtc_state(state, new_conn_state->crtc);
	old_crtc_state = drm_atomic_get_old_crtc_state(state, new_conn_state->crtc);
	if (!old_crtc_state || !new_crtc_state || !new_crtc_state->active)
		return 0;

	if (!drm_atomic_crtc_effectively_active(old_crtc_state) ||
		(ctx->current_mode->gs_mode.is_lp_mode &&
		drm_mode_vrefresh(&new_crtc_state->mode) == 60)) {
		struct drm_display_mode *mode = &new_crtc_state->adjusted_mode;
		mode->clock = mode->htotal * mode->vtotal * 120 / 1000;

		if (mode->clock != new_crtc_state->mode.clock) {
			new_crtc_state->mode_changed = true;
			ctx->gs_connector->needs_commit = true;
			dev_dbg(ctx->dev, "raise mode (%s) clock to 120hz on %s\n", mode->name,
				!drm_atomic_crtc_effectively_active(old_crtc_state) ?
				"resume" : "lp exit");
		}
	} else if (old_crtc_state->adjusted_mode.clock != old_crtc_state->mode.clock) {
		new_crtc_state->mode_changed = true;
		new_crtc_state->adjusted_mode.clock = new_crtc_state->mode.clock;
		ctx->gs_connector->needs_commit = false;
		dev_dbg(ctx->dev, "restore mode (%s) clock after resume or lp exit\n",
			new_crtc_state->mode.name);
	}

	return 0;
}

static void tg4a_update_wrctrld(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	u8 val = TG4A_WRCTRLD_BCTRL_BIT;

	if (ctx->dimming_on)
		val |= TG4A_WRCTRLD_DIMMING_BIT;

	dev_dbg(dev, "wrctrld: %#x, hbm: %d, dimming: %d\n", val,
		GS_IS_HBM_ON(ctx->hbm_mode), ctx->dimming_on);

	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY, val);
}

static int tg4a_set_brightness(struct gs_panel *ctx, u16 br)
{
	u16 brightness;
	u32 max_brightness;
	struct tg4a_panel *spanel = to_spanel(ctx);
	struct device *dev = ctx->dev;

	if (ctx->current_mode && ctx->current_mode->gs_mode.is_lp_mode) {
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
			dev_dbg(dev, "pixel off instead of dbv 0\n");
		}
		return 0;
	} else if (br && spanel->is_pixel_off) {
		GS_DCS_WRITE_CMD(dev, MIPI_DCS_ENTER_NORMAL_MODE);
		spanel->is_pixel_off = false;
	}

	if (!ctx->desc->brightness_desc->brt_capability) {
		dev_err(dev, "no available brightness capability\n");
		return -EINVAL;
	}

	max_brightness = ctx->desc->brightness_desc->brt_capability->hbm.level.max;


	if (br > max_brightness) {
		br = max_brightness;
		dev_warn(dev, "capped to dbv(%d)\n", max_brightness);
	}

	brightness = __builtin_bswap16(br);

	return gs_dcs_set_brightness(ctx, brightness);
}

static void tg4a_set_hbm_mode(struct gs_panel *ctx,
				enum gs_hbm_mode mode)
{
	struct device *dev = ctx->dev;

	ctx->hbm_mode = mode;

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);

	/* FGZ mode setting */
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x61, 0x68);

	if (GS_IS_HBM_ON(ctx->hbm_mode)) {

		if (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode)) {
			/* FGZ Mode ON */
			if (ctx->panel_rev == PANEL_REV_PROTO1_1)
				GS_DCS_BUF_ADD_CMD(dev, 0x68, 0xB0, 0x2C, 0x6A,
							0x80, 0x00, 0x00, 0xF5, 0xC4);
			else if (ctx->panel_rev == PANEL_REV_EVT1)
				GS_DCS_BUF_ADD_CMD(dev, 0x68, 0xB0, 0x2C, 0x6A,
							0x80, 0x00, 0x00, 0xE4, 0xB6);
			else
				GS_DCS_BUF_ADD_CMD(dev, 0x68, 0xB4, 0x2C, 0x6A,
							0x80, 0x00, 0x00, 0x00, 0xCD);
		} else {
			/* FGZ Mode OFF */
			GS_DCS_BUF_ADD_CMD(dev, 0x68, 0xB0, 0x2C, 0x6A,
						0x80, 0x00, 0x00, 0x00, 0x00);
		}
	} else {
		/* FGZ Mode OFF */
		GS_DCS_BUF_ADD_CMD(dev, 0x68, 0xB0, 0x2C, 0x6A, 0x80,
						0x00, 0x00, 0x00, 0x00);
	}

	if (ctx->panel_rev == PANEL_REV_PROTO1_1) {
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x01, 0xBD);
		GS_DCS_BUF_ADD_CMD(dev, 0xBD, ctx->hbm_mode ? 0x80 : 0x81);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x2E, 0xBD);
		GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, ctx->hbm_mode ? 0x01 : 0x02);
		GS_DCS_BUF_ADD_CMD(dev, 0xF7, 0x2F);
	}

	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	dev_dbg(dev, "hbm_on=%d hbm_ircoff=%d.\n", GS_IS_HBM_ON(ctx->hbm_mode),
		 GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void tg4a_set_local_hbm_brightness(struct gs_panel *ctx, bool is_first_stage)
{
	struct device *dev = ctx->dev;
	struct tg4a_panel *spanel = to_spanel(ctx);
	struct tg4a_lhbm_ctl *ctl = &spanel->lhbm_ctl;
	const u8 *brt;
	enum tg4a_lhbm_brt_overdrive_group group = LHBM_OVERDRIVE_GRP_MAX;
	u32 vrefresh = drm_mode_vrefresh(&ctx->current_mode->mode);
	enum frequency freq = (vrefresh == 120) ? HS120 : HS60;
	/* command uses one byte besides brightness */
	static u8 cmd[LHBM_BRT_LEN + 1];
	int i;

	if (!gs_is_local_hbm_post_enabling_supported(ctx))
		return;

	dev_info(dev, "set LHBM brightness at %s stage\n", is_first_stage ? "1st" : "2nd");
	if (is_first_stage) {
		u32 gray = ctx->gs_connector->lhbm_gray_level;
		u32 dbv = gs_panel_get_brightness(ctx);
		u32 normal_dbv_max = ctx->desc->brightness_desc->brt_capability->normal.level.max;
		u32 normal_nit_max = ctx->desc->brightness_desc->brt_capability->normal.nits.max;
		u32 luma = 0;

		dev_info(dev, "%s: gray level = %d\n", __func__, gray);

		if (gray < 15) {
			group = LHBM_OVERDRIVE_GRP_0_NIT;
		} else {
			if (dbv <= normal_dbv_max)
				luma = panel_calc_gamma_2_2_luminance(dbv, normal_dbv_max,
				normal_nit_max);
			else
				luma = panel_calc_linear_luminance(dbv, 645, -1256);
			luma = panel_calc_gamma_2_2_luminance(gray, 255, luma);

			if (luma < 6)
				group = LHBM_OVERDRIVE_GRP_6_NIT;
			else if (luma < 50)
				group = LHBM_OVERDRIVE_GRP_50_NIT;
			else if (luma < 300)
				group = LHBM_OVERDRIVE_GRP_300_NIT;
			else
				group = LHBM_OVERDRIVE_GRP_MAX;
		}
		dev_dbg(dev, "check LHBM overdrive condition | gray=%u dbv=%u luma=%u\n",
			gray, dbv, luma);
	}

	if (group < LHBM_OVERDRIVE_GRP_MAX) {
		brt = ctl->brt_overdrive[freq][group];
		ctl->overdrived = true;
	} else {
		brt = ctl->brt_normal[freq];
		ctl->overdrived = false;
	}
	cmd[0] = lhbm_brightness_write_reg;
	for (i = 0; i < LHBM_BRT_LEN; i++)
		cmd[i+1] = brt[i];
	dev_dbg(dev, "set %s brightness: [%d] %*ph\n",
		ctl->overdrived ? "overdrive" : "normal",
		ctl->overdrived ? group : -1, LHBM_BRT_LEN, brt);
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMDLIST(dev, lhbm_brightness_write_index[freq]);
	GS_DCS_BUF_ADD_CMDLIST(dev, cmd);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);
}

static void tg4a_set_local_hbm_mode_post(struct gs_panel *ctx)
{
	const struct tg4a_panel *spanel = to_spanel(ctx);

	if (spanel->lhbm_ctl.overdrived)
		tg4a_set_local_hbm_brightness(ctx, false);
}

static void tg4a_set_dimming(struct gs_panel *ctx, bool dimming_on)
{
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *pmode = ctx->current_mode;

	ctx->dimming_on = dimming_on;

	if (pmode->gs_mode.is_lp_mode) {
		dev_warn(dev, "in lp mode, skip to update dimming usage\n");
		return;
	}

	tg4a_update_wrctrld(ctx);
}

static void tg4a_set_local_hbm_mode(struct gs_panel *ctx, bool local_hbm_en)
{
	struct device *dev = ctx->dev;

	tg4a_update_wrctrld(ctx);

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	if (local_hbm_en) {
		GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x30);
		gs_panel_send_cmdset(ctx, &tg4a_lhbm_location_cmdset);
		gs_panel_send_cmdset(ctx, &tg4a_lhbm_on_cmdset);
	} else {
		GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);
		if (ctx->panel_rev == PANEL_REV_PROTO1_1) {
			GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x01, 0xBD);
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, (GS_IS_HBM_ON(ctx->hbm_mode)) ? 0x80 : 0x81);
			GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x2E, 0xBD);
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00,
						(GS_IS_HBM_ON(ctx->hbm_mode)) ? 0x01 : 0x02);
			GS_DCS_BUF_ADD_CMD(dev, 0xF7, 0x2F);
		}
	}
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	if (local_hbm_en)
		tg4a_set_local_hbm_brightness(ctx, true);
}

static void tg4a_mode_set(struct gs_panel *ctx,
				const struct gs_panel_mode *pmode)
{
	tg4a_change_frequency(ctx, pmode);
}

static bool tg4a_is_mode_seamless(const struct gs_panel *ctx,
					const struct gs_panel_mode *pmode)
{
	/* seamless mode switch is possible if only changing refresh rate */
	return drm_mode_equal_no_clocks(&ctx->current_mode->mode, &pmode->mode);
}

static void tg4a_calculate_lhbm_brightness(struct gs_panel *ctx,
	const u8 *p_norm, const u32 *rgb_ratio, u8 *out_norm)
{
	const u8 rgb_offset[3][2] = {
		{LHBM_R_COARSE, LHBM_R_FINE},
		{LHBM_GB_COARSE, LHBM_G_FINE},
		{LHBM_GB_COARSE, LHBM_B_FINE}
	};
	u8 new_norm[LHBM_BRT_LEN] = {0};
	u64 tmp;
	int i;
	u16 mask, shift;

	if (!rgb_ratio)
		return;

	for (i = 0; i < LHBM_RGB_RATIO_SIZE ; i++) {
		if (i % 2) {
			mask = 0xf0;
			shift = 4;
		} else {
			mask = 0x0f;
			shift = 0;
		}
		tmp = ((p_norm[rgb_offset[i][0]] & mask) >> shift) << 8
			| p_norm[rgb_offset[i][1]];
		dev_dbg(ctx->dev, "%s: lhbm_gamma[%d] = %llu\n", __func__, i, tmp);
		/* Round off and revert to original gamma value */
		tmp = (tmp * rgb_ratio[i] + 500000000)/1000000000;
		dev_dbg(ctx->dev, "%s: new lhbm_gamma[%d] = %llu\n", __func__, i, tmp);
		new_norm[rgb_offset[i][0]] |= ((tmp & 0xff00) >> 8) << shift;
		new_norm[rgb_offset[i][1]] |= tmp & 0xff;
	}
	memcpy(out_norm, new_norm, LHBM_BRT_LEN);
	dev_info(ctx->dev, "p_norm(%*ph), new_norm(%*ph), rgb_ratio(%u %u %u)\n",
		LHBM_BRT_LEN, p_norm,
		LHBM_BRT_LEN, out_norm,
		rgb_ratio[0], rgb_ratio[1], rgb_ratio[2]);
}

static void tg4a_lhbm_brightness_init(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct tg4a_panel *spanel = to_spanel(ctx);
	struct tg4a_lhbm_ctl *ctl = &spanel->lhbm_ctl;
	int group, freq, ret;

	for (freq = 0; freq < FREQUENCY_COUNT; freq++) {
		GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
		GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lhbm_brightness_read_index[freq]);
		ret = mipi_dsi_dcs_read(dsi, lhbm_brightness_read_reg,
			ctl->brt_normal[freq], LHBM_BRT_LEN);
		GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

		if (ret != LHBM_BRT_LEN) {
			dev_err(dev, "failed to read lhbm para for %s ret=%d\n",
				frequency_str[freq], ret);
			continue;
		}

		dev_info(dev, "lhbm normal brightness for %s: %*ph\n",
			frequency_str[freq], LHBM_BRT_LEN, ctl->brt_normal[freq]);

		for (group = 0; group < LHBM_OVERDRIVE_GRP_MAX; group++) {
			tg4a_calculate_lhbm_brightness(ctx, ctl->brt_normal[freq],
				lhbm_rgb_ratio[group], ctl->brt_overdrive[freq][group]);
		}
	}

	print_hex_dump_debug("tg4a-od-brightness: ", DUMP_PREFIX_NONE,
		16, 1,
		ctl->brt_overdrive, sizeof(ctl->brt_overdrive), false);
}

static void tg4a_debugfs_init(struct drm_panel *panel, struct dentry *root)
{
	if (IS_ENABLED(CONFIG_DEBUG_FS)) {
		struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
		struct dentry *panel_root, *csroot;

		if (!ctx)
			return;

		panel_root = debugfs_lookup("panel", root);
		if (!panel_root)
			return;

		csroot = debugfs_lookup("cmdsets", panel_root);
		if (!csroot)
			goto panel_out;

		gs_panel_debugfs_create_cmdset(csroot, &tg4a_init_cmdset, "init");

		dput(csroot);

panel_out:
		dput(panel_root);
	}
}

static void tg4a_panel_init(struct gs_panel *ctx)
{
	tg4a_lhbm_brightness_init(ctx);
	tg4a_lhbm_gamma_read(ctx);
	tg4a_lhbm_gamma_write(ctx);
}

static void tg4a_get_panel_rev(struct gs_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	u8 build_code = (id & 0xFF00) >> 8;
	u8 main = (build_code & 0xE0) >> 3;
	u8 sub = (build_code & 0x0C) >> 2;
	u8 rev = main | sub;

	gs_panel_get_panel_rev(ctx, rev);
}

static void tg4a_set_nolp_mode(struct gs_panel *ctx,
				  const struct gs_panel_mode *pmode)
{
	const struct gs_panel_mode *current_mode = ctx->current_mode;
	unsigned int vrefresh = current_mode ? drm_mode_vrefresh(&current_mode->mode) : 30;
	unsigned int te_usec = current_mode ? current_mode->gs_mode.te_usec : 1106;
	struct device *dev = ctx->dev;

	if (!gs_is_panel_active(ctx))
		return;

	/* AOD Mode Off Setting */
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0x53, 0x20);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	/* backlight control and dimming */
	tg4a_update_wrctrld(ctx);
	tg4a_change_frequency(ctx, pmode);

	gs_panel_wait_for_vsync_done(ctx, te_usec,
			GS_VREFRESH_TO_PERIOD_USEC(vrefresh));

	/* Additional sleep time to account for TE variability */
	usleep_range(1000, 1010);

	dev_info(dev, "exit LP mode\n");
}

static int tg4a_enable(struct drm_panel *panel)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	struct device *dev = ctx->dev;
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	const struct gs_panel_mode *pmode = ctx->current_mode;
	struct tg4a_panel *spanel = to_spanel(ctx);
	u8 ic_trim_pre_check[2];

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_dbg(ctx->dev, "%s\n", __func__);

	gs_panel_reset_helper(ctx);

	/* sleep out */
	GS_DCS_WRITE_DELAY_CMD(dev, 120, MIPI_DCS_EXIT_SLEEP_MODE);

	/* initial command */
	gs_panel_send_cmdset(ctx, &tg4a_init_cmdset);

	/* IC Trim Pre Check, only for EVT1.0 */
	if (ctx->panel_rev == PANEL_REV_EVT1) {
		GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_enable);
		if (mipi_dsi_dcs_read(dsi, 0xFA, ic_trim_pre_check, 2) == 2) {
			if (ic_trim_pre_check[0] == 0x31 && ic_trim_pre_check[1] == 0x00) {
				/* VDDD LDO Setting */
				GS_DCS_BUF_ADD_CMDLIST(dev, test_key_fc_enable);
				GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x58, 0xD7);
				GS_DCS_BUF_ADD_CMD(dev, 0xD7, 0x0A);
				GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x5B, 0xD7);
				GS_DCS_BUF_ADD_CMD(dev, 0xD7, 0x0A);
				GS_DCS_BUF_ADD_CMD(dev, 0xFE, 0x80);
				GS_DCS_BUF_ADD_CMD(dev, 0xFE, 0x00);
				GS_DCS_BUF_ADD_CMDLIST(dev, test_key_fc_disable);
			}
		} else {
			dev_err(dev, "fail to read IC Trim Pre Check parameters\n");
		}
		GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);
	}

	/* frequency */
	tg4a_change_frequency(ctx, pmode);

	tg4a_lhbm_gamma_write(ctx);

	/* DSC related configuration */
	mipi_dsi_compression_mode(to_mipi_dsi_device(dev), true);
	gs_dcs_write_dsc_config(dev, &pps_config);
	/* DSC Enable */
	GS_DCS_BUF_ADD_CMD(dev, 0xC2, 0x14);
	GS_DCS_BUF_ADD_CMD(dev, 0x9D, 0x01);

	/* dimming and HBM */
	tg4a_update_wrctrld(ctx);

	if (pmode->gs_mode.is_lp_mode)
		gs_panel_set_lp_mode_helper(ctx, pmode);

	GS_DCS_WRITE_CMD(dev, MIPI_DCS_SET_DISPLAY_ON);

	ctx->dsi_hs_clk_mbps = MIPI_DSI_FREQ_DEFAULT;
	spanel->lhbm_ctl.hist_roi_configured = false;

	return 0;
}

static int tg4a_panel_probe(struct mipi_dsi_device *dsi)
{
	struct tg4a_panel *spanel;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	spanel->is_pixel_off = false;

	return gs_dsi_panel_common_init(dsi, &spanel->base);
}

static void tg4a_pre_update_ffc(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;

	dev_dbg(ctx->dev, "%s\n", __func__);

	PANEL_ATRACE_BEGIN(__func__);

	/* FFC off */
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_fc_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x36, 0xC5);
	GS_DCS_BUF_ADD_CMD(dev, 0xC5, 0x10);
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_fc_disable);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	PANEL_ATRACE_END(__func__);
}

static void tg4a_update_ffc(struct gs_panel *ctx, unsigned int hs_clk_mbps)
{
	struct device *dev = ctx->dev;

	dev_dbg(ctx->dev, "%s: hs_clk_mbps: current=%u, target=%u\n",
		__func__, ctx->dsi_hs_clk_mbps, hs_clk_mbps);

	PANEL_ATRACE_BEGIN(__func__);

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_fc_enable);

	if (hs_clk_mbps != MIPI_DSI_FREQ_DEFAULT && hs_clk_mbps != MIPI_DSI_FREQ_ALTERNATIVE) {
		dev_warn(ctx->dev, "%s: invalid hs_clk_mbps=%u for FFC\n", __func__, hs_clk_mbps);
	} else if (ctx->dsi_hs_clk_mbps != hs_clk_mbps) {
		dev_info(ctx->dev, "%s: updating for hs_clk_mbps=%u\n", __func__, hs_clk_mbps);
		ctx->dsi_hs_clk_mbps = hs_clk_mbps;

		/* Update FFC */
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x3C, 0xC5);
		if (hs_clk_mbps == MIPI_DSI_FREQ_DEFAULT) {
			GS_DCS_BUF_ADD_CMD(dev, 0xC5, 0x45, 0xDE);
		} else { /* MIPI_DSI_FREQ_ALTERNATIVE */
			GS_DCS_BUF_ADD_CMD(dev, 0xC5, 0x4C, 0xFE);
		}
	}
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x36, 0xC5);
	GS_DCS_BUF_ADD_CMD(dev, 0xC5, 0x11, 0x10, 0x50, 0x05);
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_fc_disable);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	PANEL_ATRACE_END(__func__);
}

static void tg4a_set_ssc_en(struct gs_panel *ctx, bool enabled)
{
	struct device *dev = ctx->dev;
	const bool ssc_mode_update = ctx->ssc_en != enabled;

	if (!ssc_mode_update) {
		dev_dbg(ctx->dev, "ssc_mode skip update\n");
		return;
	}

	ctx->ssc_en = enabled;
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_fc_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x6E, 0xC5); /* global para */
	if (enabled)
		GS_DCS_BUF_ADD_CMD(dev, 0xC5, 0x07, 0x7F, 0x00, 0xFF);
	else
		GS_DCS_BUF_ADD_CMD(dev, 0xC5, 0x04);
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_fc_disable);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);
	dev_info(dev, "ssc_mode=%d\n", ctx->ssc_en);
}

static const struct gs_display_underrun_param underrun_param = {
	.te_idle_us = 1000,
	.te_var = 1,
};

static const u16 WIDTH_MM = 65, HEIGHT_MM = 146;
static const u16 HDISPLAY = 1080, VDISPLAY = 2424;
static const u16 HFP = 44, HSA = 16, HBP = 20;
static const u16 VFP = 10, VSA = 6, VBP = 10;

#define TG4A_DSC {\
	.enabled = true,\
	.dsc_count = 2,\
	.cfg = &pps_config,\
}

static const struct gs_panel_mode_array tg4a_modes = {
	.num_modes = 2,
	.modes = {
		{
			.mode = {
				.name = "1080x2424@60:60",
				DRM_MODE_TIMING(60, HDISPLAY, HFP, HSA, HBP,
							VDISPLAY, VFP, VSA, VBP),
				.flags = 0,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 8629,
				.bpc = 8,
				.dsc = TG4A_DSC,
				.underrun_param = &underrun_param,
			},
		},
		{
			.mode = {
				.name = "1080x2424@120:120",
				DRM_MODE_TIMING(120, HDISPLAY, HFP, HSA, HBP,
							VDISPLAY, VFP, VSA, VBP),
				.flags = 0,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 277,
				.bpc = 8,
				.dsc = TG4A_DSC,
				.underrun_param = &underrun_param,
			},
		},
	},
};

const struct brightness_capability tg4a_brightness_capability = {
	.normal = {
		.nits = {
			.min = 2,
			.max = 1200,
		},
		.level = {
			.min = 184,
			.max = 3427,
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
			.min = 3428,
			.max = 4095,
		},
		.percentage = {
			.min = 67,
			.max = 100,
		},
	},
};

static const struct gs_panel_mode_array tg4a_lp_modes = {
	.num_modes = 1,
	.modes = {
		{
			.mode = {
				.name = "1080x2424@30:30",
				DRM_MODE_TIMING(30, HDISPLAY, HFP, HSA, HBP,
								VDISPLAY, VFP, VSA, VBP),
				.flags = 0,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 1106,
				.bpc = 8,
				.dsc = TG4A_DSC,
				.underrun_param = &underrun_param,
				.is_lp_mode = true,
			},
		},
	},
};

static const struct drm_panel_funcs tg4a_drm_funcs = {
	.disable = gs_panel_disable,
	.unprepare = gs_panel_unprepare,
	.prepare = gs_panel_prepare,
	.enable = tg4a_enable,
	.get_modes = gs_panel_get_modes,
	.debugfs_init = tg4a_debugfs_init,
};

static const struct gs_panel_funcs tg4a_gs_funcs = {
	.set_brightness = tg4a_set_brightness,
	.set_lp_mode = gs_panel_set_lp_mode_helper,
	.set_nolp_mode = tg4a_set_nolp_mode,
	.set_binned_lp = gs_panel_set_binned_lp_helper,
	.set_dimming = tg4a_set_dimming,
	.set_hbm_mode = tg4a_set_hbm_mode,
	.set_local_hbm_mode = tg4a_set_local_hbm_mode,
	.set_local_hbm_mode_post = tg4a_set_local_hbm_mode_post,
	.is_mode_seamless = tg4a_is_mode_seamless,
	.mode_set = tg4a_mode_set,
	.panel_init = tg4a_panel_init,
	.get_panel_rev = tg4a_get_panel_rev,
	.read_id = gs_panel_read_slsi_ddic_id,
	.atomic_check = tg4a_atomic_check,
	.pre_update_ffc = tg4a_pre_update_ffc,
	.update_ffc = tg4a_update_ffc,
	.set_ssc_en = tg4a_set_ssc_en,
};

const struct gs_panel_brightness_desc tg4a_brightness_desc = {
	.max_brightness = 4095,
	.min_brightness = 2,
	.max_luminance = 10000000,
	.max_avg_luminance = 10000000,
	.min_luminance = 5,
	.default_brightness = 1290, /* 140 nits */
	.brt_capability = &tg4a_brightness_capability,
};

const struct gs_panel_reg_ctrl_desc tg4a_reg_ctrl_desc = {
	.reg_ctrl_enable = {
		{PANEL_REG_ID_VDDI, 0},
		{PANEL_REG_ID_VCI, 10},
	},
	.reg_ctrl_post_enable = {
		{PANEL_REG_ID_VDDD, 5},
	},
	.reg_ctrl_pre_disable = {
		{PANEL_REG_ID_VDDD, 0},
	},
	.reg_ctrl_disable = {
		{PANEL_REG_ID_VCI, 0},
		{PANEL_REG_ID_VDDI, 0},
	},
};

static struct gs_panel_lhbm_desc tg4a_lhbm_desc = {
	.effective_delay_frames = 1,
	.post_cmd_delay_frames = 1,
};

const struct gs_panel_desc google_tg4a = {
	.data_lane_cnt = 4,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.brightness_desc = &tg4a_brightness_desc,
	.modes = &tg4a_modes,
	.off_cmdset = &tg4a_off_cmdset,
	.lp_modes = &tg4a_lp_modes,
	.lp_cmdset = &tg4a_lp_cmdset,
	.binned_lp = tg4a_binned_lp,
	.num_binned_lp = ARRAY_SIZE(tg4a_binned_lp),
	.lhbm_desc = &tg4a_lhbm_desc,
	.reg_ctrl_desc = &tg4a_reg_ctrl_desc,
	.panel_func = &tg4a_drm_funcs,
	.gs_panel_func = &tg4a_gs_funcs,
	.default_dsi_hs_clk_mbps = MIPI_DSI_FREQ_DEFAULT,
	.reset_timing_ms = {1, 1, 1},
};

static const struct of_device_id gs_panel_of_match[] = {
	{ .compatible = "google,gs-tg4a", .data = &google_tg4a },
	{ }
};
MODULE_DEVICE_TABLE(of, gs_panel_of_match);

static struct mipi_dsi_driver gs_panel_driver = {
	.probe = tg4a_panel_probe,
	.remove = gs_dsi_panel_common_remove,
	.driver = {
		.name = "panel-gs-tg4a",
		.of_match_table = gs_panel_of_match,
	},
};
module_mipi_dsi_driver(gs_panel_driver);

MODULE_AUTHOR("Cathy Hsu <cathsu@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google tg4a panel driver");
MODULE_LICENSE("Dual MIT/GPL");

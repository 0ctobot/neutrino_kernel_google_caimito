/* SPDX-License-Identifier: MIT */
#include <drm/display/drm_dsc_helper.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/thermal.h>
#include <video/mipi_display.h>

#include "gs_panel/drm_panel_funcs_defaults.h"
#include "gs_panel/gs_panel.h"
#include "gs_panel/gs_panel_funcs_defaults.h"
#include "trace/panel_trace.h"

#define TG4C_DDIC_ID_LEN 11
#define TG4C_DIMMING_FRAME 32

#define MIPI_DSI_FREQ_MBPS_DEFAULT 1102
#define MIPI_DSI_FREQ_MBPS_ALTERNATIVE 1000

#define IRC_OFF_LHBM_EFFECTIVE_DELAY_FRAMES 3
#define NORMAL_LHBM_EFFECTIVE_DELAY_FRAMES 2

#define WIDTH_MM 64
#define HEIGHT_MM 145

#define PROJECT "TG4C"

enum tg4c_lhbm_brt {
	LHBM_R = 0,
	LHBM_G,
	LHBM_B,
	LHBM_BRT_MAX
};

enum tg4c_lhbm_brt_group {
	LHBM_BRT_GRP_0 = 0,
	LHBM_BRT_GRP_1,
	LHBM_BRT_GRP_2,
	LHBM_BRT_GRP_3,
	LHBM_BRT_GRP_4,
	LHBM_BRT_GRP_MAX,
};

#define LHBM_BRT_REG_LEN 2
#define LHBM_BRT_LEN (LHBM_BRT_MAX * LHBM_BRT_REG_LEN)

enum tg4c_lhbm_brt_overdrive_group {
	LHBM_OVERDRIVE_GRP_0_NIT = 0,
	LHBM_OVERDRIVE_GRP_6_NIT,
	LHBM_OVERDRIVE_GRP_50_NIT,
	LHBM_OVERDRIVE_GRP_300_NIT,
	LHBM_OVERDRIVE_GRP_IRC_OFF,
	LHBM_OVERDRIVE_GRP_MAX
};

struct tg4c_lhbm_ctl {
	/** @brt_normal: normal LHBM brightness parameters */
	u8 brt_normal[LHBM_BRT_GRP_MAX][LHBM_BRT_LEN];
	/** @brt_overdrive: overdrive LHBM brightness parameters */
	u8 brt_overdrive[LHBM_OVERDRIVE_GRP_MAX][LHBM_BRT_GRP_MAX][LHBM_BRT_LEN];
	/** @overdrived: whether or not LHBM is overdrived */
	bool overdrived;
	/** @hist_roi_configured: whether LHBM histogram configuration is done */
	bool hist_roi_configured;
	enum gs_drm_connector_lhbm_hist_roi_type hist_roi_type;
};


static const u8 tg4c_cmd2_page2[] = {0xF0, 0x55, 0xAA, 0x52, 0x08, 0x02};
static const u8 tg4c_lhbm_brightness_reg = 0xD1;

/**
 * struct tg4c_panel - panel specific runtime info
 *
 * This struct maintains tg4c panel specific runtime info, any fixed details about panel
 * should most likely go into struct gs_panel_desc.
 */
struct tg4c_panel {
	/** @base: base panel struct */
	struct gs_panel base;
	/** @is_hbm2_enabled: indicates panel is running in HBM mode 2 */
	bool is_hbm2_enabled;
	struct tg4c_lhbm_ctl lhbm_ctl;
};

#define to_spanel(ctx) container_of(ctx, struct tg4c_panel, base)

static const struct gs_dsi_cmd tg4c_lp_cmds[] = {
	/* disable dimming */
	GS_DSI_CMD(0x53, 0x20),
	/* Settings AOD Hclk */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x81),
	GS_DSI_CMD(0x6F, 0x0E),
	GS_DSI_CMD(0xF5, 0x20),
	/* enter AOD */
	GS_DSI_CMD(MIPI_DCS_ENTER_IDLE_MODE),
	/* DVDD Strong */
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0x6F, 0x03),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xC5, 0x24, 0x29, 0x29),
};
static DEFINE_GS_CMDSET(tg4c_lp);

static const struct gs_dsi_cmd tg4c_lp_off_cmds[] = {
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00, 0x00),
};

static const struct gs_dsi_cmd tg4c_lp_night_cmds[] = {
	/* 2 nit */
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00, 0x03),
};

static const struct gs_dsi_cmd tg4c_lp_low_cmds[] = {
	/* 10 nit */
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x07, 0xB2),
};

static const struct gs_dsi_cmd tg4c_lp_high_cmds[] = {
	/* 50 nit */
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x0F, 0xFE),
};

static const struct gs_binned_lp tg4c_binned_lp[] = {
	BINNED_LP_MODE("off", 0, tg4c_lp_off_cmds),
	/* night threshold 4 nits */
	BINNED_LP_MODE_TIMING("night", 104, tg4c_lp_night_cmds, 0, 45),
	/* rising = 0, falling = 45 */
	/* low threshold 40 nits */
	BINNED_LP_MODE_TIMING("low", 932, tg4c_lp_low_cmds, 0, 45),
	BINNED_LP_MODE_TIMING("high", 3574, tg4c_lp_high_cmds, 0, 45),
};

static const struct gs_dsi_cmd tg4c_off_cmds[] = {
	GS_DSI_DELAY_CMD(100, MIPI_DCS_SET_DISPLAY_OFF),
	GS_DSI_DELAY_CMD(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_GS_CMDSET(tg4c_off);

static const struct gs_dsi_cmd tg4c_init_cmds[] = {
	/* CMD2, Page1 */
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xC3, 0xDD),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xE5, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x08),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xE8, 0x13, 0x90, 0x80, 0x00),

	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_DVT1), 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x0A),
	/* BOIS_M on */
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_DVT1), 0xC6, 0x00, 0x03),
	/* DBI */
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x08),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xC2, 0x33, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0x6F, 0x03),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xC2, 0x77, 0x1F, 0x02, 0x00, 0x9E, 0xB8,
					0xCA, 0xD8, 0xE4, 0xF0, 0xFA, 0x00, 0x1D, 0x00, 0x00, 0x01),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xC3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
					0x00, 0x01, 0x01, 0x00, 0x01, 0x03, 0x00, 0x03, 0x0A, 0x00, 0x0A, 0x3D,
					0x10, 0x3D, 0xE1, 0x41, 0xE1, 0x00, 0x74, 0x00, 0xEF, 0x77, 0xEF, 0xEF,
					0x97, 0xEF, 0xAE,0x00, 0x00, 0x06, 0x90, 0x06, 0xAB),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xC5, 0x88, 0x00, 0x00, 0x88, 0x00, 0x00,
					0x88, 0x00, 0x00, 0x08, 0x00, 0x88, 0x00, 0x00, 0x88, 0x00, 0x00, 0x88,
					0x00, 0x00, 0x08, 0x00, 0x88, 0x00, 0x00, 0x88, 0x00, 0x00, 0x88, 0x00,
					0x00, 0x08, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xC6, 0xF8, 0x00, 0xFF, 0x88, 0x00, 0x00,
					0x88, 0x00, 0x00, 0xF8, 0x00, 0xFF, 0x88, 0x00, 0x00, 0x88, 0x00, 0x00,
					0xF8, 0x00, 0xFF, 0x88, 0x00, 0x00, 0x88, 0x00, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xC7, 0x15),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xC8, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03,
					0x06, 0x0D, 0x1A, 0x34),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xC9, 0x00, 0x01, 0x02, 0x04, 0x08, 0x10,
					0x1F, 0x3F, 0x7D, 0xFA),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xCA, 0x00, 0x01, 0x01, 0x02, 0x04, 0x09,
					0x12, 0x24, 0x47, 0x8E),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xCB, 0x21, 0x38, 0x55, 0x64, 0x2B, 0x92,
					0xCA, 0x5F, 0xE8, 0xFE, 0x59, 0x4A, 0xFF, 0x37, 0x89, 0xFE, 0x2D, 0xFF,
					0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
					0xFF, 0xFF, 0xFF),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xCC, 0x00, 0x60, 0xB9, 0x21, 0x56, 0x42,
					0x43, 0x1B, 0xD8, 0x86, 0x3A, 0xAC, 0xCA, 0x91, 0x86, 0xFD, 0xF4, 0xFF,
					0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
					0xFF, 0xFF, 0xFF),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xCE, 0x21, 0x1B, 0x16, 0x43, 0x81, 0xD3,
					0xA7, 0xB2, 0x73, 0xAA, 0xAE, 0xF5, 0xBB, 0xCC, 0x3D, 0xF9, 0xA1, 0xFF,
					0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
					0xFF, 0xFF, 0xFF),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xCF, 0x88, 0x78, 0x78, 0x78, 0x78, 0x7E,
					0x77, 0x7E, 0x01, 0x67, 0x01, 0x51, 0x66, 0x51, 0x6C, 0x46, 0x6C, 0xB6,
					0x24, 0xB6, 0xBE, 0x22, 0xBE, 0x00, 0x22, 0x00, 0x2E, 0x22, 0x2E, 0x2E,
					0x12, 0x2E, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
					0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0x6F, 0x32),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xCF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
					0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x68, 0x78,
					0x61, 0x16, 0x61, 0x04, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x33, 0x96,
					0x96, 0x33, 0x96, 0x5A, 0x23, 0x5A, 0x8F, 0x32, 0x8F, 0x7D, 0x33, 0x7D,
					0x3A, 0x23, 0x3A, 0xAB, 0x22, 0xAB, 0x38, 0x22),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0x6F, 0x64),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xCF, 0x38, 0x00, 0x12, 0x00, 0xDB, 0x11,
					0xDB, 0xDB, 0x11, 0xDB, 0x93, 0x33, 0x96, 0x61, 0x13, 0x61, 0x94, 0x88,
					0x5E, 0x5E, 0x48, 0x5E, 0x63, 0x44, 0x63, 0x35, 0x34, 0x35, 0x17, 0x33,
					0x17, 0x56, 0x13, 0x56, 0xCB, 0x11, 0xCB, 0x55, 0x21, 0x55, 0x00, 0x12,
					0x00, 0x7B, 0x11, 0x7B, 0x7B, 0x11, 0x7B, 0x2E),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0x6F, 0x96),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xCF, 0x38, 0x5E, 0x3C, 0x13, 0x3C, 0x2F),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xD0, 0x44, 0x00, 0x00, 0x44, 0x00, 0x00,
					0x44, 0x00, 0x00, 0x44, 0x00, 0x00, 0x44, 0x00, 0x00, 0x44, 0x00, 0x00,
					0x44, 0x00, 0x00, 0x44, 0x00, 0x00, 0x44, 0x00, 0x00, 0x44, 0x00, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xD2, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xD3, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xFF, 0xAA, 0x55, 0xA5, 0x84),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0x6F, 0x26),
	GS_DSI_REV_CMD(PANEL_REV_RANGE(PANEL_REV_EVT1_1, PANEL_REV_PVT), 0xF2, 0x00),

	/* Page Disable */
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x00, 0x00),
	/* CMD3, Page0 */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x80),
	GS_DSI_CMD(0x6F, 0x16),
	GS_DSI_CMD(0xF4, 0x02, 0x74),
	GS_DSI_CMD(0x6F, 0x31),
	GS_DSI_CMD(0xF8, 0x01, 0x74),
	GS_DSI_CMD(0x6F, 0x15),
	GS_DSI_CMD(0xF8, 0x01, 0x8D),

	/* AOD Source power saving */
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0x6F, 0x47),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xF2, 0x30),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0x6F, 0x37),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xF3, 0xCC, 0xCC),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0x6F, 0x3C),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xF3, 0x0C),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0x6F, 0x3E),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xF3, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0x6F, 0x02),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xF4, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0x6F, 0x37),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xF4, 0xF6, 0xF6),

	/* CMD3, Page1 */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x81),
	GS_DSI_CMD(0x6F, 0x0E),
	GS_DSI_CMD(0xF5, 0x2A),

	/* TE output @skip mode */
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0x6F, 0x0D),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xFB, 0x80),

	/* BOIS Clk, idle vfp clk off */
	GS_DSI_CMD(0x6F, 0x0F),
	GS_DSI_CMD(0xF5, 0x22),

	/* VESA_PS_build */
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0x6F, 0x02),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xF9, 0x84),

	GS_DSI_CMD(0x5F, 0x00, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_PVT), 0xFF, 0xAA, 0x55, 0xA5, 0x81),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_PVT), 0x6F, 0x0B),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_PVT), 0xFD, 0x04),
	/* CMD3, Page2 */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x82),
	GS_DSI_CMD(0x6F, 0x09),

	/* MIPI timing optimize */
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xF2, 0xFF),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xF2, 0x55),
	/* MIPI byte packet clk */
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_DVT1), 0xF8, 0x0F),

	/* DBI */
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xFF, 0xAA, 0x55, 0xA5, 0x83),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xFF, 0xAA, 0x55, 0xA5, 0x84),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0x6F, 0x26),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xF2, 0x00),

	/* CMD Disable */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x00),
	GS_DSI_CMD(0x35),
	GS_DSI_CMD(0x6F, 0x01),
	GS_DSI_CMD(0x35, 0x2D),
	GS_DSI_CMD(0x55, 0x00),
	/* BC Dimming OFF */
	GS_DSI_CMD(0x53, 0x20),
	GS_DSI_CMD(0x2A, 0x00, 0x00, 0x04, 0x37),
	GS_DSI_CMD(0x2B, 0x00, 0x00, 0x09, 0x77),
	/* Normal GMA */
	GS_DSI_CMD(0x26, 0x00),
	GS_DSI_CMD(0x51, 0x0E, 0x2C),
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_CMD(0x51, 0x0F, 0xFE),
	GS_DSI_CMD(0x81, 0x01, 0x19),
	GS_DSI_CMD(0x88, 0x01, 0x02, 0x1C, 0x06, 0xDD, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x90, 0x03, 0x43),
	GS_DSI_CMD(0x91, 0x89, 0xA8, 0x00, 0x18, 0xC2, 0x00, 0x02, 0x0E, 0x02, 0x4C, 0x00, 0x07,
				0x04, 0x2D, 0x04, 0x3D, 0x10, 0xF0),
	/* 60Hz */
	GS_DSI_CMD(0x2F, 0x02),

	/* FFC off */
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01),
	GS_DSI_CMD(0xC3, 0x00),
	/* FFC setting (MIPI: 1102) */
	GS_DSI_CMD(0xC3, 0x00, 0x06, 0x20, 0x11, 0xFF, 0x00, 0x06, 0x20, 0x11, 0xFF, 0x00, 0x05,
				0xBD, 0x1F, 0x06, 0x4F, 0x19, 0x05, 0xBD, 0x1F, 0x06, 0x4F, 0x19, 0x05, 0xBD,
				0x1F, 0x06, 0x4F, 0x19, 0x05, 0xBD, 0x1F, 0x06, 0x4F, 0x19, 0x05, 0xBD, 0x1F,
				0x06, 0x4F, 0x19),

	/* b/343332437: Extend DBI Flash Data Update Cycle time */
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x04),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xBB, 0xB3, 0x01, 0xBC),

	GS_DSI_DELAY_REV_CMD(60, PANEL_REV_LT(PANEL_REV_EVT1_1), MIPI_DCS_EXIT_SLEEP_MODE),
	GS_DSI_DELAY_REV_CMD(120, PANEL_REV_GE(PANEL_REV_EVT1_1), MIPI_DCS_EXIT_SLEEP_MODE)
};
static DEFINE_GS_CMDSET(tg4c_init);

static bool _is_max_hbm_level(u16 level, struct gs_panel * ctx) {
	return level == ctx->desc->brightness_desc->brt_capability->hbm.level.max;
}

static void tg4c_update_acd(struct gs_panel *ctx, u16 level) {
	struct device *dev = ctx->dev;
	struct tg4c_panel *spanel = to_spanel(ctx);

	if (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode) && _is_max_hbm_level(level, ctx)) {
		spanel->is_hbm2_enabled = true;
		/* set ACD Level 3 */
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x55, 0x04);
	} else {
		if (spanel->is_hbm2_enabled) {
			/* set ACD off */
			GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x55, 0x00);
		}
		spanel->is_hbm2_enabled = false;
	}
	dev_info(ctx->dev, "%s: is HBM2 enabled : %d\n",
				__func__, spanel->is_hbm2_enabled);
}

static void tg4c_update_te2(struct gs_panel *ctx)
{
	struct gs_panel_te2_timing timing;
	struct device *dev = ctx->dev;
	u8 width = 0x2D; /* default width 45H */
	u32 rising = 0, falling;
	int ret;

	ret = gs_panel_get_current_mode_te2(ctx, &timing);
	if (!ret) {
		falling = timing.falling_edge;
		if (falling >= timing.rising_edge) {
			rising = timing.rising_edge;
			width = falling - rising;
		} else {
			dev_warn(dev, "invalid timing, use default setting\n");
		}
	} else if (ret == -EAGAIN) {
		dev_dbg(dev, "Panel is not ready, use default setting\n");
	} else {
		return;
	}

	dev_dbg(dev, "TE2 updated: rising= 0x%x, width= 0x%x", rising, width);

	GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_TEAR_SCANLINE, 0x00, rising);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_SET_TEAR_ON, 0x00, width);
}

static void tg4c_update_irc(struct gs_panel *ctx, const enum gs_hbm_mode hbm_mode,
							const int vrefresh)
{
	struct device *dev = ctx->dev;
	const u16 level = gs_panel_get_brightness(ctx);

	if (GS_IS_HBM_ON_IRC_OFF(hbm_mode)) {
		/* sync from bigSurf : to achieve the max brightness with IRC off which
		 * need to set dbv to 0xFFF */
		if (_is_max_hbm_level(level, ctx)) {
			/* set brightness to hbm2 */
			GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x0F, 0xFF);
		}
		tg4c_update_acd(ctx, level);

		/* IRC Off */
		GS_DCS_BUF_ADD_CMD(dev, 0x5F, 0x01, 0x00);
		if (vrefresh == 120)
			GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x00);
		else
			GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x02);
	} else {
		const u8 val1 = level >> 8;
		const u8 val2 = level & 0xff;

		/* IRC ON */
		GS_DCS_BUF_ADD_CMD(dev, 0x5F, 0x00, 0x00);
		if (vrefresh == 120)
			GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x00);
		else
			GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x02);

		/* sync from bigSurf : restore the dbv value while IRC ON */
		GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, val1, val2);
	}
	/* Empty command is for flush */
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x00);
}

static void _write_local_hbm_brightness(struct device *dev,
	u8 (*brt_group)[LHBM_BRT_GRP_MAX][LHBM_BRT_LEN])
{
	enum tg4c_lhbm_brt_group group;

	GS_DCS_BUF_ADD_CMDLIST(dev, tg4c_cmd2_page2);
	for (group = 0; group < LHBM_BRT_GRP_MAX; group++) {
		u8 *brt = (*brt_group)[group];
		u8 lhbm_brt_offset = group * LHBM_BRT_LEN;
		enum tg4c_lhbm_brt ch;

		for (ch = 0; ch < LHBM_BRT_MAX; ch++) {
			u8 reg_offset = ch * LHBM_BRT_REG_LEN;

			GS_DCS_BUF_ADD_CMD(dev, 0x6F, lhbm_brt_offset + reg_offset);
			GS_DCS_BUF_ADD_CMD(dev, tg4c_lhbm_brightness_reg, brt[ch * 2],
								brt[ch * 2 + 1]);
		}
	}
	/* Empty command is for flush */
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x00);
}

/* sync from hlos/panel_config-tg4b-cal0.pb */
static const u32 dbv_table[] = {
	1, 2, 4, 5, 6, 7, 9, 10, 12, 13, 15, 16, 18, 19, 22, 23, 26, 27, 30, 31, 35,
	36, 40, 41, 157, 158, 159, 161, 162, 163, 164, 166, 167, 169, 170, 172, 173,
	175, 176, 179, 180, 183, 184, 187, 188, 361, 362, 535, 536, 712, 714, 1059,
	1062, 1352, 1353, 1933, 1934, 2117, 2118, 2262, 2282, 2422, 2446, 2587, 2616,
	2758, 2794, 2938, 2985, 3135, 3199, 3363, 3457, 3628, 3781, 3939
};

static const u32 nit_table[] = {
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 5, 5,
	5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 11, 11, 19, 19, 29,
	29, 54, 54, 81, 81, 152, 152, 185, 185, 217, 222, 261, 269, 318, 329, 392,
	409, 490, 520, 630, 684, 846, 957, 1200, 1450, 1800
};

static u32 _convert_dbv_to_nits(struct device *dev, u32 dbv)
{
	int count = ARRAY_SIZE(dbv_table);
	int i;

	for(i = count - 1; i >= 0; i--) {
		if (dbv >= dbv_table[i]) {
			u32 coef = mult_frac(1, (nit_table[i+1] - nit_table[i]) * 1000,
									dbv_table[i + 1] - dbv_table[i]);

			return panel_calc_linear_luminance(dbv - dbv_table[i], coef, nit_table[i]);
		}
	}

	return 0;
}

static const int dbv_band[] = {
	3629, 3735, 3810, 3890, 3939
};

static const int gray_band[] = {
	0, 25, 75, 125, 170, 215
};

static const int dbv_gray_coef[ARRAY_SIZE(dbv_band)][ARRAY_SIZE(gray_band)] = {
	{95, 97, 97, 99, 100, 101},
	{95, 97, 99, 100, 101, 103},
	{95, 97, 99, 100, 101, 103},
	{95, 97, 99, 100, 101, 103},
	{95, 97, 99, 100, 101, 103}
};

static int tg4c_get_local_hbm_brightness_coef(struct device *dev, u32 dbv, u32 gray) {
	int i;
	int dbv_grp = 0;
	int gray_grp = 0;

	for (i = 0; i < ARRAY_SIZE(dbv_band); i++) {
		if (dbv < dbv_band[i]) break;
		dbv_grp = i;
	}

	for(i = 0; i < ARRAY_SIZE(gray_band); i++) {
		if (gray < gray_band[i]) break;
		gray_grp = i;
	}

	dev_dbg(dev, "dbv_grp=%d, gray_grp=%d, coef=%d/100",
			dbv_grp, gray_grp, dbv_gray_coef[dbv_grp][gray_grp]);
	return dbv_gray_coef[dbv_grp][gray_grp];
}

static void _update_lhbm_brightness_irc_off(struct gs_panel *ctx, int coef) {
	struct tg4c_panel *spanel = to_spanel(ctx);
	struct device *dev = ctx->dev;

	int ch = 0;
	enum tg4c_lhbm_brt_group group;
	for (group= 0; group < LHBM_BRT_GRP_MAX ; group++) {
		u8 *p_norm = spanel->lhbm_ctl.brt_normal[group];
		u8 *p_over = spanel->lhbm_ctl.brt_overdrive[LHBM_OVERDRIVE_GRP_IRC_OFF][group];
		int val;
		for (ch = 0; ch < LHBM_BRT_MAX; ch++) {
			int p = ch * 2;

			val = (p_norm[p] << 8) | p_norm[p + 1];
			val = mult_frac(val, coef, 100);
			p_over[p] = (val & 0xFF00) >> 8;
			p_over[p + 1] = val & 0x00FF;
		}
	}

	for (group = 0; group < LHBM_BRT_GRP_MAX; group++)
		dev_dbg(dev, "normal lhbm brightness[%d]: %*ph\n",
			group, LHBM_BRT_LEN, spanel->lhbm_ctl.brt_normal[group]);

	for (group = 0; group < LHBM_BRT_GRP_MAX; group++)
		dev_dbg(dev, "irc off lhbm brightness[%d]: %*ph\n",
			group, LHBM_BRT_LEN, spanel->lhbm_ctl.brt_overdrive[LHBM_OVERDRIVE_GRP_IRC_OFF][group]);
}

static enum tg4c_lhbm_brt_overdrive_group tg4c_get_local_hbm_brightness_grp(struct gs_panel *ctx) {
	enum tg4c_lhbm_brt_overdrive_group group = LHBM_OVERDRIVE_GRP_MAX;
	struct device *dev = ctx->dev;
	u32 gray = ctx->gs_connector->lhbm_gray_level;
	u32 dbv = gs_panel_get_brightness(ctx);
	u32 normal_dbv_max = ctx->desc->brightness_desc->brt_capability->normal.level.max;
	u32 luma = 0;

	if (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode)) {
		int coef = tg4c_get_local_hbm_brightness_coef(dev, dbv, gray);

		_update_lhbm_brightness_irc_off(ctx, coef);
		group = LHBM_OVERDRIVE_GRP_IRC_OFF;
	} else if (gray < 15) {
		group = LHBM_OVERDRIVE_GRP_0_NIT;
	} else {
		if (dbv <= normal_dbv_max)
			luma = _convert_dbv_to_nits(dev, dbv);
		else
			luma = panel_calc_linear_luminance(dbv, 645, -1256);

		dev_dbg(dev, "lhbm overdrive | %d dbv = %d nits(255 gray)", dbv, luma);
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

	if (group == LHBM_OVERDRIVE_GRP_IRC_OFF) {
		dev_info(dev, "lhbm overdrive | gray=%u dbv=%u grp=%d\n", gray, dbv, group);
	} else {
		dev_info(dev, "lhbm overdrive | gray=%u dbv=%u luma=%u grp=%d\n", gray, dbv, luma, group);
	}
	return group;
}

static void tg4c_set_local_hbm_brightness(struct gs_panel *ctx, bool is_first_stage)
{
	struct tg4c_panel *spanel = to_spanel(ctx);
	struct device *dev = ctx->dev;
	struct tg4c_lhbm_ctl *ctl = &spanel->lhbm_ctl;
	u8 (*brt)[LHBM_BRT_GRP_MAX][LHBM_BRT_LEN];

	enum tg4c_lhbm_brt_overdrive_group group = LHBM_OVERDRIVE_GRP_MAX;
	dev_info(ctx->dev, "set LHBM brightness at %s stage\n", is_first_stage ? "1st" : "2nd");
	PANEL_ATRACE_BEGIN(__func__);
	if (is_first_stage) {
		group = tg4c_get_local_hbm_brightness_grp(ctx);
	} else {
		if (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode)) {
			dev_info(dev, "lhbm overdrive | irc off, no action\n");
			PANEL_ATRACE_END(__func__);
			return;
		}
	}

	if (group < LHBM_OVERDRIVE_GRP_MAX) {
		brt = &(ctl->brt_overdrive[group]);
		ctl->overdrived = true;
	} else {
		brt = &(ctl->brt_normal);
		ctl->overdrived = false;
	}
	dev_dbg(dev, "lhbm overdrive | set %s brightness: [%d]\n",
		ctl->overdrived ? "overdrive" : "normal", ctl->overdrived ? group : -1);

	_write_local_hbm_brightness(dev, brt);
	PANEL_ATRACE_END(__func__);
}

#define LHBM_GAMMASET1 3628
#define LHBM_GAMMASET2 2774
#define LHBM_GAMMASET3 2186

static void tg4c_set_local_hbm_gamma(struct gs_panel *ctx, u16 br)
{
	struct device *dev = ctx->dev;

	GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x07);
	if (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode) && _is_max_hbm_level(br, ctx))
		GS_DCS_BUF_ADD_CMD(dev, 0x87, 0x00);
	else if (br > LHBM_GAMMASET1)
		GS_DCS_BUF_ADD_CMD(dev, 0x87, 0x01);
	else if (br > LHBM_GAMMASET2)
		GS_DCS_BUF_ADD_CMD(dev, 0x87, 0x02);
	else if (br > LHBM_GAMMASET3)
		GS_DCS_BUF_ADD_CMD(dev, 0x87, 0x03);
	else
		GS_DCS_BUF_ADD_CMD(dev, 0x87, 0x04);
}

static void tg4c_set_local_hbm_mode(struct gs_panel *ctx, bool local_hbm_en)
{
	struct device *dev = ctx->dev;

	if (local_hbm_en) {
		u16 level = gs_panel_get_brightness(ctx);

		tg4c_set_local_hbm_brightness(ctx, level);
		tg4c_set_local_hbm_gamma(ctx, level);
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x87, 0x05);
	} else {
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x87, 0x00);
	}
}

static void tg4c_change_frequency(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	int vrefresh = drm_mode_vrefresh(&pmode->mode);
	struct device *dev = ctx->dev;

	if (vrefresh != 60 && vrefresh != 120) {
		dev_warn(dev, "%s: invalid refresh rate %dhz\n", __func__, vrefresh);
		return;
	}

	if (vrefresh != 120 && (!gs_is_local_hbm_disabled(ctx))) {
		dev_err(dev, "%s: switch to %dhz will fail when LHBM is on, disable LHBM\n",
				__func__, vrefresh);
		tg4c_set_local_hbm_mode(ctx, false);
		ctx->lhbm.effective_state = GLOCAL_HBM_DISABLED;
	}

	if (!GS_IS_HBM_ON(ctx->hbm_mode)) {
		if (vrefresh == 120)
			GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x2F, 0x00);
		else
			GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x2F, 0x02);
	} else {
		tg4c_update_irc(ctx, ctx->hbm_mode, vrefresh);
	}

	dev_dbg(dev, "%s: change to %dhz\n", __func__, vrefresh);
}

static void tg4c_set_dimming(struct gs_panel *ctx, bool dimming_on)
{
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *pmode = ctx->current_mode;

	if (pmode->gs_mode.is_lp_mode) {
		dev_warn(dev, "in lp mode, skip dimming update\n");
		return;
	}

	ctx->dimming_on = dimming_on;
	GS_DCS_WRITE_CMD(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY, ctx->dimming_on ? 0x28 : 0x20);
	dev_dbg(dev, "%s dimming_on=%d\n", __func__, dimming_on);
}

#define EXIT_IDLE_DELAY_FRAME 2
#define EXIT_IDLE_DELAY_DELTA 100
static void tg4c_set_nolp_mode(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;
	int vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (!gs_is_panel_active(ctx))
		return;
	/* exit AOD */
	if (ctx->panel_rev < PANEL_REV_EVT1_1) {
		GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x03);
		GS_DCS_BUF_ADD_CMD(dev, 0xC5, 0x00, 0x24, 0x24);
	}
	GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_EXIT_IDLE_MODE);
	GS_DCS_BUF_ADD_CMD(dev, 0xFF, 0xAA, 0x55, 0xA5, 0x81);
	GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x0E);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xF5, 0x2B);
	tg4c_change_frequency(ctx, pmode);

	/* Delay setting dimming on AOD exit until brightness has stabilized. */
	ctx->timestamps.idle_exit_dimming_delay_ts = ktime_add_us(
		ktime_get(), 100 + GS_VREFRESH_TO_PERIOD_USEC(vrefresh) * EXIT_IDLE_DELAY_FRAME);

	dev_info(dev, "exit LP mode\n");
}

static void tg4c_dimming_frame_setting(struct gs_panel *ctx, u8 dimming_frame)
{
	struct device *dev = ctx->dev;

	/* Fixed time 1 frame */
	if (!dimming_frame)
		dimming_frame = 0x01;

	GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	GS_DCS_BUF_ADD_CMD(dev, 0xB2, 0x19);
	GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x05);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xB2, dimming_frame, dimming_frame);
}

static int tg4c_enable(struct drm_panel *panel)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *pmode = ctx->current_mode;
	struct tg4c_panel *spanel = to_spanel(ctx);

	if (!pmode) {
		dev_err(dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_dbg(dev, "%s\n", __func__);

	/* toggle reset gpio */
	gs_panel_reset_helper(ctx);

	/* toggle reset gpio */
	gs_panel_send_cmdset(ctx, &tg4c_init_cmdset);

	/* frequency */
	tg4c_change_frequency(ctx, pmode);

	/* dimming frame */
	tg4c_dimming_frame_setting(ctx, TG4C_DIMMING_FRAME);
	ctx->timestamps.idle_exit_dimming_delay_ts = 0;

	if (pmode->gs_mode.is_lp_mode)
		gs_panel_set_lp_mode_helper(ctx, pmode);

	GS_DCS_WRITE_CMD(dev, MIPI_DCS_SET_DISPLAY_ON);

	spanel->lhbm_ctl.hist_roi_configured = false;
	ctx->dsi_hs_clk_mbps = MIPI_DSI_FREQ_MBPS_DEFAULT;

	return 0;
}

static int tg4c_disable(struct drm_panel *panel)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	struct tg4c_panel *spanel = to_spanel(ctx);
	int ret;

	spanel->is_hbm2_enabled = false;

	ret = gs_panel_disable(panel);
	if (ret)
		return ret;

	return 0;
}

static u32 tg4c_get_local_hbm_mode_effective_delay_frames(struct gs_panel *ctx) {
	if (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode)) {
		return IRC_OFF_LHBM_EFFECTIVE_DELAY_FRAMES;
	} else {
		return NORMAL_LHBM_EFFECTIVE_DELAY_FRAMES;
	}
}

static void tg4c_update_lhbm_hist_config(struct gs_panel *ctx, struct drm_atomic_state *state)
{
	struct tg4c_panel *spanel = to_spanel(ctx);
	struct tg4c_lhbm_ctl *ctl = &spanel->lhbm_ctl;
	const struct gs_panel_mode *pmode = ctx->current_mode;
	const struct drm_display_mode *mode;
	/* lhbm center below the center of AA: 540, radius: 100 */
	const int d = 540, r = 100;
	enum gs_drm_connector_lhbm_hist_roi_type new_hist_roi_type =
			(GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode)) ? GS_HIST_ROI_FULL_SCREEN : GS_HIST_ROI_CIRCLE;

	if (ctl->hist_roi_configured && new_hist_roi_type == ctl->hist_roi_type)
		return;

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return;
	}
	mode = &pmode->mode;
	if (new_hist_roi_type == GS_HIST_ROI_FULL_SCREEN) {
		// IRC OFF - Full screen
		gs_panel_update_lhbm_hist_data_helper(ctx, state, true, GS_HIST_ROI_FULL_SCREEN, d, r);
		dev_info(ctx->dev, "set roi: full screen");

	} else {
		// IRC ON - LHBM circle
		gs_panel_update_lhbm_hist_data_helper(ctx, state, true, GS_HIST_ROI_CIRCLE, d, r);
		dev_info(ctx->dev, "set roi: lhbm circle");
	}
	ctl->hist_roi_type = new_hist_roi_type;
	ctl->hist_roi_configured = true;
}

static int tg4c_atomic_check(struct gs_panel *ctx, struct drm_atomic_state *state)
{
	struct drm_connector *conn = &ctx->gs_connector->base;
	struct drm_connector_state *new_conn_state = drm_atomic_get_new_connector_state(state, conn);
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;

	tg4c_update_lhbm_hist_config(ctx, state);

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
					!drm_atomic_crtc_effectively_active(old_crtc_state) ? "resume" : "lp exit");
		}
	} else if (old_crtc_state->adjusted_mode.clock != old_crtc_state->mode.clock) {
		/* b/282222114: clock hacked in last commit due to resume or lp exit, undo that */
		new_crtc_state->mode_changed = true;
		new_crtc_state->adjusted_mode.clock = new_crtc_state->mode.clock;
		ctx->gs_connector->needs_commit = false;
		dev_dbg(ctx->dev, "restore mode (%s) clock after resume or lp exit\n",
				new_crtc_state->mode.name);
	}
	return 0;
}

static void tg4c_pre_update_ffc(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;

	dev_dbg(ctx->dev, "%s\n", __func__);

	PANEL_ATRACE_BEGIN(__func__);

	/* FFC off */
	GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xC3, 0x00);

	PANEL_ATRACE_END(__func__);
}

static void tg4c_update_ffc(struct gs_panel *ctx, unsigned int hs_clk_mbps)
{
	struct device *dev = ctx->dev;

	dev_dbg(ctx->dev, "%s: hs_clk_mbps: current=%d, target=%d\n",
		__func__, ctx->dsi_hs_clk_mbps, hs_clk_mbps);

	PANEL_ATRACE_BEGIN(__func__);

	if (hs_clk_mbps != MIPI_DSI_FREQ_MBPS_DEFAULT &&
	    hs_clk_mbps != MIPI_DSI_FREQ_MBPS_ALTERNATIVE) {
		dev_warn(ctx->dev, "invalid hs_clk_mbps=%d for FFC\n", hs_clk_mbps);
	} else if (ctx->dsi_hs_clk_mbps != hs_clk_mbps) {
		dev_info(ctx->dev, "%s: updating for hs_clk_mbps=%d\n", __func__, hs_clk_mbps);
		ctx->dsi_hs_clk_mbps = hs_clk_mbps;

		/* Update FFC */
		GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
		if (hs_clk_mbps == MIPI_DSI_FREQ_MBPS_DEFAULT)
			GS_DCS_BUF_ADD_CMD(dev, 0xC3, 0x00, 0x06, 0x20,
						0x11, 0xFF, 0x00, 0x06, 0x20, 0x11,
						0xFF, 0x00, 0x05, 0xBD, 0x1F, 0x06,
						0x4F, 0x19, 0x05, 0xBD, 0x1F, 0x06,
						0x4F, 0x19, 0x05, 0xBD, 0x1F, 0x06,
						0x4F, 0x19, 0x05, 0xBD, 0x1F, 0x06,
						0x4F, 0x19, 0x05, 0xBD, 0x1F, 0x06,
						0x4F, 0x19);
		else
			GS_DCS_BUF_ADD_CMD(dev, 0xC3, 0x00, 0x06, 0x20,
						0x10, 0xFF, 0x00, 0x06, 0x20, 0x10,
						0xFF, 0x00, 0x05, 0xEA, 0x1D, 0x06,
						0x1E, 0x16, 0x05, 0xEA, 0x1D, 0x06,
						0x1E, 0x16, 0x05, 0xEA, 0x1D, 0x06,
						0x1E, 0x16, 0x05, 0xEA, 0x1D, 0x06,
						0x1E, 0x16, 0x05, 0xEA, 0x1D, 0x06,
						0x1E, 0x16);
	}

	/* FFC on */
	GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xC3, 0xDD);

	PANEL_ATRACE_END(__func__);
}

#define MAX_BR_HBM_IRC_OFF 4095
static int tg4c_set_brightness(struct gs_panel *ctx, u16 br)
{
	struct device *dev = ctx->dev;

	if (ctx->current_mode->gs_mode.is_lp_mode) {
		if (gs_panel_has_func(ctx, set_binned_lp))
			ctx->desc->gs_panel_func->set_binned_lp(ctx, br);
		return 0;
	}

	if (ctx->timestamps.idle_exit_dimming_delay_ts &&
		(ktime_sub(ctx->timestamps.idle_exit_dimming_delay_ts, ktime_get()) <= 0)) {
		GS_DCS_WRITE_CMD(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY,
						ctx->dimming_on ? 0x28 : 0x20);
		ctx->timestamps.idle_exit_dimming_delay_ts = 0;
	}

	if (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode) && _is_max_hbm_level(br, ctx)) {
		/* set brightness to hbm2 */
		GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x0F, 0xFF);
		tg4c_update_acd(ctx, br);
	} else {
		tg4c_update_acd(ctx, br);
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
						br >> 8, br & 0xff);
	}

	return 0;
}

static void tg4c_set_hbm_mode(struct gs_panel *ctx, enum gs_hbm_mode hbm_mode)
{
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *pmode = ctx->current_mode;
	int vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (ctx->hbm_mode == hbm_mode)
		return;

	tg4c_update_irc(ctx, hbm_mode, vrefresh);

	ctx->hbm_mode = hbm_mode;
	dev_info(dev, "hbm_on=%d hbm_ircoff=%d\n", GS_IS_HBM_ON(ctx->hbm_mode),
			 GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void tg4c_set_local_hbm_mode_post(struct gs_panel *ctx)
{
	const struct tg4c_panel *spanel = to_spanel(ctx);

	if (spanel->lhbm_ctl.overdrived)
		tg4c_set_local_hbm_brightness(ctx, false);
}

static void tg4c_mode_set(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	tg4c_change_frequency(ctx, pmode);
}

static void tg4c_get_panel_rev(struct gs_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	const u8 build_code = (id & 0xFF00) >> 8;
	const u8 main = (build_code & 0xE0) >> 3;
	const u8 sub = (build_code & 0x0C) >> 2;

	gs_panel_get_panel_rev(ctx, main | sub);
}

static int tg4c_read_id(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);

	char buf[TG4C_DDIC_ID_LEN] = {0};
	int ret;

	GS_DCS_WRITE_CMD(dev, 0xFF, 0xAA, 0x55, 0xA5, 0x81);
	ret = mipi_dsi_dcs_read(dsi, 0xF2, buf, TG4C_DDIC_ID_LEN);
	if (ret != TG4C_DDIC_ID_LEN) {
		dev_warn(dev, "Unable to read DDIC id (%d)\n", ret);
		goto done;
	} else {
		ret = 0;
	}

	bin2hex(ctx->panel_id, buf, TG4C_DDIC_ID_LEN);
done:
	GS_DCS_WRITE_CMD(dev, 0xFF, 0xAA, 0x55, 0xA5, 0x00);
	return ret;
}

static const struct gs_display_underrun_param underrun_param = {
	.te_idle_us = 1000,
	.te_var = 1,
};

/* Truncate 8-bit signed value to 6-bit signed value */
#define TO_6BIT_SIGNED(v) ((v) & 0x3F)

static const struct drm_dsc_config tg4c_dsc_cfg = {
	/* Used DSC v1.2 */
	.dsc_version_major = 1,
	.dsc_version_minor = 2,
	.line_buf_depth = 9,
	.bits_per_component = 8,
	.convert_rgb = true, /* confirm */
	.slice_count = 2,
	.slice_width = 540,
	.slice_height = 24,
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
		{0, 4, TO_6BIT_SIGNED(2)},
		{0, 4, TO_6BIT_SIGNED(0)},
		{1, 5, TO_6BIT_SIGNED(0)},
		{1, 6, TO_6BIT_SIGNED(-2)},
		{3, 7, TO_6BIT_SIGNED(-4)},
		{3, 7, TO_6BIT_SIGNED(-6)},
		{3, 7, TO_6BIT_SIGNED(-8)},
		{3, 8, TO_6BIT_SIGNED(-8)},
		{3, 9, TO_6BIT_SIGNED(-8)},
		{3, 10, TO_6BIT_SIGNED(-10)},
		{5, 10, TO_6BIT_SIGNED(-10)},
		{5, 11, TO_6BIT_SIGNED(-12)},
		{5, 11, TO_6BIT_SIGNED(-12)},
		{9, 12, TO_6BIT_SIGNED(-12)},
		{12, 13, TO_6BIT_SIGNED(-12)},
	},
	.rc_model_size = 8192,
	.flatness_min_qp = 3,
	.flatness_max_qp = 12,
	.initial_scale_value = 32,
	.scale_decrement_interval = 7,
	.scale_increment_interval = 588,
	.nfl_bpg_offset = 1069,
	.slice_bpg_offset = 1085,
	.final_offset = 4336,
	.vbr_enable = false,
	.slice_chunk_size = 540,
	.native_422 = false,
	.native_420 = false,
	.second_line_bpg_offset = 0,
	.nsl_bpg_offset = 0,
	.second_line_offset_adj = 0,
};
#define TG4C_DSC {\
		.enabled = true, \
		.dsc_count = 2, \
		.cfg = &tg4c_dsc_cfg, \
}
static const struct gs_panel_mode_array tg4c_modes = {
	.num_modes = 2,
	.modes = {
		{
			.mode = {
				.name = "1080x2424@60:60",
				DRM_MODE_TIMING(60, 1080, 32, 12, 16, 2424, 12, 4, 15),
				/* aligned to bootloader setting */
				.type = DRM_MODE_TYPE_PREFERRED,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 8450,
				.bpc = 8,
				.dsc = TG4C_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = 0,
				.falling_edge = 45,
			},
		},
		{
			.mode = {
				.name = "1080x2424@120:120",
				DRM_MODE_TIMING(120, 1080, 32, 12, 16, 2424, 12, 4, 15),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 276,
				.bpc = 8,
				.dsc = TG4C_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = 0,
				.falling_edge = 45,
			},
		},
	},/* modes */
};

static const struct gs_panel_mode_array tg4c_lp_modes = {
	.num_modes = 1,
	.modes = {
		{
			.mode = {
				.name = "1080x2424@30:30",
				DRM_MODE_TIMING(30, 1080, 32, 12, 16, 2424, 12, 4, 15),
				.type = DRM_MODE_TYPE_DRIVER,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = TG4C_DSC,
				.underrun_param = &underrun_param,
				.is_lp_mode = true,
			},
		},
	},/* modes */
};

static void _update_lhbm_overdrive_brightness(struct gs_panel *ctx,
	enum tg4c_lhbm_brt_overdrive_group grp, enum tg4c_lhbm_brt ch, u16 offset)
{
	struct tg4c_panel *spanel = to_spanel(ctx);
	enum tg4c_lhbm_brt_group norm_grp;

	for (norm_grp = 0; norm_grp < LHBM_BRT_GRP_MAX ; norm_grp++) {
		u8 *p_norm = spanel->lhbm_ctl.brt_normal[norm_grp];
		u8 *p_over = spanel->lhbm_ctl.brt_overdrive[grp][norm_grp];
		u16 val;
		int p = ch * 2;

		val = (p_norm[p] << 8) | p_norm[p + 1];
		val += offset;
		p_over[p] = (val & 0xFF00) >> 8;
		p_over[p + 1] = val & 0x00FF;
	}
}

static int tg4c_get_normal_lhbm_brightness(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	struct tg4c_panel *spanel = to_spanel(ctx);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	enum tg4c_lhbm_brt_group group;
	int ret;

	GS_DCS_BUF_ADD_CMDLIST(dev, tg4c_cmd2_page2);
	for (group = 0; group < LHBM_BRT_GRP_MAX; group++) {
		u8 *p_norm = spanel->lhbm_ctl.brt_normal[group];
		u8 lhbm_brt_offset = group * LHBM_BRT_LEN;
		enum tg4c_lhbm_brt ch;

		/* read RGB gamma value */
		for (ch = 0; ch < LHBM_BRT_MAX; ch++) {
			u8 reg_offset = ch * LHBM_BRT_REG_LEN;

			GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x6F, lhbm_brt_offset + reg_offset);
			ret = mipi_dsi_dcs_read(dsi, 0xD1, p_norm + reg_offset, 2);
			if(ret != LHBM_BRT_REG_LEN) {
				dev_err(dev, "failed to read lhbm gamma set[%d] ret=%d\n",
					group, ret);
				return 1;
			}
		}
	}

	for (group = 0; group < LHBM_BRT_GRP_MAX; group++)
		dev_info(dev, "lhbm normal brightness[%d]: %*ph\n",
			group, LHBM_BRT_LEN, spanel->lhbm_ctl.brt_normal[group]);

	return 0;
}

static void tg4c_lhbm_brightness_init(struct gs_panel *ctx)
{
	struct tg4c_panel *spanel = to_spanel(ctx);
	enum tg4c_lhbm_brt_overdrive_group grp;
	enum tg4c_lhbm_brt_group normal_grp;

	if (tg4c_get_normal_lhbm_brightness(ctx)) {
		return;
	}

	/* 0 nit, R+724, G+591, B+867 */
	grp = LHBM_OVERDRIVE_GRP_0_NIT;
	_update_lhbm_overdrive_brightness(ctx, grp, LHBM_R, 0x2D4);
	_update_lhbm_overdrive_brightness(ctx, grp, LHBM_G, 0x24F);
	_update_lhbm_overdrive_brightness(ctx, grp, LHBM_B, 0x363);
	/* 0-6 nit, R+394, G+322, B+472 */
	grp = LHBM_OVERDRIVE_GRP_6_NIT;
	_update_lhbm_overdrive_brightness(ctx, grp, LHBM_R, 0x18A);
	_update_lhbm_overdrive_brightness(ctx, grp, LHBM_G, 0x142);
	_update_lhbm_overdrive_brightness(ctx, grp, LHBM_B, 0x1D8);
	/* 6-50 nit, R+283, G+231, B+338 */
	grp = LHBM_OVERDRIVE_GRP_50_NIT;
	_update_lhbm_overdrive_brightness(ctx, grp, LHBM_R, 0x11B);
	_update_lhbm_overdrive_brightness(ctx, grp, LHBM_G, 0xE7);
	_update_lhbm_overdrive_brightness(ctx, grp, LHBM_B, 0x152);
	/* 50-300 nit, R+154, G+126, B+184 */
	grp = LHBM_OVERDRIVE_GRP_300_NIT;
	_update_lhbm_overdrive_brightness(ctx, grp, LHBM_R, 0x9A);
	_update_lhbm_overdrive_brightness(ctx, grp, LHBM_G, 0x7E);
	_update_lhbm_overdrive_brightness(ctx, grp, LHBM_B, 0xB8);

	for (grp = 0; grp < LHBM_OVERDRIVE_GRP_MAX; grp++)
		for (normal_grp = 0; normal_grp < LHBM_BRT_GRP_MAX; normal_grp++)
			dev_info(ctx->dev, "lhbm overdrive brightness[%d][%d]: %*ph\n",
				grp, normal_grp, LHBM_BRT_LEN,
				spanel->lhbm_ctl.brt_overdrive[grp][normal_grp]);
}

static void tg4c_debugfs_init(struct drm_panel *panel, struct dentry *root)
{
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

	gs_panel_debugfs_create_cmdset(csroot, &tg4c_init_cmdset, "init");
	dput(csroot);
panel_out:
	dput(panel_root);
}

static void tg4c_panel_init(struct gs_panel *ctx)
{
	tg4c_dimming_frame_setting(ctx, TG4C_DIMMING_FRAME);
	tg4c_lhbm_brightness_init(ctx);
}

static int tg4c_panel_probe(struct mipi_dsi_device *dsi)
{
	struct tg4c_panel *spanel;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	spanel->is_hbm2_enabled = false;
	return gs_dsi_panel_common_init(dsi, &spanel->base);
}

static const struct drm_panel_funcs tg4c_drm_funcs = {
	.disable = tg4c_disable,
	.unprepare = gs_panel_unprepare,
	.prepare = gs_panel_prepare,
	.enable = tg4c_enable,
	.get_modes = gs_panel_get_modes,
	.debugfs_init = tg4c_debugfs_init,
};

static int tg4c_panel_config(struct gs_panel *ctx);

static const struct gs_panel_funcs tg4c_gs_funcs = {
	.set_brightness = tg4c_set_brightness,
	.set_lp_mode = gs_panel_set_lp_mode_helper,
	.set_nolp_mode = tg4c_set_nolp_mode,
	.set_binned_lp = gs_panel_set_binned_lp_helper,
	.set_local_hbm_mode = tg4c_set_local_hbm_mode,
	.set_local_hbm_mode_post = tg4c_set_local_hbm_mode_post,
	.set_hbm_mode = tg4c_set_hbm_mode,
	.set_dimming = tg4c_set_dimming,
	.is_mode_seamless = gs_panel_is_mode_seamless_helper,
	.mode_set = tg4c_mode_set,
	.panel_init = tg4c_panel_init,
	.panel_config = tg4c_panel_config,
	.get_panel_rev = tg4c_get_panel_rev,
	.get_te2_edges = gs_panel_get_te2_edges_helper,
	.set_te2_edges = gs_panel_set_te2_edges_helper,
	.update_te2 = tg4c_update_te2,
	.read_id = tg4c_read_id,
	.atomic_check = tg4c_atomic_check,
	.pre_update_ffc = tg4c_pre_update_ffc,
	.update_ffc = tg4c_update_ffc,
	.get_local_hbm_mode_effective_delay_frames =
		tg4c_get_local_hbm_mode_effective_delay_frames,
};

static const struct gs_brightness_configuration tg4c_btr_configs[] = {
	{
		.panel_rev = PANEL_REV_LATEST,
		.default_brightness = 1829,
		.brt_capability = {
			.normal = {
				.nits = {
					.min = 2,
					.max = 1200,
				},
				.level = {
					.min = 1,
					.max = 3628,
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
					.min = 3629,
					.max = 3939,
				},
				.percentage = {
					.min = 67,
					.max = 100,
				},
			},
		},
	},
};

static struct gs_panel_brightness_desc tg4c_brightness_desc = {
	.max_luminance = 10000000,
	.max_avg_luminance = 10000000,
	.min_luminance = 5,
};

static struct gs_panel_reg_ctrl_desc tg4c_reg_ctrl_desc = {
	.reg_ctrl_enable = {
		{PANEL_REG_ID_VDDI, 0},
		{PANEL_REG_ID_VCI, 0},
		{PANEL_REG_ID_VDDD, 11},
	},
	.reg_ctrl_disable = {
		{PANEL_REG_ID_VDDD, 0},
		{PANEL_REG_ID_VCI, 0},
		{PANEL_REG_ID_VDDI, 1},
	},
};

static struct gs_panel_lhbm_desc tg4c_lhbm_desc = {
	.lhbm_on_delay_frames = 2,
	.effective_delay_frames = 2,
	.post_cmd_delay_frames = 2,
};

static struct gs_panel_desc gs_tg4c = {
	.data_lane_cnt = 4,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.brightness_desc = &tg4c_brightness_desc,
	.modes = &tg4c_modes,
	.off_cmdset = &tg4c_off_cmdset,
	.lp_modes = &tg4c_lp_modes,
	.lp_cmdset = &tg4c_lp_cmdset,
	.binned_lp = tg4c_binned_lp,
	.num_binned_lp = ARRAY_SIZE(tg4c_binned_lp),
	.lhbm_desc = &tg4c_lhbm_desc,
	.has_off_binned_lp_entry = true,
	.reg_ctrl_desc = &tg4c_reg_ctrl_desc,
	.panel_func = &tg4c_drm_funcs,
	.gs_panel_func = &tg4c_gs_funcs,
	.reset_timing_ms = {1, 1, 20},
	.refresh_on_lp = true,
	.default_dsi_hs_clk_mbps = MIPI_DSI_FREQ_MBPS_DEFAULT,
};

static int tg4c_panel_config(struct gs_panel *ctx)
{
	gs_panel_model_init(ctx, PROJECT, 0);
	return gs_panel_update_brightness_desc(&tg4c_brightness_desc, tg4c_btr_configs,
						ARRAY_SIZE(tg4c_btr_configs), ctx->panel_rev);
}

static const struct of_device_id gs_panel_of_match[] = {
	{ .compatible = "google,gs-tg4c", .data = &gs_tg4c },
	{ }
};
MODULE_DEVICE_TABLE(of, gs_panel_of_match);

static struct mipi_dsi_driver gs_panel_driver = {
	.probe = tg4c_panel_probe,
	.remove = gs_dsi_panel_common_remove,
	.driver = {
		.name = "panel-gs-tg4c",
		.of_match_table = gs_panel_of_match,
	},
};
module_mipi_dsi_driver(gs_panel_driver);

MODULE_AUTHOR("Shin-Yu Wang <shinyuw@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google tg4c panel driver");
MODULE_LICENSE("Dual MIT/GPL");

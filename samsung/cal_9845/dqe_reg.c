// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Register access functions for Samsung Display Quality Enhancer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <cal_config.h>
#include <dqe_cal.h>
#include <decon_cal.h>
#include <drm/samsung_drm.h>

#include "regs-dqe.h"

#define DITHER_OFFSET_B0       0x400 /* CGC and disp dither */
#define MATRIX_OFFSET_B0       0x800 /* linear and gamma matrix */
#define DEGAMMA_LUT_OFFSET_B0  0x800
#define CGC_CON_OFFSET_B0      0x800
#define REGAMMA_LUT_OFFSET_B0  0x400
#define HIST_OFFSET_B0         0x400

struct cal_regs_dqe {
	struct cal_regs_desc desc;
	enum dqe_version version;
};

static struct cal_regs_dqe regs_dqe;

#define dqe_read(offset)	cal_read((&regs_dqe.desc), offset)
#define dqe_write(offset, val)	cal_write((&regs_dqe.desc), offset, val)
#define dqe_read_mask(offset, mask)		\
		cal_read_mask((&regs_dqe.desc), offset, mask)
#define dqe_write_mask(offset, val, mask)	\
		cal_write_mask((&regs_dqe.desc), offset, val, mask)
#define dqe_write_relaxed(offset, val)		\
		cal_write_relaxed((&regs_dqe.desc), offset, val)

#define dither_offset	(regs_dqe.version > DQE_V1 ? DITHER_OFFSET_B0 : 0)
#define dither_read(offset)		dqe_read(offset + dither_offset)
#define dither_write(offset, val)	dqe_write(offset + dither_offset, val)

#define matrix_offset	(regs_dqe.version > DQE_V1 ? MATRIX_OFFSET_B0 : 0)
#define matrix_write(offset, val)	dqe_write(offset + matrix_offset, val)
#define matrix_read_mask(offset, mask)	\
	dqe_read_mask(offset + matrix_offset, mask)

#define degamma_offset	(regs_dqe.version > DQE_V1 ? DEGAMMA_LUT_OFFSET_B0 : 0)
#define degamma_read(offset)		dqe_read(offset + degamma_offset)
#define degamma_write(offset, val)	dqe_write(offset + degamma_offset, val)

#define cgc_offset	(regs_dqe.version > DQE_V1 ? CGC_CON_OFFSET_B0 : 0)
#define cgc_read_mask(offset, mask)	dqe_read_mask(offset + cgc_offset, mask)
#define cgc_write_mask(offset, val, mask)	\
	dqe_write_mask(offset + cgc_offset, val, mask)

#define regamma_offset	(regs_dqe.version > DQE_V1 ? REGAMMA_LUT_OFFSET_B0 : 0)
#define regamma_read(offset)		dqe_read(offset + regamma_offset)
#define regamma_write(offset, val)	dqe_write(offset + regamma_offset, val)

#define hist_offset	(regs_dqe.version > DQE_V1 ? HIST_OFFSET_B0 : 0)
#define hist_read(offset)		dqe_read(offset + hist_offset)
#define hist_read_mask(offset, mask)	\
	dqe_read_mask(offset + hist_offset, mask)

void
dqe_regs_desc_init(void __iomem *regs, const char *name, enum dqe_version ver)
{
	regs_dqe.version = ver;
	regs_dqe.desc.regs = regs;
	regs_dqe.desc.name = name;
}

static void dqe_reg_set_img_size(u32 width, u32 height)
{
	u32 val;

	val = DQE_IMG_VSIZE(height) | DQE_IMG_HSIZE(width);
	dqe_write(DQE0_TOP_IMG_SIZE, val);
}

static void dqe_reg_set_full_img_size(u32 width, u32 height)
{
	u32 val;

	val = DQE_FULL_IMG_VSIZE(height) | DQE_FULL_IMG_HSIZE(width);
	dqe_write(DQE0_TOP_FRM_SIZE, val);

	val = DQE_FULL_PXL_NUM(width * height);
	dqe_write(DQE0_TOP_FRM_PXL_NUM, val);
}

static void dqe_reg_set_atc_ibsi(u32 width, u32 height)
{
	u32 hori_grid = DIV_ROUND_UP(width, 8);
	u32 verti_grid = DIV_ROUND_UP(height, 16);
	u32 ibsi_x, ibsi_y;

	ibsi_x = (1 << 16) / (hori_grid * 4);
	ibsi_y = (1 << 16) / (verti_grid * 4);

	dqe_write(DQE0_ATC_PARTIAL_IBSI_P1,
			ATC_IBSI_X_P1(ibsi_x) | ATC_IBSI_Y_P1(ibsi_y));

	ibsi_x = (1 << 16) / (hori_grid * 2);
	ibsi_y = (1 << 16) / (verti_grid * 2);
	dqe_write(DQE0_ATC_PARTIAL_IBSI_P2,
			ATC_IBSI_X_P2(ibsi_x) | ATC_IBSI_Y_P2(ibsi_y));
}

/* exposed to driver layer for DQE CAL APIs */
void dqe_reg_init(u32 width, u32 height)
{
	cal_log_debug(0, "%s +\n", __func__);
	dqe_reg_set_img_size(width, height);
	dqe_reg_set_full_img_size(width, height);
	dqe_reg_set_atc_ibsi(width, height);
	cal_log_debug(0, "%s -\n", __func__);
}

void dqe_reg_set_degamma_lut(const struct drm_color_lut *lut)
{
	int i;
	u32 val;

	cal_log_debug(0, "%s +\n", __func__);

	if (!lut) {
		degamma_write(DQE0_DEGAMMA_CON, 0);
		return;
	}

	degamma_write(DQE0_DEGAMMA_CON, DEGAMMA_EN);
	for (i = 0; i < DIV_ROUND_UP(DEGAMMA_LUT_SIZE, 2); ++i) {
		val = DEGAMMA_LUT_H(lut[i * 2 + 1].red) |
			DEGAMMA_LUT_L(lut[i * 2].red);
		degamma_write(DQE0_DEGAMMALUT(i), val);

		cal_log_debug(0, "[%d] 0x%x\n", i, val);
	}

	cal_log_debug(0, "%s -\n", __func__);
}

void dqe_reg_set_cgc_lut(const struct cgc_lut *lut)
{
	int i;

	cal_log_debug(0, "%s +\n", __func__);

	if (!lut) {
		cgc_write_mask(DQE0_CGC_CON, 0, CGC_EN);
		return;
	}

	for (i = 0; i < DRM_SAMSUNG_CGC_LUT_REG_CNT; ++i) {
		dqe_write_relaxed(DQE0_CGC_LUT_R(i), lut->r_values[i]);
		dqe_write_relaxed(DQE0_CGC_LUT_G(i), lut->g_values[i]);
		dqe_write_relaxed(DQE0_CGC_LUT_B(i), lut->b_values[i]);
	}

	cgc_write_mask(DQE0_CGC_CON, ~0, CGC_EN);

	cal_log_debug(0, "%s -\n", __func__);
}

void dqe_reg_set_regamma_lut(const struct drm_color_lut *lut)
{
	int i;
	u32 val;

	cal_log_debug(0, "%s +\n", __func__);

	if (!lut) {
		regamma_write(DQE0_REGAMMA_CON, 0);
		return;
	}

	regamma_write(DQE0_REGAMMA_CON, REGAMMA_EN);
	for (i = 0; i < DIV_ROUND_UP(REGAMMA_LUT_SIZE, 2); ++i) {
		val = REGAMMA_LUT_H(lut[i * 2 + 1].red) |
				REGAMMA_LUT_L(lut[i * 2].red);
		regamma_write(DQE0_REGAMMALUT_R(i), val);
		cal_log_debug(0, "[%d]   red: 0x%x\n", i, val);

		val = REGAMMA_LUT_H(lut[i * 2 + 1].green) |
				REGAMMA_LUT_L(lut[i * 2].green);
		regamma_write(DQE0_REGAMMALUT_G(i), val);
		cal_log_debug(0, "[%d] green: 0x%x\n", i, val);

		val = REGAMMA_LUT_H(lut[i * 2 + 1].blue) |
				REGAMMA_LUT_L(lut[i * 2].blue);
		regamma_write(DQE0_REGAMMALUT_B(i), val);
		cal_log_debug(0, "[%d]  blue: 0x%x\n", i, val);
	}

	cal_log_debug(0, "%s -\n", __func__);
}

static void dqe_reg_print_lut(u32 start, u32 count, const u32 offset)
{
	u32 val;
	int i;
	char buf[128];
	char *p = buf;
	const char *end = buf + sizeof(buf);

	for (i = 0; i < DIV_ROUND_UP(count, 2); ++i) {
		val = dqe_read(start + i * 4 + offset);

		p += scnprintf(p, end - p, "[%4d]%4x ", i * 2, GET_LUT_L(val));

		if ((i * 2 + 1) != count)
			p += scnprintf(p, end - p, "[%4d]%4x ", i * 2 + 1,
					GET_LUT_H(val));

		if ((i % 5) == 4) {
			cal_log_info(0, "%s\n", buf);
			p = buf;
		}
	}

	if (p != buf)
		cal_log_info(0, "%s\n", buf);
}

void dqe_reg_print_hist(void)
{
	u32 val;

	val = hist_read(DQE0_HIST);
	cal_log_info(0, "DQE: histogram EN(%d) ROI_ON(%d) LUMA_SEL(%d)\n",
			cal_mask(val, HIST_EN), cal_mask(val, HIST_ROI_ON),
			cal_mask(val, HIST_LUMA_SEL));

	val = hist_read(DQE0_HIST_SIZE);
	cal_log_info(0, "image size: %d x %d\n", cal_mask(val, HIST_HSIZE_MASK),
			cal_mask(val, HIST_VSIZE_MASK));

	val = hist_read(DQE0_HIST_START);
	cal_log_info(0, "ROI position start x,y(%d,%d)\n",
			cal_mask(val, HIST_START_X_MASK),
			cal_mask(val, HIST_START_Y_MASK));

	val = hist_read(DQE0_HIST_WEIGHT_0);
	cal_log_info(0, "weight red(%d) green(%d) blue(%d)\n",
			cal_mask(val, HIST_WEIGHT_R_MASK),
			cal_mask(val, HIST_WEIGHT_G_MASK),
			hist_read_mask(DQE0_HIST_WEIGHT_1, HIST_WEIGHT_B_MASK));

	cal_log_info(0, "threshold: %d\n",
			hist_read_mask(DQE0_HIST_THRESH, HIST_THRESHOLD_MASK));

	dqe_reg_print_lut(DQE0_HIST_BIN(0), HIST_BIN_SIZE, hist_offset);
}

void dqe_reg_print_gamma_matrix(void)
{
	const u32 offset = matrix_offset;

	cal_log_info(0, "DQE: gamma matrix %s\n",
			matrix_read_mask(DQE0_GAMMA_MATRIX_CON, GAMMA_MATRIX_EN)
			? "on" : "off");

	cal_log_info(0, "COEFFS:\n");
	dqe_reg_print_lut(DQE0_GAMMA_MATRIX_COEFF(0), GAMMA_MATRIX_COEFFS_CNT,
			offset);

	cal_log_info(0, "OFFSETS:\n");
	dqe_reg_print_lut(DQE0_GAMMA_MATRIX_OFFSET0, GAMMA_MATRIX_OFFSETS_CNT,
			offset);
}

void dqe_reg_print_linear_matrix(void)
{
	const u32 offset = matrix_offset;

	cal_log_info(0, "DQE: linear matrix %s\n",
			matrix_read_mask(DQE0_LINEAR_MATRIX_CON, LINEAR_MATRIX_EN)
			? "on" : "off");

	cal_log_info(0, "COEFFS:\n");
	dqe_reg_print_lut(DQE0_LINEAR_MATRIX_COEFF(0),
			LINEAR_MATRIX_COEFFS_CNT, offset);

	cal_log_info(0, "OFFSETS:\n");
	dqe_reg_print_lut(DQE0_LINEAR_MATRIX_OFFSET0,
			LINEAR_MATRIX_OFFSETS_CNT, offset);
}

void dqe_reg_set_cgc_dither(struct dither_config *config)
{
	u32 value = config ? cpu_to_le32(*(u32 *)config) : 0;

	dither_write(DQE0_CGC_DITHER, value);
}

void dqe_reg_set_disp_dither(struct dither_config *config)
{
	u32 value = config ? cpu_to_le32(*(u32 *)config) : 0;

	dither_write(DQE0_DISP_DITHER, value);
}

void dqe_reg_print_dither(enum dqe_dither_type dither)
{
	u32 val;
	const char * const dither_name[] = {
		[CGC_DITHER] = "CGC",
		[DISP_DITHER] = "DISP"
	};

	if (dither == CGC_DITHER)
		val = dither_read(DQE0_CGC_DITHER);
	else if (dither == DISP_DITHER)
		val = dither_read(DQE0_DISP_DITHER);
	else
		return;

	cal_log_info(0, "DQE: %s dither %s\n", dither_name[dither],
		(val & DITHER_EN_MASK) ? "on" : "off");
	cal_log_info(0, "%s mode, frame control %s, frame offset: %d\n",
		(val & DITHER_MODE) ? "Shift" : "Dither",
		(val & DITHER_FRAME_CON) ? "on" : "off",
		(val & DITHER_FRAME_OFFSET_MASK) >> DITHER_FRAME_OFFSET_SHIFT);
	cal_log_info(0, "Table red(%c) green(%c) blue(%c)\n",
		(val & DITHER_TABLE_SEL_R) ? 'B' : 'A',
		(val & DITHER_TABLE_SEL_G) ? 'B' : 'A',
		(val & DITHER_TABLE_SEL_B) ? 'B' : 'A');
}

void dqe_reg_print_degamma_lut(void)
{
	u32 val;

	val = degamma_read(DQE0_DEGAMMA_CON);
	cal_log_info(0, "DQE: degamma %s\n", val ? "on" : "off");

	if (!val)
		return;

	dqe_reg_print_lut(DQE0_DEGAMMALUT(0), DEGAMMA_LUT_SIZE, degamma_offset);
}

void dqe_reg_print_cgc_lut(u32 count)
{
	u32 val;

	val = cgc_read_mask(DQE0_CGC_CON, CGC_EN);
	cal_log_info(0, "DQE: cgc %s\n", val ? "on" : "off");

	if (!val)
		return;

	if (count > CGC_LUT_SIZE)
		count = CGC_LUT_SIZE;

	cal_log_info(0, "[Red]\n");
	dqe_reg_print_lut(DQE0_CGC_LUT_R(0), count, 0);

	cal_log_info(0, "[Green]\n");
	dqe_reg_print_lut(DQE0_CGC_LUT_G(0), count, 0);

	cal_log_info(0, "[Blue]\n");
	dqe_reg_print_lut(DQE0_CGC_LUT_B(0), count, 0);
}

void dqe_reg_print_regamma_lut(void)
{
	u32 val;
	const u32 offset = regamma_offset;

	val = regamma_read(DQE0_REGAMMA_CON);
	cal_log_info(0, "DQE: regamma %s\n", val ? "on" : "off");

	if (!val)
		return;

	cal_log_info(0, "[Red]\n");
	dqe_reg_print_lut(DQE0_REGAMMALUT_R(0), REGAMMA_LUT_SIZE, offset);

	cal_log_info(0, "[Green]\n");
	dqe_reg_print_lut(DQE0_REGAMMALUT_G(0), REGAMMA_LUT_SIZE, offset);

	cal_log_info(0, "[Blue]\n");
	dqe_reg_print_lut(DQE0_REGAMMALUT_B(0), REGAMMA_LUT_SIZE, offset);
}

void dqe_reg_set_linear_matrix(const struct exynos_matrix *lm)
{
	int i, reg_cnt;
	u32 val;

	cal_log_debug(0, "%s +\n", __func__);

	if (!lm) {
		matrix_write(DQE0_LINEAR_MATRIX_CON, 0);
		return;
	}

	reg_cnt = DIV_ROUND_UP(LINEAR_MATRIX_COEFFS_CNT , 2);
	for (i = 0; i < reg_cnt; ++i) {
		if (i == reg_cnt - 1)
			val = LINEAR_MATRIX_COEFF_L(lm->coeffs[i * 2]);
		else
			val = LINEAR_MATRIX_COEFF_H(lm->coeffs[i * 2 + 1]) |
				LINEAR_MATRIX_COEFF_L(lm->coeffs[i * 2]);
		matrix_write(DQE0_LINEAR_MATRIX_COEFF(i), val);
	}

	matrix_write(DQE0_LINEAR_MATRIX_OFFSET0,
			LINEAR_MATRIX_OFFSET_1(lm->offsets[1]) |
			LINEAR_MATRIX_OFFSET_0(lm->offsets[0]));
	matrix_write(DQE0_LINEAR_MATRIX_OFFSET1,
			LINEAR_MATRIX_OFFSET_2(lm->offsets[2]));

	matrix_write(DQE0_LINEAR_MATRIX_CON, LINEAR_MATRIX_EN);

	cal_log_debug(0, "%s -\n", __func__);
}

void dqe_reg_set_gamma_matrix(const struct exynos_matrix *matrix)
{
	int i, reg_cnt;
	u32 val;

	cal_log_debug(0, "%s +\n", __func__);

	if (!matrix) {
		matrix_write(DQE0_GAMMA_MATRIX_CON, 0);
		return;
	}

	reg_cnt = DIV_ROUND_UP(GAMMA_MATRIX_COEFFS_CNT , 2);
	for (i = 0; i < reg_cnt; ++i) {
		if (i == reg_cnt - 1)
			val = GAMMA_MATRIX_COEFF_L(matrix->coeffs[i * 2]);
		else
			val = GAMMA_MATRIX_COEFF_H(matrix->coeffs[i * 2 + 1]) |
				GAMMA_MATRIX_COEFF_L(matrix->coeffs[i * 2]);
		matrix_write(DQE0_GAMMA_MATRIX_COEFF(i), val);
	}

	matrix_write(DQE0_GAMMA_MATRIX_OFFSET0,
			GAMMA_MATRIX_OFFSET_1(matrix->offsets[1]) |
			GAMMA_MATRIX_OFFSET_0(matrix->offsets[0]));
	matrix_write(DQE0_GAMMA_MATRIX_OFFSET1,
			GAMMA_MATRIX_OFFSET_2(matrix->offsets[2]));

	matrix_write(DQE0_GAMMA_MATRIX_CON, GAMMA_MATRIX_EN);

	cal_log_debug(0, "%s -\n", __func__);
}

void dqe_reg_set_atc(const struct exynos_atc *atc)
{
	u32 val, mask;

	if (!atc) {
		dqe_write_mask(DQE0_ATC_CONTROL, 0, DQE_ATC_EN_MASK);
		return;
	}

	dqe_write_mask(DQE0_ATC_CONTROL, ~0, DQE_ATC_EN_MASK);

	val = ATC_LT(atc->lt) | ATC_NS(atc->ns) | ATC_ST(atc->st) |
		ATC_ONE_DITHER(atc->dither);
	dqe_write(DQE0_ATC_GAIN, val);

	val = ATC_PL_W1(atc->pl_w1) | ATC_PL_W2(atc->pl_w2);
	dqe_write(DQE0_ATC_WEIGHT, val);

	dqe_write(DQE0_ATC_CTMODE, atc->ctmode);
	dqe_write(DQE0_ATC_PPEN, atc->pp_en);

	val = ATC_TDR_MIN(atc->tdr_min) | ATC_TDR_MAX(atc->tdr_max) |
		ATC_UPGRADE_ON(atc->upgrade_on);
	mask = ATC_TDR_MIN_MASK | ATC_TDR_MAX_MASK | ATC_UPGRADE_ON_MASK;
	dqe_write_mask(DQE0_ATC_TDRMINMAX, val, mask);

	dqe_write(DQE0_ATC_AMBIENT_LIGHT, atc->ambient_light);
	dqe_write(DQE0_ATC_BACK_LIGHT, atc->back_light);
	dqe_write(DQE0_ATC_DSTEP, atc->dstep);
	dqe_write(DQE0_ATC_SCALE_MODE, atc->scale_mode);

	val = ATC_THRESHOLD_1(atc->threshold_1) |
		ATC_THRESHOLD_2(atc->threshold_2) |
		ATC_THRESHOLD_3(atc->threshold_3);
	dqe_write(DQE0_ATC_THRESHOLD, val);

	val = ATC_GAIN_LIMIT(atc->gain_limit) |
		ATC_LT_CALC_AB_SHIFT(atc->lt_calc_ab_shift);
	dqe_write(DQE0_ATC_GAIN_LIMIT, val);
}

void dqe_reg_print_atc(void)
{
	u32 val;

	val = dqe_read_mask(DQE0_ATC_CONTROL, DQE_ATC_EN_MASK);
	cal_log_info(0, "DQE: atc %s\n", val ? "on" : "off");

	if (!val)
		return;

	cal_log_info(0, "ATC configuration\n");
	dpu_print_hex_dump(regs_dqe.desc.regs,
			regs_dqe.desc.regs + DQE0_ATC_CONTROL, 0x40);
}

void dqe_reg_save_lpd_atc(u32 *lpd_atc_regs)
{
	int i;

	for (i = 0; i < LPD_ATC_REG_CNT; ++i)
		lpd_atc_regs[i] = dqe_read(DQE0_TOP_LPD_ATC_CON + (i * 4));

	dqe_write(DQE0_TOP_LPD_MODE_CONTROL, 0);
}

void dqe_reg_restore_lpd_atc(u32 *lpd_atc_regs)
{
	int i;

	dqe_write(DQE0_TOP_LPD_MODE_CONTROL, DQE_LPD_MODE_EXIT);

	for (i = 0; i < LPD_ATC_REG_CNT; ++i)
		dqe_write(DQE0_TOP_LPD_ATC_CON + (i * 4), lpd_atc_regs[i]);
}

bool dqe_reg_dimming_in_progress(void)
{
	return dqe_read_mask(DQE0_ATC_DIMMING_DONE_INTR,
			ATC_DIMMING_IN_PROGRESS);
}

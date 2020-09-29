/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2020 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Header file for GS101 DQE CAL
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __SAMSUNG_DQE_CAL_H__
#define __SAMSUNG_DQE_CAL_H__

#include <linux/types.h>
#include <drm/samsung_drm.h>
#include <drm/drm_mode.h>

#define DEGAMMA_LUT_SIZE	65
#define REGAMMA_LUT_SIZE	65

enum dqe_dither_type {
	CGC_DITHER = 0,
	DISP_DITHER = 1,
};

void dqe_regs_desc_init(void __iomem *regs, const char *name);
void dqe_reg_init(u32 width, u32 height);
void dqe_reg_set_degamma_lut(const struct drm_color_lut *lut);
void dqe_reg_set_cgc_lut(const struct cgc_lut *lut);
void dqe_reg_set_regamma_lut(const struct drm_color_lut *lut);
void dqe_reg_set_cgc_dither(u32 val);
void dqe_reg_set_disp_dither(u32 val);
void dqe_reg_print_dither(enum dqe_dither_type dither);
#endif /* __SAMSUNG_DQE_CAL_H__ */

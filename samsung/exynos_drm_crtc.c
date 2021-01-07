// SPDX-License-Identifier: GPL-2.0-only
/* exynos_drm_crtc.c
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 * Authors:
 *	Inki Dae <inki.dae@samsung.com>
 *	Joonyoung Shim <jy0922.shim@samsung.com>
 *	Seung-Woo Kim <sw0312.kim@samsung.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_vblank.h>
#include <drm/samsung_drm.h>

#include <dqe_cal.h>

#include "exynos_drm_crtc.h"
#include "exynos_drm_decon.h"
#include "exynos_drm_drv.h"
#include "exynos_drm_plane.h"

static void exynos_drm_crtc_atomic_enable(struct drm_crtc *crtc,
					  struct drm_crtc_state *old_state)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	if (exynos_crtc->ops->enable)
		exynos_crtc->ops->enable(exynos_crtc, old_state);

	drm_crtc_vblank_on(crtc);
}

static void exynos_drm_crtc_atomic_disable(struct drm_crtc *crtc,
					   struct drm_crtc_state *old_state)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	drm_atomic_helper_disable_planes_on_crtc(old_state, false);

	drm_crtc_vblank_off(crtc);

	if (exynos_crtc->ops->disable)
		exynos_crtc->ops->disable(exynos_crtc);

	if (crtc->state->event && !crtc->state->active) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		spin_unlock_irq(&crtc->dev->event_lock);

		crtc->state->event = NULL;
	}
}

static void exynos_crtc_update_lut(struct drm_crtc *crtc,
					struct drm_crtc_state *state)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct drm_color_lut *degamma_lut, *gamma_lut;
	struct cgc_lut *cgc_lut;
	struct decon_device *decon = exynos_crtc->ctx;
	struct exynos_drm_crtc_state *exynos_state;
	struct exynos_dqe_state *dqe_state;

	if (!decon->dqe)
		return;

	exynos_state = to_exynos_crtc_state(state);
	dqe_state = &exynos_state->dqe;

	if (exynos_state->cgc_lut) {
		cgc_lut = exynos_state->cgc_lut->data;
		dqe_state->cgc_lut = cgc_lut;
	} else {
		dqe_state->cgc_lut = NULL;
	}

	if (exynos_state->disp_dither)
		dqe_state->disp_dither_config = exynos_state->disp_dither->data;
	else
		dqe_state->disp_dither_config = NULL;

	if (exynos_state->cgc_dither)
		dqe_state->cgc_dither_config = exynos_state->cgc_dither->data;
	else
		dqe_state->cgc_dither_config = NULL;

	if (exynos_state->linear_matrix)
		dqe_state->linear_matrix = exynos_state->linear_matrix->data;
	else
		dqe_state->linear_matrix = NULL;

	if (exynos_state->gamma_matrix)
		dqe_state->gamma_matrix = exynos_state->gamma_matrix->data;
	else
		dqe_state->gamma_matrix = NULL;

	if (state->degamma_lut) {
		degamma_lut = state->degamma_lut->data;
		dqe_state->degamma_lut = degamma_lut;
	} else {
		dqe_state->degamma_lut = NULL;
	}

	if (state->gamma_lut) {
		gamma_lut = state->gamma_lut->data;
		dqe_state->regamma_lut = gamma_lut;
	} else {
		dqe_state->regamma_lut = NULL;
	}
}

static int exynos_crtc_atomic_check(struct drm_crtc *crtc,
				     struct drm_crtc_state *state)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct exynos_drm_crtc_state *new_exynos_state =
						to_exynos_crtc_state(state);
	struct drm_plane *plane;
	const struct drm_plane_state *plane_state;
	uint32_t max_bpc;

	DRM_DEBUG("%s +\n", __func__);

	if (!state->enable)
		return 0;

	exynos_crtc_update_lut(crtc, state);

	if (exynos_crtc->ops->atomic_check)
		exynos_crtc->ops->atomic_check(exynos_crtc, state);

	if (new_exynos_state->force_bpc == EXYNOS_BPC_MODE_UNSPECIFIED) {
		max_bpc = 8; /* initial bpc value */
		drm_atomic_crtc_state_for_each_plane_state(plane, plane_state, state) {
			const struct drm_format_info *info;
			const struct dpu_fmt *fmt_info;

			info = plane_state->fb->format;
			fmt_info = dpu_find_fmt_info(info->format);
			if (fmt_info->bpc == 10) {
				max_bpc = 10;
				break;
			}
		}
		new_exynos_state->in_bpc = max_bpc;
	} else {
		new_exynos_state->in_bpc =
			new_exynos_state->force_bpc == EXYNOS_BPC_MODE_10 ?
			10 : 8;
	}

	DRM_DEBUG("%s -\n", __func__);

	return 0;
}

static void exynos_crtc_atomic_begin(struct drm_crtc *crtc,
				     struct drm_crtc_state *old_crtc_state)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	if (exynos_crtc->ops->atomic_begin)
		exynos_crtc->ops->atomic_begin(exynos_crtc);
}

static void exynos_crtc_atomic_flush(struct drm_crtc *crtc,
				     struct drm_crtc_state *old_crtc_state)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	if (exynos_crtc->ops->atomic_flush)
		exynos_crtc->ops->atomic_flush(exynos_crtc, old_crtc_state);
}

static enum drm_mode_status exynos_crtc_mode_valid(struct drm_crtc *crtc,
	const struct drm_display_mode *mode)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	if (exynos_crtc->ops->mode_valid)
		return exynos_crtc->ops->mode_valid(exynos_crtc, mode);

	return MODE_OK;
}

static bool exynos_crtc_mode_fixup(struct drm_crtc *crtc,
				   const struct drm_display_mode *mode,
				   struct drm_display_mode *adjusted_mode)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	if (exynos_crtc->ops->mode_fixup)
		return exynos_crtc->ops->mode_fixup(exynos_crtc, mode,
						    adjusted_mode);

	return true;
}

static void exynos_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	const struct drm_crtc_state *crtc_state = crtc->state;

	if (exynos_crtc->ops->mode_set)
		return exynos_crtc->ops->mode_set(exynos_crtc,
						  &crtc_state->mode,
						  &crtc_state->adjusted_mode);
}

static const struct drm_crtc_helper_funcs exynos_crtc_helper_funcs = {
	.mode_valid	= exynos_crtc_mode_valid,
	.mode_fixup	= exynos_crtc_mode_fixup,
	.mode_set_nofb	= exynos_crtc_mode_set_nofb,
	.atomic_check	= exynos_crtc_atomic_check,
	.atomic_begin	= exynos_crtc_atomic_begin,
	.atomic_flush	= exynos_crtc_atomic_flush,
	.atomic_enable	= exynos_drm_crtc_atomic_enable,
	.atomic_disable	= exynos_drm_crtc_atomic_disable,
};

void exynos_crtc_handle_event(struct exynos_drm_crtc *exynos_crtc)
{
	struct drm_crtc *crtc = &exynos_crtc->base;
	struct drm_pending_vblank_event *event = crtc->state->event;
	unsigned long flags;

	if (!event)
		return;
	crtc->state->event = NULL;

	WARN_ON(drm_crtc_vblank_get(crtc) != 0);

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	drm_crtc_arm_vblank_event(crtc, event);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static void exynos_drm_crtc_destroy(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	drm_crtc_cleanup(crtc);
	kfree(exynos_crtc);
}

static int exynos_drm_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	if (exynos_crtc->ops->enable_vblank)
		return exynos_crtc->ops->enable_vblank(exynos_crtc);

	return 0;
}

static void exynos_drm_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	if (exynos_crtc->ops->disable_vblank)
		exynos_crtc->ops->disable_vblank(exynos_crtc);
}

static u32 exynos_drm_crtc_get_vblank_counter(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	if (exynos_crtc->ops->get_vblank_counter)
		return exynos_crtc->ops->get_vblank_counter(exynos_crtc);

	return 0;
}

static void exynos_drm_crtc_destroy_state(struct drm_crtc *crtc,
					struct drm_crtc_state *state)
{
	struct exynos_drm_crtc_state *exynos_crtc_state;

	exynos_crtc_state = to_exynos_crtc_state(state);
	drm_property_blob_put(exynos_crtc_state->cgc_lut);
	drm_property_blob_put(exynos_crtc_state->disp_dither);
	drm_property_blob_put(exynos_crtc_state->cgc_dither);
	drm_property_blob_put(exynos_crtc_state->linear_matrix);
	drm_property_blob_put(exynos_crtc_state->gamma_matrix);
	__drm_atomic_helper_crtc_destroy_state(state);
	kfree(exynos_crtc_state);
}

static void exynos_drm_crtc_reset(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc_state *exynos_crtc_state;

	if (crtc->state) {
		exynos_drm_crtc_destroy_state(crtc, crtc->state);
		crtc->state = NULL;
	}

	exynos_crtc_state = kzalloc(sizeof(*exynos_crtc_state), GFP_KERNEL);
	if (exynos_crtc_state) {
		crtc->state = &exynos_crtc_state->base;
		crtc->state->crtc = crtc;
	} else {
		pr_err("failed to allocate exynos crtc state\n");
	}
}

static struct drm_crtc_state *
exynos_drm_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc_state *exynos_crtc_state;
	struct exynos_drm_crtc_state *copy;

	exynos_crtc_state = to_exynos_crtc_state(crtc->state);
	copy = kzalloc(sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return NULL;

	memcpy(copy, exynos_crtc_state, sizeof(*copy));

	if (copy->cgc_lut)
		drm_property_blob_get(copy->cgc_lut);

	if (copy->disp_dither)
		drm_property_blob_get(copy->disp_dither);

	if (copy->cgc_dither)
		drm_property_blob_get(copy->cgc_dither);

	if (copy->linear_matrix)
		drm_property_blob_get(copy->linear_matrix);

	if (copy->gamma_matrix)
		drm_property_blob_get(copy->gamma_matrix);

	__drm_atomic_helper_crtc_duplicate_state(crtc, &copy->base);

	copy->seamless_mode_changed = false;

	return &copy->base;
}

static int
exynos_drm_replace_property_blob_from_id(struct drm_device *dev,
					 struct drm_property_blob **blob,
					 uint64_t blob_id,
					 ssize_t expected_size,
					 ssize_t expected_elem_size)
{
	struct drm_property_blob *new_blob = NULL;

	if (blob_id != 0) {
		new_blob = drm_property_lookup_blob(dev, blob_id);
		if (new_blob == NULL)
			return -EINVAL;

		if (expected_size > 0 &&
		    new_blob->length != expected_size) {
			drm_property_blob_put(new_blob);
			return -EINVAL;
		}
		if (expected_elem_size > 0 &&
		    new_blob->length % expected_elem_size != 0) {
			drm_property_blob_put(new_blob);
			return -EINVAL;
		}
	}

	drm_property_replace_blob(blob, new_blob);
	drm_property_blob_put(new_blob);

	return 0;
}

static int exynos_drm_crtc_set_property(struct drm_crtc *crtc,
					struct drm_crtc_state *state,
					struct drm_property *property,
					uint64_t val)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct exynos_drm_crtc_state *exynos_crtc_state;
	int ret;

	exynos_crtc_state = to_exynos_crtc_state(state);

	if (property == exynos_crtc->props.color_mode) {
		exynos_crtc_state->color_mode = val;
	} else if (property == exynos_crtc->props.force_bpc) {
		exynos_crtc_state->force_bpc = val;
	} else if (property == exynos_crtc->props.ppc ||
			property == exynos_crtc->props.max_disp_freq) {
		return 0;
	} else if (property == exynos_crtc->props.cgc_lut) {
		ret = exynos_drm_replace_property_blob_from_id(state->crtc->dev,
				&exynos_crtc_state->cgc_lut, val,
				sizeof(struct cgc_lut), -1);
		return ret;
	} else if (property == exynos_crtc->props.disp_dither) {
		ret = exynos_drm_replace_property_blob_from_id(state->crtc->dev,
				&exynos_crtc_state->disp_dither, val,
				sizeof(struct dither_config), -1);
		return ret;
	} else if (property == exynos_crtc->props.cgc_dither) {
		ret = exynos_drm_replace_property_blob_from_id(state->crtc->dev,
				&exynos_crtc_state->cgc_dither, val,
				sizeof(struct dither_config), -1);
		return ret;
	} else if (property == exynos_crtc->props.linear_matrix) {
		ret = exynos_drm_replace_property_blob_from_id(state->crtc->dev,
				&exynos_crtc_state->linear_matrix, val,
				sizeof(struct exynos_matrix), -1);
		return ret;
	} else if (property == exynos_crtc->props.gamma_matrix) {
		ret = exynos_drm_replace_property_blob_from_id(state->crtc->dev,
				&exynos_crtc_state->gamma_matrix, val,
				sizeof(struct exynos_matrix), -1);
		return ret;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int exynos_drm_crtc_get_property(struct drm_crtc *crtc,
					const struct drm_crtc_state *state,
					struct drm_property *property,
					uint64_t *val)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct exynos_drm_crtc_state *exynos_crtc_state;
	struct decon_device *decon = exynos_crtc->ctx;

	exynos_crtc_state =
		to_exynos_crtc_state((struct drm_crtc_state *)state);

	if (property == exynos_crtc->props.color_mode)
		*val = exynos_crtc_state->color_mode;
	else if (property == exynos_crtc->props.ppc)
		*val = decon->bts.ppc;
	else if (property == exynos_crtc->props.max_disp_freq)
		*val = decon->bts.dvfs_max_disp_freq;
	else if (property == exynos_crtc->props.force_bpc)
		*val = exynos_crtc_state->force_bpc;
	else if (property == exynos_crtc->props.cgc_lut)
		*val = (exynos_crtc_state->cgc_lut) ?
			exynos_crtc_state->cgc_lut->base.id : 0;
	else if (property == exynos_crtc->props.disp_dither)
		*val = (exynos_crtc_state->disp_dither) ?
			exynos_crtc_state->disp_dither->base.id : 0;
	else if (property == exynos_crtc->props.cgc_dither)
		*val = (exynos_crtc_state->cgc_dither) ?
			exynos_crtc_state->cgc_dither->base.id : 0;
	else if (property == exynos_crtc->props.linear_matrix)
		*val = (exynos_crtc_state->linear_matrix) ?
			exynos_crtc_state->linear_matrix->base.id : 0;
	else if (property == exynos_crtc->props.gamma_matrix)
		*val = (exynos_crtc_state->gamma_matrix) ?
			exynos_crtc_state->gamma_matrix->base.id : 0;
	else
		return -EINVAL;

	return 0;
}

static void exynos_drm_crtc_print_state(struct drm_printer *p,
					const struct drm_crtc_state *state)
{
	const struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(state->crtc);
	const struct exynos_drm_crtc_state *exynos_crtc_state = to_exynos_crtc_state(state);
	const struct decon_device *decon = exynos_crtc->ctx;
	const struct decon_config *cfg = &decon->config;

	drm_printf(p, "\treserved_win_mask=0x%x\n", exynos_crtc_state->reserved_win_mask);
	drm_printf(p, "\tDecon #%d (state:%d)\n", decon->id, decon->state);
	drm_printf(p, "\t\ttype=0x%x\n", cfg->out_type);
	drm_printf(p, "\t\tsize=%dx%d\n", cfg->image_width, cfg->image_height);
	if (cfg->mode.dsi_mode != DSI_MODE_NONE) {
		drm_printf(p, "\t\tdsi_mode=%s (%d)\n",
			cfg->mode.op_mode == DECON_VIDEO_MODE ? "vid" : "cmd",
			cfg->mode.dsi_mode);
		if (cfg->mode.op_mode == DECON_COMMAND_MODE)
			drm_printf(p, "\t\ttrig_mode=%s ddi=%d\n",
				cfg->mode.trig_mode == DECON_HW_TRIG ? "hw" : "sw",
				cfg->te_from);
	}
	drm_printf(p, "\t\tbpc=%d\n", cfg->out_bpc);
}

static int exynos_drm_crtc_late_register(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct decon_device *decon = exynos_crtc->ctx;

	return dpu_init_debug(decon);
}

static const struct drm_crtc_funcs exynos_crtc_funcs = {
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.destroy		= exynos_drm_crtc_destroy,
	.reset			= exynos_drm_crtc_reset,
	.atomic_duplicate_state	= exynos_drm_crtc_duplicate_state,
	.atomic_destroy_state	= exynos_drm_crtc_destroy_state,
	.atomic_set_property	= exynos_drm_crtc_set_property,
	.atomic_get_property	= exynos_drm_crtc_get_property,
	.atomic_print_state     = exynos_drm_crtc_print_state,
	.enable_vblank		= exynos_drm_crtc_enable_vblank,
	.disable_vblank		= exynos_drm_crtc_disable_vblank,
	.get_vblank_counter	= exynos_drm_crtc_get_vblank_counter,
	.late_register		= exynos_drm_crtc_late_register,
};

static int
exynos_drm_crtc_create_color_mode_property(struct exynos_drm_crtc *exynos_crtc)
{
	struct drm_crtc *crtc = &exynos_crtc->base;
	struct drm_property *prop;
	static const struct drm_prop_enum_list color_mode_list[] = {
		{ HAL_COLOR_MODE_NATIVE, "Native" },
		{ HAL_COLOR_MODE_STANDARD_BT601_625, "BT601_625" },
		{ HAL_COLOR_MODE_STANDARD_BT601_625_UNADJUSTED,
						"BT601_625_UNADJUSTED" },
		{ HAL_COLOR_MODE_STANDARD_BT601_525, "BT601_525" },
		{ HAL_COLOR_MODE_STANDARD_BT601_525_UNADJUSTED,
						"BT601_525_UNADJUSTED" },
		{ HAL_COLOR_MODE_STANDARD_BT709, "BT709" },
		{ HAL_COLOR_MODE_DCI_P3, "DCI-P3" },
		{ HAL_COLOR_MODE_SRGB, "sRGB" },
		{ HAL_COLOR_MODE_ADOBE_RGB, "Adobe RGB" },
		{ HAL_COLOR_MODE_DISPLAY_P3, "Display P3" },
		{ HAL_COLOR_MODE_BT2020, "BT2020" },
		{ HAL_COLOR_MODE_BT2100_PQ, "BT2100 PQ" },
		{ HAL_COLOR_MODE_BT2100_HLG, "BT2100 HLG" },
	};

	prop = drm_property_create_enum(crtc->dev, 0, "color mode",
			color_mode_list, ARRAY_SIZE(color_mode_list));
	if (!prop)
		return -ENOMEM;

	drm_object_attach_property(&crtc->base, prop, HAL_COLOR_MODE_NATIVE);
	exynos_crtc->props.color_mode = prop;

	return 0;
}

static int
exynos_drm_crtc_create_force_bpc_property(struct exynos_drm_crtc *exynos_crtc)
{
	struct drm_crtc *crtc = &exynos_crtc->base;
	struct drm_property *prop;
	static const struct drm_prop_enum_list bpc_list[] = {
		{ EXYNOS_BPC_MODE_UNSPECIFIED, "Unspecified" },
		{ EXYNOS_BPC_MODE_8, "8bpc" },
		{ EXYNOS_BPC_MODE_10, "10bpc" },
	};

	prop = drm_property_create_enum(crtc->dev, 0, "force_bpc", bpc_list,
			ARRAY_SIZE(bpc_list));
	if (!prop)
		return -ENOMEM;

	drm_object_attach_property(&crtc->base, prop,
				EXYNOS_BPC_MODE_UNSPECIFIED);
	exynos_crtc->props.force_bpc = prop;

	return 0;
}

static int exynos_drm_crtc_create_blob(struct drm_crtc *crtc, const char *name,
		struct drm_property **prop)
{
	struct drm_property *p;

	p = drm_property_create(crtc->dev, DRM_MODE_PROP_BLOB, name, 0);
	if (!p)
		return -ENOMEM;

	drm_object_attach_property(&crtc->base, p, 0);
	*prop = p;

	return 0;
}

static int exynos_drm_crtc_create_range(struct drm_crtc *crtc, const char *name,
		struct drm_property **prop)
{
	struct drm_property *p;

	p = drm_property_create_range(crtc->dev, 0, name, 0, UINT_MAX);
	if (!p)
		return -ENOMEM;

	drm_object_attach_property(&crtc->base, p, 0);
	*prop = p;

	return 0;
}

struct exynos_drm_crtc *exynos_drm_crtc_create(struct drm_device *drm_dev,
					struct drm_plane *plane,
					enum exynos_drm_output_type type,
					const struct exynos_drm_crtc_ops *ops,
					void *ctx)
{
	struct exynos_drm_crtc *exynos_crtc;
	struct drm_crtc *crtc;
	const struct decon_device *decon = ctx;
	int ret;

	exynos_crtc = kzalloc(sizeof(*exynos_crtc), GFP_KERNEL);
	if (!exynos_crtc)
		return ERR_PTR(-ENOMEM);

	exynos_crtc->possible_type = type;
	exynos_crtc->ops = ops;
	exynos_crtc->ctx = ctx;

	crtc = &exynos_crtc->base;

	ret = drm_crtc_init_with_planes(drm_dev, crtc, plane, NULL,
					&exynos_crtc_funcs, "exynos-crtc-%d",
					decon->id);
	if (ret < 0)
		goto err_crtc;

	drm_crtc_helper_add(crtc, &exynos_crtc_helper_funcs);

	ret = exynos_drm_crtc_create_color_mode_property(exynos_crtc);
	if (ret)
		goto err_crtc;

	ret = exynos_drm_crtc_create_force_bpc_property(exynos_crtc);
	if (ret)
		goto err_crtc;

	if (exynos_drm_crtc_create_range(crtc, "ppc", &exynos_crtc->props.ppc))
		goto err_crtc;

	if (exynos_drm_crtc_create_range(crtc, "max_disp_freq",
				&exynos_crtc->props.max_disp_freq))
		goto err_crtc;

	if (decon->dqe) {
		ret = exynos_drm_crtc_create_blob(crtc, "cgc_lut",
				&exynos_crtc->props.cgc_lut);
		if (ret)
			goto err_crtc;
		ret = exynos_drm_crtc_create_blob(crtc, "disp_dither",
				&exynos_crtc->props.disp_dither);
		if (ret)
			goto err_crtc;

		ret = exynos_drm_crtc_create_blob(crtc, "cgc_dither",
				&exynos_crtc->props.cgc_dither);
		if (ret)
			goto err_crtc;

		drm_crtc_enable_color_mgmt(crtc, DEGAMMA_LUT_SIZE, false,
				REGAMMA_LUT_SIZE);

		ret = exynos_drm_crtc_create_blob(crtc, "linear_matrix",
				&exynos_crtc->props.linear_matrix);
		if (ret)
			goto err_crtc;

		ret = exynos_drm_crtc_create_blob(crtc, "gamma_matrix",
				&exynos_crtc->props.gamma_matrix);
		if (ret)
			goto err_crtc;
	}

	return exynos_crtc;

err_crtc:
	plane->funcs->destroy(plane);
	kfree(exynos_crtc);
	return ERR_PTR(ret);
}

uint32_t exynos_drm_get_possible_crtcs(const struct drm_encoder *encoder,
		enum exynos_drm_output_type out_type)
{
	const struct drm_crtc *crtc;
	uint32_t possible_crtcs = 0;

	drm_for_each_crtc(crtc, encoder->dev) {
		if (to_exynos_crtc(crtc)->possible_type & out_type)
			possible_crtcs |= drm_crtc_mask(crtc);
	}

	return possible_crtcs;
}

void exynos_drm_crtc_te_handler(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	if (exynos_crtc->ops->te_handler)
		exynos_crtc->ops->te_handler(exynos_crtc);
}

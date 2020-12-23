/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _EXYNOS_DRM_CONNECTOR_H_
#define _EXYNOS_DRM_CONNECTOR_H_

#include <drm/drm_atomic.h>
#include <drm/drm_connector.h>
#include <drm/samsung_drm.h>

struct exynos_drm_connector;

struct exynos_drm_connector_properties {
	struct drm_property *max_luminance;
	struct drm_property *max_avg_luminance;
	struct drm_property *min_luminance;
	struct drm_property *hdr_formats;
	struct drm_property *lp_mode;
	struct drm_property *hbm_on;
	struct drm_property *brightness_capability;
	struct drm_property *brightness_level;
};

struct exynos_display_dsc {
	bool enabled;
	unsigned int dsc_count;
	unsigned int slice_count;
	unsigned int slice_height;
};

struct exynos_display_underrun_param {
	/* @te_idle_us: te idle (us) to calculate underrun_lp_ref */
	unsigned int te_idle_us;
	/* @te_var: te variation (percentage) to calculate underrun_lp_ref */
	unsigned int te_var;
	/* @max_vrefresh: refresh rate used to calculate underrun_lp_ref */
	unsigned int max_vrefresh;
};

/**
 * struct exynos_display_mode - exynos display specific info
 */
struct exynos_display_mode {
	/* @dsc: DSC parameters for the selected mode */
	struct exynos_display_dsc dsc;

	/* @mode_flags: DSI mode flags from drm_mipi_dsi.h */
	unsigned long mode_flags;

	/* @vblank_usec: command mode: TE pulse time, video mode: vbp+vfp time */
	unsigned int vblank_usec;

	/* @bpc: display bits per component */
	unsigned int bpc;

	/* @underrun_param: parameters to calculate underrun_lp_ref when hs_clock changes */
	const struct exynos_display_underrun_param *underrun_param;

	/* @is_lp_mode: boolean, if true it means this mode is a Low Power mode */
	bool is_lp_mode;

	/**
	 * @sw_trigger:
	 *
	 * Force frame transfer to be triggered by sw instead of based on TE.
	 * This is only applicable for DSI command mode, SW trigger is the
	 * default for Video mode.
	 */
	bool sw_trigger;
};

/**
 * struct exynos_drm_connector_state - mutable connector state
 */
struct exynos_drm_connector_state {
	/* @base: base connector state */
	struct drm_connector_state base;

	/* @mode: additional mode details */
	struct exynos_display_mode exynos_mode;

	/* @seamless_possible: this is set if the current mode switch can be done seamlessly */
	bool seamless_possible;

	/* @brightness_level: panel brightness level */
	unsigned int brightness_level;

	/* @hbm_on: hbm_on indicator */
	bool hbm_on;

	/*
	 * @te_from: Specify ddi interface where TE signals are received by decon.
	 *	     This is required for dsi command mode hw trigger.
	 */
	int te_from;

	/*
	 * @te_gpio: Provies the the gpio for panel TE signal.
	 *	     This is required for dsi command mode hw trigger.
	 */
	int te_gpio;
};

#define to_exynos_connector_state(connector_state) \
	container_of((connector_state), struct exynos_drm_connector_state, base)

struct exynos_drm_connector_funcs {
	void (*atomic_print_state)(struct drm_printer *p,
				   const struct exynos_drm_connector_state *state);
	int (*atomic_set_property)(struct exynos_drm_connector *exynos_connector,
				   struct exynos_drm_connector_state *exynos_state,
				   struct drm_property *property,
				   uint64_t val);
	int (*atomic_get_property)(struct exynos_drm_connector *exynos_connector,
				   const struct exynos_drm_connector_state *exynos_state,
				   struct drm_property *property,
				   uint64_t *val);
};

struct exynos_drm_connector_helper_funcs {
	void (*atomic_commit)(struct exynos_drm_connector *exynos_connector,
			      struct exynos_drm_connector_state *exynos_old_state,
			      struct exynos_drm_connector_state *exynos_new_state);
};

struct exynos_drm_connector {
	struct drm_connector base;
	const struct exynos_drm_connector_funcs *funcs;
	const struct exynos_drm_connector_helper_funcs *helper_private;
};

#define to_exynos_connector(connector) \
	container_of((connector), struct exynos_drm_connector, base)

bool is_exynos_drm_connector(const struct drm_connector *connector);
int exynos_drm_connector_init(struct drm_device *dev,
			      struct exynos_drm_connector *exynos_connector,
			      const struct exynos_drm_connector_funcs *funcs,
			      const struct exynos_drm_connector_helper_funcs *helper_funcs,
			      int connector_type);
int exynos_drm_connector_create_properties(struct drm_device *dev);
struct exynos_drm_connector_properties *
exynos_drm_connector_get_properties(struct exynos_drm_connector *exynos_conector);

static inline struct exynos_drm_connector_state *
crtc_get_exynos_connector_state(const struct drm_atomic_state *state,
				const struct drm_crtc_state *crtc_state)
{
	const struct drm_connector *conn;
	struct drm_connector_state *conn_state;
	int i;

	for_each_new_connector_in_state(state, conn, conn_state, i) {
		if (!(crtc_state->connector_mask & drm_connector_mask(conn)))
			continue;

		if (is_exynos_drm_connector(conn))
			return to_exynos_connector_state(conn_state);
	}

	return NULL;
}

#endif /* _EXYNOS_DRM_CONNECTOR_H_ */

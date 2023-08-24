/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#ifndef _GS_DRM_CONNECTOR_H_
#define _GS_DRM_CONNECTOR_H_

#include <drm/drm_atomic.h>
#include <drm/drm_connector.h>

#include "gs_drm/gs_display_mode.h"

#define MIN_WIN_BLOCK_WIDTH 8
#define MIN_WIN_BLOCK_HEIGHT 1

enum gs_hbm_mode {
	GS_HBM_OFF = 0,
	GS_HBM_ON_IRC_ON,
	GS_HBM_ON_IRC_OFF,
	GS_HBM_STATE_MAX,
};

enum gs_mipi_sync_mode {
	GS_MIPI_CMD_SYNC_NONE = BIT(0),
	GS_MIPI_CMD_SYNC_REFRESH_RATE = BIT(1),
	GS_MIPI_CMD_SYNC_LHBM = BIT(2),
	GS_MIPI_CMD_SYNC_GHBM = BIT(3),
	GS_MIPI_CMD_SYNC_BL = BIT(4),
};

struct gs_drm_connector;

struct gs_drm_connector_properties {
	struct drm_property *max_luminance;
	struct drm_property *max_avg_luminance;
	struct drm_property *min_luminance;
	struct drm_property *hdr_formats;
	struct drm_property *lp_mode;
	struct drm_property *global_hbm_mode;
	struct drm_property *local_hbm_on;
	struct drm_property *dimming_on;
	struct drm_property *brightness_capability;
	struct drm_property *brightness_level;
	struct drm_property *is_partial;
	struct drm_property *panel_idle_support;
	struct drm_property *mipi_sync;
	struct drm_property *panel_orientation;
	struct drm_property *vrr_switch_duration;
	struct drm_property *refresh_on_lp;
};

struct gs_display_partial {
	bool enabled;
	unsigned int min_width;
	unsigned int min_height;
};

/**
 * struct gs_drm_connector_state - mutable connector state
 */
struct gs_drm_connector_state {
	/** @base: base connector state */
	struct drm_connector_state base;

	/** @mode: additional mode details */
	struct gs_display_mode gs_mode;

	/** @seamless_possible: this is set if the current mode switch can be done seamlessly */
	bool seamless_possible;

	/** @brightness_level: panel brightness level */
	unsigned int brightness_level;

	/** @global_hbm_mode: global_hbm_mode indicator */
	enum gs_hbm_mode global_hbm_mode;

	/** @local_hbm_on: local_hbm_on indicator */
	bool local_hbm_on;

	/** @dimming_on: dimming on indicator */
	bool dimming_on;

	/** @pending_update_flags: flags for pending update */
	unsigned int pending_update_flags;

	/**
	 * @te_from: Specify ddi interface where TE signals are received by decon.
	 *	     This is required for dsi command mode hw trigger.
	 */
	int te_from;

	/**
	 * @te_gpio: Provies the gpio for panel TE signal.
	 *	     This is required for dsi command mode hw trigger.
	 */
	int te_gpio;

	/**
	 * @partial: Specify whether this panel supports partial update feature.
	 */
	struct gs_display_partial partial;

	/**
	 * @mipi_sync: Indicating if the mipi command in current drm commit should be
	 *	       sent in the same vsync period as the frame.
	 */
	unsigned long mipi_sync;

	/**
	 * @panel_idle_support: Indicating display support panel idle mode. Panel can
	 *			go into idle after some idle period.
	 */
	bool panel_idle_support;

	/**
	 * @blanked_mode: Display should go into forced blanked mode, where power is on but
	 *                nothing is being displayed on screen.
	 */
	bool blanked_mode;

	/** @is_recovering: whether we're doing decon recovery */
	bool is_recovering;
};

#define to_gs_connector_state(connector_state) \
	container_of((connector_state), struct gs_drm_connector_state, base)

struct gs_drm_connector_funcs {
	void (*atomic_print_state)(struct drm_printer *p,
				   const struct gs_drm_connector_state *state);
	int (*atomic_set_property)(struct gs_drm_connector *gs_connector,
				   struct gs_drm_connector_state *gs_state,
				   struct drm_property *property, uint64_t val);
	int (*atomic_get_property)(struct gs_drm_connector *gs_connector,
				   const struct gs_drm_connector_state *gs_state,
				   struct drm_property *property, uint64_t *val);
};

struct gs_drm_connector_helper_funcs {
	/*
	 * @atomic_pre_commit: Update connector states before planes commit.
	 *                     Usually for mipi commands and frame content synchronization.
	 */
	void (*atomic_pre_commit)(struct gs_drm_connector *gs_connector,
				  struct gs_drm_connector_state *gs_old_state,
				  struct gs_drm_connector_state *gs_new_state);

	/*
	 * @atomic_commit: Update connector states after planes commit.
	 */
	void (*atomic_commit)(struct gs_drm_connector *gs_connector,
			      struct gs_drm_connector_state *gs_old_state,
			      struct gs_drm_connector_state *gs_new_state);
};

/**
 * struct gs_drm_connector - private data for connector device
 */
struct gs_drm_connector {
	/** @base: base connector data */
	struct drm_connector base;
	/** @properties: drm properties associated with this connector */
	struct gs_drm_connector_properties properties;
	/** @funcs: functions used to interface with this connector */
	const struct gs_drm_connector_funcs *funcs;
	/** @helper_private: private helper functions for drm operations */
	const struct gs_drm_connector_helper_funcs *helper_private;
	/**
	 * @panel_dsi_device: pointer to the dsi device associated with the
	 * connected panel. Crucial for the `gs_connector_to_panel` function.
	 */
	struct mipi_dsi_device *panel_dsi_device;
	/**
	 * @dsi_host_device: pointer to dsi device hosting this connector
	 * Should be on the other end of the connector's DT graph
	 */
	struct mipi_dsi_host *dsi_host_device;
	/**
	 * @needs_commit: connector will always get atomic commit callback for any
	 * pipeline updates for as long as this flag is set
	 */
	bool needs_commit;
};

#define to_gs_connector(connector) container_of((connector), struct gs_drm_connector, base)

bool is_gs_drm_connector(const struct drm_connector *connector);
int gs_drm_connector_create_properties(struct drm_connector *connector);
struct gs_drm_connector_properties *
gs_drm_connector_get_properties(struct gs_drm_connector *gs_conector);

static inline struct gs_drm_connector_state *
crtc_get_gs_connector_state(const struct drm_atomic_state *state,
			    const struct drm_crtc_state *crtc_state)
{
	const struct drm_connector *conn;
	struct drm_connector_state *conn_state;
	int i;

	for_each_new_connector_in_state(state, conn, conn_state, i) {
		if (!(crtc_state->connector_mask & drm_connector_mask(conn)))
			continue;

		if (is_gs_drm_connector(conn))
			return to_gs_connector_state(conn_state);
	}

	return NULL;
}

int gs_connector_bind(struct device *dev, struct device *master, void *data);

void gs_connector_set_panel_name(const char *new_name, size_t len);

#endif /* _GS_DRM_CONNECTOR_H_ */

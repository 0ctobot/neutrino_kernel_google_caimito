/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "gs_panel/gs_panel.h"

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/version.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <video/mipi_display.h>

#include "gs_drm/gs_drm_connector.h"
#include "gs_panel_internal.h"

#define CREATE_TRACE_POINTS
#include <trace/panel_trace.h>

/* DEVICE TREE */

struct gs_drm_connector *get_gs_drm_connector_parent(const struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	struct device_node *panel_node = dev->of_node;
	struct device_node *parent_node;
	struct platform_device *parent_pdev;

	parent_node = of_get_parent(panel_node);
	if (!parent_node) {
		dev_warn(dev, "Unable to find parent node for device_node %p\n", panel_node);
		return NULL;
	}
	parent_pdev = of_find_device_by_node(parent_node);
	if (!parent_pdev) {
		dev_warn(dev, "Unable to find parent platform device for node %p\n", parent_node);
		of_node_put(parent_node);
		return NULL;
	}
	of_node_put(parent_node);
	return platform_get_drvdata(parent_pdev);
}

struct gs_panel *gs_connector_to_panel(const struct gs_drm_connector *gs_connector)
{
	if (!gs_connector->panel_dsi_device) {
		dev_err(gs_connector->base.kdev, "No panel_dsi_device associated with connector\n");
		return NULL;
	}
	return mipi_dsi_get_drvdata(gs_connector->panel_dsi_device);
}

static int gs_panel_parse_gpios(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	struct gs_panel_gpio *gpio = &ctx->gpio;

	dev_dbg(dev, "%s +\n", __func__);

	gpio->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_ASIS);
	if (gpio->reset_gpio == NULL) {
		dev_warn(dev, "no reset gpio found\n");
	} else if (IS_ERR(gpio->reset_gpio)) {
		dev_err(dev, "failed to get reset-gpios %ld\n", PTR_ERR(gpio->reset_gpio));
		return PTR_ERR(gpio->reset_gpio);
	}

	gpio->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (gpio->enable_gpio == NULL) {
		dev_dbg(dev, "no enable gpio found\n");
	} else if (IS_ERR(gpio->enable_gpio)) {
		dev_warn(dev, "failed to get enable-gpio %ld\n", PTR_ERR(gpio->enable_gpio));
		gpio->enable_gpio = NULL;
	}

	dev_dbg(dev, "%s -\n", __func__);
	return 0;
}

static int gs_panel_parse_regulator_or_null(struct device *dev,
					    struct regulator **regulator,
					    const char name[])
{
	*regulator = devm_regulator_get_optional(dev, name);
	if (IS_ERR(*regulator)) {
		if (PTR_ERR(*regulator) == -ENODEV) {
			dev_warn(dev, "no %s found for panel\n", name);
			*regulator = NULL;
		} else {
			dev_warn(dev, "failed to get panel %s (%pe).\n", name,
				 *regulator);
			return PTR_ERR(*regulator);
		}
	}
	return 0;
}

static int gs_panel_parse_regulators(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	struct gs_panel_regulator *gs_reg = &ctx->regulator;
	struct regulator *reg;
	int ret = 0;

	ret = gs_panel_parse_regulator_or_null(dev, &gs_reg->vddi, "vddi");
	if (ret)
		return ret;
	ret = gs_panel_parse_regulator_or_null(dev, &gs_reg->vci, "vci");
	if (ret)
		return ret;
	ret = gs_panel_parse_regulator_or_null(dev, &gs_reg->vddd, "vddd");
	if (ret)
		return ret;

	ret = of_property_read_u32(dev->of_node, "vddd-normal-microvolt", &gs_reg->vddd_normal_uV);
	if (ret)
		gs_reg->vddd_normal_uV = 0;

	ret = of_property_read_u32(dev->of_node, "vddd-lp-microvolt", &gs_reg->vddd_lp_uV);
	if (ret) {
		gs_reg->vddd_lp_uV = 0;
		if (gs_reg->vddd_normal_uV != 0) {
			pr_warn("ignore vddd normal %u\n", gs_reg->vddd_normal_uV);
			gs_reg->vddd_normal_uV = 0;
		}
	}

	reg = devm_regulator_get_optional(dev, "vddr_en");
	if (!PTR_ERR_OR_ZERO(reg)) {
		dev_dbg(dev, "panel vddr_en found\n");
		gs_reg->vddr_en = reg;
	}

	reg = devm_regulator_get_optional(dev, "vddr");
	if (!PTR_ERR_OR_ZERO(reg)) {
		dev_dbg(dev, "panel vddr found\n");
		gs_reg->vddr = reg;
	}

	return 0;
}

static int gs_panel_parse_dt(struct gs_panel *ctx)
{
	int ret = 0;
	u32 orientation = DRM_MODE_PANEL_ORIENTATION_NORMAL;

	if (IS_ERR_OR_NULL(ctx->dev->of_node)) {
		dev_err(ctx->dev, "no device tree information of gs panel\n");
		return -EINVAL;
	}

	ret = gs_panel_parse_gpios(ctx);
	if (ret)
		goto err;

	ret = gs_panel_parse_regulators(ctx);
	if (ret)
		goto err;

	ctx->touch_dev = of_parse_phandle(ctx->dev->of_node, "touch", 0);

	of_property_read_u32(ctx->dev->of_node, "orientation", &orientation);
	if (orientation > DRM_MODE_PANEL_ORIENTATION_RIGHT_UP) {
		dev_warn(ctx->dev, "invalid display orientation %d\n", orientation);
		orientation = DRM_MODE_PANEL_ORIENTATION_NORMAL;
	}
	ctx->orientation = orientation;

err:
	return ret;
}

#ifdef CONFIG_OF
static void devm_backlight_release(void *data)
{
	struct backlight_device *bd = data;

	if (bd)
		put_device(&bd->dev);
}
#endif

static int gs_panel_of_parse_backlight(struct gs_panel *ctx)
{
#ifdef CONFIG_OF
	struct device *dev;
	struct device_node *np;
	struct backlight_device *bd;
	int ret = 0;

	dev = ctx->base.dev;
	if (!dev)
		return -EINVAL;

	if (!dev->of_node)
		return 0;

	np = of_parse_phandle(dev->of_node, "backlight", 0);
	if (!np)
		return 0;

	bd = of_find_backlight_by_node(np);
	of_node_put(np);
	if (IS_ERR_OR_NULL(bd))
		return -EPROBE_DEFER;
	ctx->base.backlight = bd;
	ret = devm_add_action(dev, devm_backlight_release, bd);
	if (ret) {
		put_device(&bd->dev);
		return ret;
	}
	ctx->bl_ctrl_dcs = of_property_read_bool(dev->of_node, "bl-ctrl-dcs");
	dev_info(ctx->dev, "successfully registered devtree backlight phandle\n");
	return 0;
#else
	return 0;
#endif
}

/* Modes */

const struct gs_panel_mode *gs_panel_get_mode(struct gs_panel *ctx,
					      const struct drm_display_mode *mode)
{
	const struct gs_panel_mode *pmode;
	int i;

	if (ctx->desc->modes) {
		for (i = 0; i < ctx->desc->modes->num_modes; i++) {
			pmode = &ctx->desc->modes->modes[i];

			if (drm_mode_equal(&pmode->mode, mode))
				return pmode;
		}
	}

	if (ctx->desc->lp_modes) {
		pmode = &ctx->desc->lp_modes->modes[0];
		if (pmode) {
			const size_t count = ctx->desc->lp_modes->num_modes ?: 1;

			for (i = 0; i < count; i++, pmode++)
				if (drm_mode_equal(&pmode->mode, mode))
					return pmode;
		}
	}

	return NULL;
}

/* BACKLIGHT */

static int gs_get_brightness(struct backlight_device *bl)
{
	return bl->props.brightness;
}

u16 gs_panel_get_brightness(struct gs_panel *panel)
{
	return gs_get_brightness(panel->bl);
}
EXPORT_SYMBOL(gs_panel_get_brightness);

static int gs_update_status(struct backlight_device *bl)
{
	struct gs_panel *ctx = bl_get_data(bl);
	struct device *dev = ctx->dev;
	int brightness = bl->props.brightness;
	int min_brightness = ctx->desc->brightness_desc->min_brightness;
	if (min_brightness == 0)
		min_brightness = 1;

	if (!gs_is_panel_active(ctx)) {
		dev_dbg(dev, "panel is not enabled\n");
		return -EPERM;
	}

	/* check if backlight is forced off */
	if (bl->props.power != FB_BLANK_UNBLANK)
		brightness = 0;

	if (brightness && brightness < min_brightness)
		brightness = min_brightness;

	dev_info(dev, "req: %d, br: %d\n", bl->props.brightness, brightness);

	mutex_lock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
	if (ctx->base.backlight && !ctx->bl_ctrl_dcs) {
		dev_info(dev, "Setting brightness via backlight function\n");
		backlight_device_set_brightness(ctx->base.backlight, brightness);
	}
	/* TODO(tknelms): elif statement for custom set-brightness gs_func */
	else {
		dev_info(dev, "Setting brightness via dcs\n");
		gs_dcs_set_brightness(ctx, brightness);
	}

	mutex_unlock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
	return 0;
}

static const struct backlight_ops gs_backlight_ops = {
	.get_brightness = gs_get_brightness,
	.update_status = gs_update_status,
};

/* Connector Funcs */

static int gs_panel_attach_brightness_capability(struct gs_drm_connector *gs_conn,
						 const struct brightness_capability *brt_capability)
{
	struct gs_drm_connector_properties *p = gs_drm_connector_get_properties(gs_conn);
	struct drm_property_blob *blob;

	blob = drm_property_create_blob(gs_conn->base.dev, sizeof(struct brightness_capability),
					brt_capability);
	if (IS_ERR(blob))
		return PTR_ERR(blob);
	drm_object_attach_property(&gs_conn->base.base, p->brightness_capability, blob->base.id);

	return 0;
}

static int gs_panel_connector_attach_properties(struct gs_panel *ctx)
{
	struct gs_drm_connector_properties *p = gs_drm_connector_get_properties(ctx->gs_connector);
	struct drm_mode_object *obj = &ctx->gs_connector->base.base;
	const struct gs_panel_desc *desc = ctx->desc;
	int ret = 0;

	if (!p || !desc)
		return -ENOENT;

	dev_dbg(ctx->dev, "%s+\n", __func__);

	drm_object_attach_property(obj, p->min_luminance, desc->brightness_desc->min_luminance);
	drm_object_attach_property(obj, p->max_luminance, desc->brightness_desc->max_luminance);
	drm_object_attach_property(obj, p->max_avg_luminance,
				   desc->brightness_desc->max_avg_luminance);
	drm_object_attach_property(obj, p->hdr_formats, desc->hdr_formats);
	drm_object_attach_property(obj, p->brightness_level, 0);
	drm_object_attach_property(obj, p->global_hbm_mode, 0);
	drm_object_attach_property(obj, p->local_hbm_on, 0);
	drm_object_attach_property(obj, p->dimming_on, 0);
	drm_object_attach_property(obj, p->mipi_sync, 0);
	drm_object_attach_property(obj, p->is_partial, desc->is_partial);
	drm_object_attach_property(obj, p->panel_idle_support, desc->is_idle_supported);
	drm_object_attach_property(obj, p->panel_orientation, ctx->orientation);
	drm_object_attach_property(obj, p->vrr_switch_duration, desc->vrr_switch_duration);

	if (desc->brightness_desc->brt_capability) {
		ret = gs_panel_attach_brightness_capability(ctx->gs_connector,
							    desc->brightness_desc->brt_capability);
		if (ret)
			dev_err(ctx->dev, "Failed to attach brightness capability (%d)\n", ret);
	}

	if (desc->lp_modes && desc->lp_modes->num_modes > 0)
		drm_object_attach_property(obj, p->lp_mode, 0);

	dev_dbg(ctx->dev, "%s-\n", __func__);

	return ret;
}

/* Regulators */

static int _gs_panel_reg_ctrl(struct gs_panel *ctx, const struct panel_reg_ctrl *reg_ctrl,
			      bool enable)
{
	struct regulator *panel_reg[PANEL_REG_ID_MAX] = {
		[PANEL_REG_ID_VCI] = ctx->regulator.vci,
		[PANEL_REG_ID_VDDD] = ctx->regulator.vddd,
		[PANEL_REG_ID_VDDI] = ctx->regulator.vddi,
		[PANEL_REG_ID_VDDR_EN] = ctx->regulator.vddr_en,
		[PANEL_REG_ID_VDDR] = ctx->regulator.vddr,
	};
	u32 i;

	for (i = 0; i < PANEL_REG_COUNT; i++) {
		enum panel_reg_id id = reg_ctrl[i].id;
		u32 delay_ms = reg_ctrl[i].post_delay_ms;
		int ret;
		struct regulator *reg;

		if (!IS_VALID_PANEL_REG_ID(id))
			return 0;

		reg = panel_reg[id];
		if (!reg) {
			dev_dbg(ctx->dev, "no valid regulator found id=%d\n", id);
			continue;
		}
		ret = enable ? regulator_enable(reg) : regulator_disable(reg);
		if (ret) {
			dev_err(ctx->dev, "failed to %s regulator id=%d\n",
				enable ? "enable" : "disable", id);
			return ret;
		}

		if (delay_ms)
			usleep_range(delay_ms * 1000, delay_ms * 1000 + 10);
		dev_dbg(ctx->dev, "%s regulator id=%d with post_delay=%d ms\n",
			enable ? "enable" : "disable", id, delay_ms);
	}
	return 0;
}

static void gs_panel_pre_power_off(struct gs_panel *ctx)
{
	int ret;

	if (!IS_VALID_PANEL_REG_ID(ctx->desc->reg_ctrl_desc->reg_ctrl_pre_disable[0].id))
		return;

	ret = _gs_panel_reg_ctrl(ctx, ctx->desc->reg_ctrl_desc->reg_ctrl_pre_disable, false);
	if (ret)
		dev_err(ctx->dev, "failed to set pre power off: ret %d\n", ret);
	else
		dev_dbg(ctx->dev, "set pre power off\n");
}

static int _gs_panel_set_power(struct gs_panel *ctx, bool on)
{
	const struct panel_reg_ctrl default_ctrl_disable[PANEL_REG_COUNT] = {
		{ PANEL_REG_ID_VDDR, 0 },
		{ PANEL_REG_ID_VDDR_EN, 0 },
		{ PANEL_REG_ID_VDDD, 0 },
		{ PANEL_REG_ID_VDDI, 0 },
		{ PANEL_REG_ID_VCI, 0 },
	};
	const struct panel_reg_ctrl default_ctrl_enable[PANEL_REG_COUNT] = {
		{ PANEL_REG_ID_VDDI, 5 },
		{ PANEL_REG_ID_VDDD, 0 },
		{ PANEL_REG_ID_VCI, 0 },
		{ PANEL_REG_ID_VDDR_EN, 2 },
		{ PANEL_REG_ID_VDDR, 0 },
	};
	const struct panel_reg_ctrl *reg_ctrl;

	if (on) {
		if (!IS_ERR_OR_NULL(ctx->gpio.enable_gpio)) {
			gpiod_set_value(ctx->gpio.enable_gpio, 1);
			usleep_range(10000, 11000);
		}
		if (ctx->desc->reg_ctrl_desc &&
		    IS_VALID_PANEL_REG_ID(ctx->desc->reg_ctrl_desc->reg_ctrl_enable[0].id)) {
			reg_ctrl = ctx->desc->reg_ctrl_desc->reg_ctrl_enable;
		} else {
			reg_ctrl = default_ctrl_enable;
		}
	} else {
		gs_panel_pre_power_off(ctx);
		if (!IS_ERR_OR_NULL(ctx->gpio.reset_gpio))
			gpiod_set_value(ctx->gpio.reset_gpio, 0);
		if (!IS_ERR_OR_NULL(ctx->gpio.enable_gpio))
			gpiod_set_value(ctx->gpio.enable_gpio, 0);
		if (ctx->desc->reg_ctrl_desc &&
		    IS_VALID_PANEL_REG_ID(ctx->desc->reg_ctrl_desc->reg_ctrl_disable[0].id)) {
			reg_ctrl = ctx->desc->reg_ctrl_desc->reg_ctrl_disable;
		} else {
			reg_ctrl = default_ctrl_disable;
		}
	}

	return _gs_panel_reg_ctrl(ctx, reg_ctrl, on);
}

int gs_panel_set_power_helper(struct gs_panel *ctx, bool on)
{
	int ret;

	ret = _gs_panel_set_power(ctx, on);

	if (ret) {
		dev_err(ctx->dev, "failed to set power: ret %d\n", ret);
		return ret;
	}

	ctx->bl->props.power = on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN;

	return 0;
}
EXPORT_SYMBOL(gs_panel_set_power_helper);

static void _gs_panel_set_vddd_voltage(struct gs_panel *ctx, bool is_lp)
{
	u32 uv = is_lp ? ctx->regulator.vddd_lp_uV : ctx->regulator.vddd_normal_uV;
	if (!uv || !ctx->regulator.vddd)
		return;
	if (regulator_set_voltage(ctx->regulator.vddd, uv, uv))
		dev_err(ctx->dev, "failed to set vddd at %u uV\n", uv);
}

int gs_panel_initialize_gs_connector(struct gs_panel *ctx, struct drm_device *drm_dev,
				     struct gs_drm_connector *gs_connector)
{
	struct device *dev = ctx->dev;
	struct drm_connector *connector = &gs_connector->base;
	int ret = 0;

	/* Initialize drm_connector */
	if (!gs_connector->base.funcs) {
		gs_connector_bind(gs_connector->base.kdev, NULL, drm_dev);
	}
	ret = drm_connector_init(drm_dev, connector, gs_connector->base.funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret) {
		dev_err(dev, "Error initializing drm_connector (%d)\n", ret);
		return ret;
	}

	/* Attach functions */
	gs_connector->funcs = get_panel_gs_drm_connector_funcs();
	gs_connector->helper_private = get_panel_gs_drm_connector_helper_funcs();
	drm_connector_helper_add(connector, get_panel_drm_connector_helper_funcs());

	/* Attach properties */
	ret = gs_panel_connector_attach_properties(ctx);
	if (ret) {
		dev_err(dev, "Error attaching connector properties (%d)\n", ret);
		return ret;
	}

	/* Register */
	ret = drm_connector_register(connector);
	if (ret) {
		dev_err(dev, "Error registering drm_connector (%d)\n", ret);
		return ret;
	}

	return 0;
}

/* INITIALIZATION */

int gs_panel_first_enable(struct gs_panel *ctx)
{
	const struct gs_panel_funcs *funcs = ctx->desc->gs_panel_func;
	struct device *dev = ctx->dev;
	int ret = 0;

	if (ctx->initialized) {
		dev_info(dev, "%s called but ctx not initialized\n", __func__);
		return 0;
	}

	/*TODO(tknelms)
	ret = gs_panel_read_extinfo(ctx);
	if (!ret)
		ctx->initialized = true;
	*/

	/*TODO(tknelms)
	if (funcs && funcs->get_panel_rev) {
		u32 id;

		if (kstrtou32(ctx->panel_extinfo, 16, &id)) {
			dev_warn(ctx->dev,
				"failed to get panel extinfo, "
				"default to latest\n");
			ctx->panel_rev = PANEL_REV_LATEST;
		} else {
			funcs->get_panel_rev(ctx, id);
		}
	} else {
	*/
	dev_warn(dev, "unable to get panel rev, default to latest\n");
	ctx->panel_rev = PANEL_REV_LATEST;

	/*TODO(tknelms)
	if (funcs && funcs->read_id)
		ret = funcs->read_id(ctx);
	else
		ret = gs_panel_read_id(ctx);
	if (ret)
		return ret;
	*/

	if (funcs && funcs->panel_init)
		funcs->panel_init(ctx);

	return ret;
}

static void gs_panel_post_power_on(struct gs_panel *ctx)
{
	int ret;

	if (!IS_VALID_PANEL_REG_ID(ctx->desc->reg_ctrl_desc->reg_ctrl_post_enable[0].id))
		return;

	ret = _gs_panel_reg_ctrl(ctx, ctx->desc->reg_ctrl_desc->reg_ctrl_post_enable, true);
	if (ret)
		dev_err(ctx->dev, "failed to set post power on: ret %d\n", ret);
	else
		dev_dbg(ctx->dev, "set post power on\n");
}

static void gs_panel_handoff(struct gs_panel *ctx)
{
	bool enabled = gpiod_get_raw_value(ctx->gpio.reset_gpio) > 0;
	_gs_panel_set_vddd_voltage(ctx, false);
	if (enabled) {
		dev_info(ctx->dev, "panel enabled at boot\n");
		ctx->panel_state = GPANEL_STATE_HANDOFF;
		gs_panel_set_power_helper(ctx, true);
		gs_panel_post_power_on(ctx);
	} else {
		ctx->panel_state = GPANEL_STATE_UNINITIALIZED;
		gpiod_direction_output(ctx->gpio.reset_gpio, 0);
	}

	if (ctx->desc && ctx->desc->modes && ctx->desc->modes->num_modes > 0 &&
	    ctx->panel_state == GPANEL_STATE_HANDOFF) {
		int i;
		for (i = 0; i < ctx->desc->modes->num_modes; i++) {
			const struct gs_panel_mode *pmode;

			pmode = &ctx->desc->modes->modes[i];
			if (pmode->mode.type & DRM_MODE_TYPE_PREFERRED) {
				ctx->current_mode = pmode;
				break;
			}
		}
		if (ctx->current_mode == NULL) {
			ctx->current_mode = &ctx->desc->modes->modes[0];
			i = 0;
		}
		dev_dbg(ctx->dev, "set default panel mode[%d]: %s\n", i,
			ctx->current_mode->mode.name[0] ? ctx->current_mode->mode.name : "NA");
	}
}

static int gs_panel_init_backlight(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	char name[32];
	static atomic_t panel_index = ATOMIC_INIT(-1);

	/* Backlight */
	scnprintf(name, sizeof(name), "panel%d-backlight", atomic_inc_return(&panel_index));
	ctx->bl = devm_backlight_device_register(dev, name, dev, ctx, &gs_backlight_ops, NULL);
	if (IS_ERR(ctx->bl)) {
		dev_err(dev, "failed to register backlight device\n");
		return PTR_ERR(ctx->bl);
	}

	ctx->bl->props.max_brightness = ctx->desc->brightness_desc->max_brightness;
	ctx->bl->props.brightness = ctx->desc->brightness_desc->default_brightness;

	return 0;
}

int gs_panel_common_init(struct mipi_dsi_device *dsi, struct gs_panel *ctx)
{
	struct device *dev = &dsi->dev;
	int ret = 0;

	dev_dbg(dev, "%s +\n", __func__);

	/* Attach descriptive panel data to driver data structure */
	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dev = dev;
	ctx->desc = of_device_get_match_data(dev);

	/* Set DSI data */
	dsi->lanes = ctx->desc->data_lane_cnt;
	dsi->format = MIPI_DSI_FMT_RGB888;

	/* Connector */
	ctx->gs_connector = get_gs_drm_connector_parent(ctx);

	/* Register connector as bridge */
#ifdef CONFIG_OF
	ctx->bridge.of_node = ctx->gs_connector->base.kdev->of_node;
#endif
	drm_bridge_add(&ctx->bridge);

	/* Parse device tree */
	ret = gs_panel_parse_dt(ctx);
	if (ret) {
		dev_err(dev, "Error parsing device tree (%d), exiting init\n", ret);
		return ret;
	}

	ret = gs_panel_init_backlight(ctx);
	if (ret)
		return ret;

	/* HBM */
	/*TODO(tknelms): hbm*/

	/* Vrefresh */
	if (ctx->desc->modes) {
		size_t i;
		for (i = 0; i < ctx->desc->modes->num_modes; i++) {
			const struct gs_panel_mode *pmode = &ctx->desc->modes->modes[i];
			const int vrefresh = drm_mode_vrefresh(&pmode->mode);
			if (ctx->peak_vrefresh < vrefresh)
				ctx->peak_vrefresh = vrefresh;
		}
	}

	/* Idle work */
	/* TODO(tknelms): idle work */
	ctx->idle_data.panel_idle_enabled = false;

	/* Initialize mutexes */
	/*TODO(b/267170999): all*/
	mutex_init(&ctx->mode_lock);
	mutex_init(&ctx->bl_state_lock);
	mutex_init(&ctx->lp_state_lock);

	/* Initialize panel */
	drm_panel_init(&ctx->base, dev, ctx->desc->panel_func, DRM_MODE_CONNECTOR_DSI);

	/* Add the panel officially */
	drm_panel_add(&ctx->base);

	/* Parse device tree - Backlight */
	ret = gs_panel_of_parse_backlight(ctx);
	if (ret) {
		dev_err(dev, "failed to register devtree backlight (%d)\n", ret);
		goto err_panel;
	}

	/* Attach bridge funcs */
	ctx->bridge.funcs = get_panel_drm_bridge_funcs();

	/* Create sysfs files */
	ret = gs_panel_sysfs_create_files(dev);
	if (ret)
		dev_warn(dev, "unable to add panel sysfs failes (%d)\n", ret);
	/* TODO(tknelms): bl_device_groups
	ret = sysfs_create_groups(&ctx->bl->dev.kobj, bl_device_groups);
	if (ret)
		dev_err(dev, "unable to create bl_device_groups groups\n");
	*/

	/* TODO(tknelms): cabc_mode
	if (ctx->desc->gs_panel_func && ctx->desc->gs_panel_func->base &&
	    ctx->desc->gs_panel_func->base->set_cabc_mode) {
		ret = sysfs_create_file(&ctx->bl->dev.kobj, *dev_attr_cabc_mode.attr);
		if (ret)
			dev_err(dev, "unable to create cabc_mode\n");
	}
	*/

	/* panel handoff */
	gs_panel_handoff(ctx);

	/* dsi attach */
	ret = mipi_dsi_attach(dsi);
	if (ret)
		goto err_panel;

	dev_info(dev, "gs common panel driver has been probed; dsi %s\n", dsi->name);
	dev_dbg(dev, "%s -\n", __func__);
	return 0;

err_panel:
	drm_panel_remove(&ctx->base);
	dev_err(dev, "failed to probe gs common panel driver (%d)\n", ret);

	return ret;
}
EXPORT_SYMBOL(gs_panel_common_init);

/* DRM panel funcs */

void gs_panel_reset_helper(struct gs_panel *ctx)
{
	u32 delay;
	const u32 *timing_ms = ctx->desc->reset_timing_ms;

	dev_dbg(ctx->dev, "%s +\n", __func__);

	if (IS_ERR_OR_NULL(ctx->gpio.reset_gpio)) {
		dev_dbg(ctx->dev, "%s -(no reset gpio)\n", __func__);
		return;
	}

	gpiod_set_value(ctx->gpio.reset_gpio, 1);
	delay = timing_ms[PANEL_RESET_TIMING_HIGH] ?: 5;
	delay *= 1000;
	usleep_range(delay, delay + 10);

	gpiod_set_value(ctx->gpio.reset_gpio, 0);
	delay = timing_ms[PANEL_RESET_TIMING_LOW] ?: 5;
	delay *= 1000;
	usleep_range(delay, delay + 10);

	gpiod_set_value(ctx->gpio.reset_gpio, 1);
	delay = timing_ms[PANEL_RESET_TIMING_INIT] ?: 10;
	delay *= 1000;
	usleep_range(delay, delay + 10);

	dev_dbg(ctx->dev, "%s -\n", __func__);

	gs_panel_first_enable(ctx);

	gs_panel_post_power_on(ctx);
}
EXPORT_SYMBOL(gs_panel_reset_helper);

/* Tracing */

void gs_panel_msleep(u32 delay_ms)
{
	trace_msleep(delay_ms);
}
EXPORT_SYMBOL(gs_panel_msleep);

MODULE_AUTHOR("Taylor Nelms <tknelms@google.com>");
MODULE_DESCRIPTION("MIPI-DSI panel driver abstraction for use across panel vendors");
MODULE_LICENSE("Dual MIT/GPL");

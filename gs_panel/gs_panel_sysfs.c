/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "gs_panel_internal.h"

#include <linux/delay.h>
#include <linux/sysfs.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_vblank.h>

#include "gs_panel/gs_panel.h"

/* Sysfs Node */

static ssize_t serial_number_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	const struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);

	if (!ctx->initialized)
		return -EPERM;

	if (!strcmp(ctx->panel_id, ""))
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", ctx->panel_id);
}

static ssize_t panel_extinfo_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	const struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);

	if (!ctx->initialized)
		return -EPERM;

	return sysfs_emit(buf, "%s\n", ctx->panel_extinfo);
}

static ssize_t panel_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	const char *p;

	/* filter priority info in the dsi device name */
	p = strstr(dsi->name, ":");
	if (!p)
		p = dsi->name;
	else
		p++;

	return sysfs_emit(buf, "%s\n", p);
}

static ssize_t panel_model_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	const struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);

	return sysfs_emit(buf, "%s\n", ctx->panel_model);
}

static ssize_t panel_idle_store(struct device *dev, struct device_attribute *attr, const char *buf,
				size_t count)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);
	bool idle_enabled;
	int ret;

	ret = kstrtobool(buf, &idle_enabled);
	if (ret) {
		dev_err(dev, "invalid panel idle value\n");
		return ret;
	}

	mutex_lock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
	if (idle_enabled != ctx->idle_data.panel_idle_enabled) {
		ctx->idle_data.panel_idle_enabled = idle_enabled;

		if (idle_enabled)
			ctx->timestamps.last_panel_idle_set_ts = ktime_get();

		panel_update_idle_mode_locked(ctx, true);
	}
	mutex_unlock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/

	return count;
}

static ssize_t panel_idle_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);

	return sysfs_emit(buf, "%d\n", ctx->idle_data.panel_idle_enabled);
}

static ssize_t panel_need_handle_idle_exit_store(struct device *dev, struct device_attribute *attr,
						 const char *buf, size_t count)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);
	bool idle_handle_exit;
	int ret;

	ret = kstrtobool(buf, &idle_handle_exit);
	if (ret) {
		dev_err(dev, "invalid panel idle handle exit value\n");
		return ret;
	}

	mutex_lock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
	ctx->idle_data.panel_need_handle_idle_exit = idle_handle_exit;
	mutex_unlock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/

	return count;
}

static ssize_t panel_need_handle_idle_exit_show(struct device *dev, struct device_attribute *attr,
						char *buf)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);

	return sysfs_emit(buf, "%d\n", ctx->idle_data.panel_need_handle_idle_exit);
}

static ssize_t idle_delay_ms_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);
	u32 idle_delay_ms;
	int ret;

	ret = kstrtou32(buf, 0, &idle_delay_ms);
	if (ret) {
		dev_err(dev, "invalid idle delay ms\n");
		return ret;
	}

	mutex_lock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
	if (ctx->idle_data.idle_delay_ms != idle_delay_ms) {
		ctx->idle_data.idle_delay_ms = idle_delay_ms;
		panel_update_idle_mode_locked(ctx, true);
	}
	mutex_unlock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/

	return count;
}
static ssize_t idle_delay_ms_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);

	return sysfs_emit(buf, "%d\n", ctx->idle_data.idle_delay_ms);
}

static ssize_t op_hz_store(struct device *dev, struct device_attribute *attr, const char *buf,
			   size_t count)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);
	ssize_t ret;
	u32 hz;

	if (!count)
		return -EINVAL;

	ret = kstrtou32(buf, 0, &hz);
	if (ret) {
		dev_err(ctx->dev, "invalid op_hz value\n");
		return ret;
	}

	ret = gs_panel_set_op_hz(ctx, hz);
	if (ret)
		return ret;

	return count;
}

static ssize_t op_hz_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);

	if (!gs_is_panel_initialized(ctx))
		return -EAGAIN;

	if (!gs_panel_has_func(ctx, set_op_hz))
		return -EINVAL;

	return sysfs_emit(buf, "%u\n", ctx->op_hz);
}

static ssize_t refresh_rate_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);
	const struct gs_panel_mode *current_mode;
	int rr = -1;

	mutex_lock(&ctx->mode_lock);
	current_mode = ctx->current_mode;
	if (current_mode != NULL)
		rr = drm_mode_vrefresh(&current_mode->mode);
	mutex_unlock(&ctx->mode_lock);

	return sysfs_emit(buf, "%d\n", rr);
}

static ssize_t refresh_ctrl_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);
	ssize_t ret;
	u32 ctrl;
	bool auto_fi;
	bool onetime_fi;
	bool repeated_fi;

	if (!count)
		return -EINVAL;

	if (!gs_panel_has_func(ctx, refresh_ctrl))
		return -EINVAL;

	ret = kstrtou32(buf, 0, &ctrl);
	if (ret) {
		dev_err(ctx->dev, "%s: failed to parse input\n", __func__);
		return -EINVAL;
	}

	auto_fi = ctrl & GS_PANEL_REFRESH_CTRL_FI_AUTO;
	onetime_fi = ctrl & GS_PANEL_REFRESH_CTRL_FI_FRAME_COUNT_MASK;
	repeated_fi = ctrl & GS_PANEL_REFRESH_CTRL_FI_REFRESH_RATE_MASK;
	if ((auto_fi && (onetime_fi || repeated_fi)) || (onetime_fi && repeated_fi)) {
		dev_err(ctx->dev, "%s: invalid command combination: 0x%X\n", __func__, ctrl);
		return -EINVAL;
	}

	mutex_lock(&ctx->mode_lock);
	ctx->refresh_ctrl = ctrl;
	if (!gs_is_panel_initialized(ctx) || !gs_is_panel_enabled(ctx))
		dev_info(dev, "%s: cache ctrl=0x%08lX\n", __func__,
			 ctrl & GS_PANEL_REFRESH_CTRL_FEATURE_MASK);
	else
		ctx->desc->gs_panel_func->refresh_ctrl(ctx);
	ctx->refresh_ctrl &= GS_PANEL_REFRESH_CTRL_FEATURE_MASK;
	mutex_unlock(&ctx->mode_lock);

	return count;
}

static ssize_t refresh_ctrl_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);

	return sysfs_emit(buf, "%s\n", gs_panel_has_func(ctx, refresh_ctrl) ? "Enabled" :
									      "Disabled");
}

static ssize_t min_vrefresh_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int min_vrefresh;
	int ret;

	ret = kstrtoint(buf, 0, &min_vrefresh);
	if (ret) {
		dev_err(dev, "invalid min vrefresh value\n");
		return ret;
	}

	mutex_lock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
	if (ctx->min_vrefresh != min_vrefresh) {
		ctx->min_vrefresh = min_vrefresh;
		panel_update_idle_mode_locked(ctx, true);
	}
	mutex_unlock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/

	return count;
}

static ssize_t min_vrefresh_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);

	return sysfs_emit(buf, "%d\n", ctx->min_vrefresh);
}

/**
 * gs_get_te2_timing() - Outputs te2 timingss to sysfs
 * @ctx: panel struct
 * @buf: output buffer for human-readable te2 data
 * @lp_mode: whether these timings apply to LP modes
 *
 * Return: number of bytes written to buffer
 */
static ssize_t gs_get_te2_timing(struct gs_panel *ctx, char *buf, bool lp_mode)
{
	size_t len;

	if (!gs_panel_has_func(ctx, get_te2_edges))
		return -EPERM;

	mutex_lock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
	len = ctx->desc->gs_panel_func->get_te2_edges(ctx, buf, lp_mode);
	mutex_unlock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/

	return len;
}

static ssize_t te2_timing_store(struct device *dev, struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);
	ssize_t ret;

	if (!gs_is_panel_initialized(ctx))
		return -EAGAIN;

	ret = gs_set_te2_timing(ctx, count, buf, false);
	if (ret < 0)
		dev_err(ctx->dev, "failed to set normal mode TE2 timing: ret %ld\n", ret);

	return ret;
}

static ssize_t te2_timing_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);
	ssize_t ret;

	if (!gs_is_panel_initialized(ctx))
		return -EAGAIN;

	ret = gs_get_te2_timing(ctx, buf, false);
	if (ret < 0)
		dev_err(ctx->dev, "failed to get normal mode TE2 timing: ret %ld\n", ret);

	return ret;
}

static ssize_t te2_lp_timing_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);
	ssize_t ret;

	if (!gs_is_panel_initialized(ctx))
		return -EAGAIN;

	ret = gs_set_te2_timing(ctx, count, buf, true);
	if (ret < 0)
		dev_err(ctx->dev, "failed to set LP mode TE2 timing: ret %ld\n", ret);

	return ret;
}

static ssize_t te2_lp_timing_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);
	ssize_t ret;

	if (!gs_is_panel_initialized(ctx))
		return -EAGAIN;

	ret = gs_get_te2_timing(ctx, buf, true);
	if (ret < 0)
		dev_err(ctx->dev, "failed to get LP mode TE2 timing: ret %ld\n", ret);

	return ret;
}

static DEVICE_ATTR_RO(serial_number);
static DEVICE_ATTR_RO(panel_extinfo);
static DEVICE_ATTR_RO(panel_name);
static DEVICE_ATTR_RO(panel_model);
static DEVICE_ATTR_RW(panel_idle);
static DEVICE_ATTR_RW(panel_need_handle_idle_exit);
static DEVICE_ATTR_RW(idle_delay_ms);
static DEVICE_ATTR_RW(op_hz);
static DEVICE_ATTR_RO(refresh_rate);
static DEVICE_ATTR_RW(refresh_ctrl);
static DEVICE_ATTR_RW(min_vrefresh);
static DEVICE_ATTR_RW(te2_timing);
static DEVICE_ATTR_RW(te2_lp_timing);
/* TODO(tknelms): re-implement below */
#if 0
static DEVICE_ATTR_WO(gamma);
static DEVICE_ATTR_RW(force_power_on);
static DEVICE_ATTR_RW(osc2_clk_khz);
static DEVICE_ATTR_RO(available_osc2_clk_khz);
#endif

static const struct attribute *panel_attrs[] = {
	&dev_attr_serial_number.attr,
	&dev_attr_panel_extinfo.attr,
	&dev_attr_panel_name.attr,
	&dev_attr_panel_model.attr,
	&dev_attr_panel_idle.attr,
	&dev_attr_panel_need_handle_idle_exit.attr,
	&dev_attr_idle_delay_ms.attr,
	&dev_attr_op_hz.attr,
	&dev_attr_refresh_rate.attr,
	&dev_attr_refresh_ctrl.attr,
	&dev_attr_min_vrefresh.attr,
	&dev_attr_te2_timing.attr,
	&dev_attr_te2_lp_timing.attr,
/* TODO(tknelms): re-implement below */
#if 0
	&dev_attr_gamma.attr,
	&dev_attr_force_power_on.attr,
	&dev_attr_osc2_clk_khz.attr,
	&dev_attr_available_osc2_clk_khz.attr,
#endif
	NULL
};

int gs_panel_sysfs_create_files(struct device *dev)
{
	return sysfs_create_files(&dev->kobj, panel_attrs);
}

/* Backlight Sysfs Node */

static ssize_t hbm_mode_store(struct device *dev, struct device_attribute *attr, const char *buf,
			      size_t count)
{
	struct backlight_device *bd = to_backlight_device(dev);
	struct gs_panel *ctx = bl_get_data(bd);
	const struct gs_panel_mode *pmode;
	u32 hbm_mode;
	int ret;

	if (!gs_panel_has_func(ctx, set_hbm_mode)) {
		dev_err(ctx->dev, "HBM is not supported\n");
		return -ENOTSUPP;
	}

	mutex_lock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
	pmode = ctx->current_mode;

	if (!gs_is_panel_active(ctx) || !pmode) {
		dev_err(ctx->dev, "panel is not enabled\n");
		ret = -EPERM;
		goto unlock;
	}

	if (pmode->gs_mode.is_lp_mode) {
		dev_dbg(ctx->dev, "hbm unsupported in LP mode\n");
		ret = -EPERM;
		goto unlock;
	}

	ret = kstrtouint(buf, 0, &hbm_mode);
	if (ret || (hbm_mode >= GS_HBM_STATE_MAX)) {
		dev_err(ctx->dev, "invalid hbm_mode value\n");
		goto unlock;
	}

	if (hbm_mode != ctx->hbm_mode) {
		ctx->desc->gs_panel_func->set_hbm_mode(ctx, hbm_mode);
		schedule_work(&ctx->state_notify);
	}

unlock:
	mutex_unlock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/

	return ret ? ret : count;
}

static ssize_t hbm_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct backlight_device *bd = to_backlight_device(dev);
	struct gs_panel *ctx = bl_get_data(bd);

	return sysfs_emit(buf, "%u\n", ctx->hbm_mode);
}

static ssize_t dimming_on_store(struct device *dev, struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct backlight_device *bd = to_backlight_device(dev);
	struct gs_panel *ctx = bl_get_data(bd);
	bool dimming_on;
	int ret;

	if (!gs_is_panel_active(ctx)) {
		dev_err(ctx->dev, "panel is not enabled\n");
		return -EPERM;
	}

	ret = kstrtobool(buf, &dimming_on);
	if (ret) {
		dev_err(ctx->dev, "invalid dimming_on value\n");
		return ret;
	}

	gs_panel_set_dimming(ctx, dimming_on);

	return count;
}

static ssize_t dimming_on_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct backlight_device *bd = to_backlight_device(dev);
	struct gs_panel *ctx = bl_get_data(bd);

	return sysfs_emit(buf, "%d\n", ctx->dimming_on);
}

static ssize_t local_hbm_mode_store(struct device *dev, struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct backlight_device *bd = to_backlight_device(dev);
	struct gs_panel *ctx = bl_get_data(bd);
	bool local_hbm_en;
	int ret;
	struct drm_crtc *crtc = get_gs_panel_connector_crtc(ctx);

	if (!gs_is_panel_active(ctx)) {
		dev_err(ctx->dev, "panel is not enabled\n");
		return -EPERM;
	}

	if (!gs_panel_has_func(ctx, set_local_hbm_mode)) {
		dev_err(ctx->dev, "Local HBM is not supported\n");
		return -ENOTSUPP;
	}

	ret = kstrtobool(buf, &local_hbm_en);
	if (ret) {
		dev_err(ctx->dev, "invalid local_hbm_mode value\n");
		return ret;
	}

	if (crtc && !drm_crtc_vblank_get(crtc)) {
		struct drm_vblank_crtc vblank = crtc->dev->vblank[crtc->index];
		u32 delay_us = vblank.framedur_ns / 2000;

		drm_crtc_wait_one_vblank(crtc);
		drm_crtc_vblank_put(crtc);
		/* wait for 0.5 frame to send to ensure it is done in one frame */
		usleep_range(delay_us, delay_us + 10);
	}

	dev_info(ctx->dev, "%s: set LHBM to %d\n", __func__, local_hbm_en);
	mutex_lock(&ctx->mode_lock); /* TODO(b/267170999): MODE */
	ctx->lhbm.requested_state = local_hbm_en;
	panel_update_lhbm(ctx);
	mutex_unlock(&ctx->mode_lock); /* TODO(b/267170999): MODE */

	return count;
}

static ssize_t local_hbm_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct backlight_device *bd = to_backlight_device(dev);
	struct gs_panel *ctx = bl_get_data(bd);

	return sysfs_emit(buf, "%d\n", ctx->lhbm.effective_state);
}

static ssize_t local_hbm_max_timeout_store(struct device *dev, struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct backlight_device *bd = to_backlight_device(dev);
	struct gs_panel *ctx = bl_get_data(bd);
	int ret;

	ret = kstrtou32(buf, 0, &ctx->lhbm.max_timeout_ms);
	if (ret) {
		dev_err(ctx->dev, "invalid local_hbm_max_timeout_ms value\n");
		return ret;
	}

	return count;
}

static ssize_t local_hbm_max_timeout_show(struct device *dev, struct device_attribute *attr,
					  char *buf)
{
	struct backlight_device *bd = to_backlight_device(dev);
	struct gs_panel *ctx = bl_get_data(bd);

	return sysfs_emit(buf, "%d\n", ctx->lhbm.max_timeout_ms);
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct backlight_device *bl = to_backlight_device(dev);
	struct gs_panel *ctx = bl_get_data(bl);
	const char *statestr;
	int rc, ret_cnt;

	mutex_lock(&ctx->bl_state_lock);

	if (bl->props.state & BL_STATE_STANDBY) {
		mutex_unlock(&ctx->bl_state_lock);
		return sysfs_emit(buf, "Off\n");
	} else if (bl->props.state & BL_STATE_LP) {
		statestr = "LP";
	} else if (GS_IS_HBM_ON(ctx->hbm_mode)) {
		statestr = GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode) ? "HBM IRC_OFF" : "HBM";
	} else {
		statestr = "On";
	}

	mutex_unlock(&ctx->bl_state_lock);

	ret_cnt = sysfs_emit(buf, "%s", statestr);
	rc = ret_cnt;

	if (rc > 0) {
		const struct gs_panel_mode *pmode;

		mutex_lock(&ctx->mode_lock);
		pmode = ctx->current_mode;
		mutex_unlock(&ctx->mode_lock);
		if (pmode) {
			ret_cnt = sysfs_emit_at(buf, ret_cnt, ": %dx%d@%d\n", pmode->mode.hdisplay,
						pmode->mode.vdisplay, gs_get_actual_vrefresh(ctx));
			if (ret_cnt > 0)
				rc += ret_cnt;
		} else {
			ret_cnt = sysfs_emit_at(buf, ret_cnt, "\n");
			if (ret_cnt > 0)
				rc += ret_cnt;
		}
	}

	dev_dbg(ctx->dev, "%s: %s\n", __func__, rc > 0 ? buf : "");

	return rc;
}

static ssize_t acl_mode_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);
	ssize_t ret;
	u32 acl_mode;

	if (!gs_is_panel_active(ctx)) {
		dev_err(ctx->dev, "panel is not enabled\n");
		return -EAGAIN;
	}

	if (!gs_panel_has_func(ctx, set_acl_mode)) {
		dev_err(ctx->dev, "ACL is not supported\n");
		return -ENOTSUPP;
	}

	ret = kstrtouint(buf, 0, &acl_mode);
	if (ret || (acl_mode > ACL_ENHANCED)) {
		dev_err(dev, "invalid acl mode\n");
		return ret;
	}

	mutex_lock(&ctx->mode_lock);
	ctx->acl_mode = acl_mode;
	ctx->desc->gs_panel_func->set_acl_mode(ctx, acl_mode);
	mutex_unlock(&ctx->mode_lock);

	return count;
}

static ssize_t acl_mode_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);

	if (!gs_is_panel_active(ctx)) {
		dev_err(ctx->dev, "panel is not enabled\n");
		return -EAGAIN;
	}

	return sysfs_emit(buf, "%d\n", ctx->acl_mode);
}

static DEVICE_ATTR_RW(hbm_mode);
static DEVICE_ATTR_RW(dimming_on);
static DEVICE_ATTR_RW(local_hbm_mode);
static DEVICE_ATTR_RW(local_hbm_max_timeout);
static DEVICE_ATTR_RO(state);
static DEVICE_ATTR_RW(acl_mode);

static struct attribute *bl_device_attrs[] = {
	&dev_attr_hbm_mode.attr,
	&dev_attr_dimming_on.attr,
	&dev_attr_local_hbm_mode.attr,
	&dev_attr_local_hbm_max_timeout.attr,
	&dev_attr_acl_mode.attr,
	&dev_attr_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(bl_device);

int gs_panel_sysfs_create_bl_files(struct device *bl_dev)
{
	return sysfs_create_groups(&bl_dev->kobj, bl_device_groups);
}

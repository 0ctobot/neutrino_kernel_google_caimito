// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Whitechapel AoC ALSA Driver on  AoC Audio Control
 *
 * Copyright (c) 2019 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "aoc_alsa.h"
#include "aoc_alsa_drv.h"

#ifndef ALSA_AOC_CMD_LOG_DISABLE
static int cmd_count;
#endif

/*
 * Sending commands to AoC for setting parameters and start/stop the streams
 */
static int aoc_audio_control(const char *cmd_channel, const uint8_t *cmd,
			     size_t cmd_size, uint8_t *response,
			     struct aoc_chip *chip)
{
	struct aoc_service_dev *dev;
	uint8_t *buffer;
	int buffer_size = 1024;
	struct timespec64 tv0, tv1;
	int err, count;
	unsigned long time_expired;

	if (!cmd_channel || !cmd)
		return -EINVAL;

	spin_lock(&chip->audio_lock);

	/* Get the aoc audio control channel at runtime */
	err = alloc_aoc_audio_service(cmd_channel, &dev);
	if (err < 0) {
		spin_unlock(&chip->audio_lock);
		return err;
	}

#ifndef ALSA_AOC_CMD_LOG_DISABLE
	cmd_count++;
	pr_notice_ratelimited(ALSA_AOC_CMD
			      " cmd [%s] id %#06x, size %zu, cntr %d\n",
			      CMD_CHANNEL(dev), ((struct CMD_HDR *)cmd)->id,
			      cmd_size, cmd_count);
#endif

	buffer = kmalloc_array(buffer_size, sizeof(*buffer), GFP_ATOMIC);
	if (!buffer) {
		err = -ENOMEM;
		pr_err("ERR: no memory!\n");
		goto exit;
	}

	/*
	 * TODO: assuming only one user for the audio control channel.
	 * clear all the response messages from previous commands
	 */
	count = 0;
	while (((err = aoc_service_read(dev, buffer, buffer_size,
					NONBLOCKING)) >= 1)) {
		count++;
	}
	if (count > 0)
		pr_debug("%d messages read for previous commands\n", count);

	/* Sending cmd to AoC */
	if ((aoc_service_write(dev, cmd, cmd_size, NONBLOCKING)) != cmd_size) {
		pr_err(ALSA_AOC_CMD " ERR: ring full - cmd id %#06x\n",
		       ((struct CMD_HDR *)cmd)->id);
		err = -EAGAIN;
		goto exit;
	}

#ifdef AOC_CMD_DEBUG_ENABLE
	ktime_get_real_ts64(&tv0);
#endif /* AOC_CMD_DEBUG_ENABLE */

	/* Getting responses from Aoc for the command just sent */
	count = 0;
	time_expired = jiffies + msecs_to_jiffies(WAITING_TIME_MS);
	while (((err = aoc_service_read(dev, buffer, buffer_size,
					NONBLOCKING)) < 1) &&
	       time_is_after_jiffies(time_expired)) {
		count++;
	}

#ifdef AOC_CMD_DEBUG_ENABLE
	ktime_get_real_ts64(&tv1);
	pr_debug("Elapsed: %lld (usecs)\n",
		 (tv1.tv_sec - tv0.tv_sec) * USEC_PER_SEC +
			 (tv1.tv_nsec - tv0.tv_nsec) / NSEC_PER_USEC);

	if (count > 0)
		pr_debug("%d times tried for response\n", count);
#endif /* AOC_CMD_DEBUG_ENABLE */

	if (err < 1) {
		pr_err(ALSA_AOC_CMD " ERR:timeout - cmd [%s] id %#06x\n",
		       CMD_CHANNEL(dev), ((struct CMD_HDR *)cmd)->id);
		print_hex_dump(KERN_ERR, ALSA_AOC_CMD " :mem ",
			       DUMP_PREFIX_OFFSET, 16, 1, cmd, cmd_size, false);
	} else if (err == 4) {
		pr_err(ALSA_AOC_CMD " ERR:%#x - cmd [%s] id %#06x\n",
		       *(uint32_t *)buffer, CMD_CHANNEL(dev),
		       ((struct CMD_HDR *)cmd)->id);
		print_hex_dump(KERN_ERR, ALSA_AOC_CMD " :mem ",
			       DUMP_PREFIX_OFFSET, 16, 1, cmd, cmd_size, false);
	} else {
		pr_debug(ALSA_AOC_CMD
			 " cmd [%s] id %#06x, reply mesg size %d\n",
			 CMD_CHANNEL(dev), ((struct CMD_HDR *)cmd)->id, err);
	}

	if (response != NULL)
		memcpy(response, buffer, cmd_size);

exit:
	kfree(buffer);
	free_aoc_audio_service(cmd_channel, dev);
	spin_unlock(&chip->audio_lock);

	return err < 1 ? -EAGAIN : 0;
}

int aoc_audio_volume_set(struct aoc_chip *chip, uint32_t volume, int src,
			 int dst)
{
	int err;
	struct CMD_AUDIO_OUTPUT_SET_PARAMETER cmd;

	/* No volume control for capturing */
	/* Haptics in AoC does not have adjustable volume */
	if (src == HAPTICS)
		return 0;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_SET_PARAMETER_ID,
		     sizeof(cmd));

	/* Sink 0-4 with block ID from 16-20 */
	cmd.block = dst + AOC_AUDIO_SINK_BLOCK_ID_BASE;
	cmd.component = 0;
	cmd.key = src;
	cmd.val = volume;
	pr_debug("volume changed to: %d\n", volume);

	/* Send cmd to AOC */
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd,
				sizeof(cmd), NULL, chip);
	if (err < 0)
		pr_err("ERR:%d in volume set\n", err);

	return err;
}

int aoc_set_builtin_mic_power_state(struct aoc_chip *chip, int iMic, int state)
{
	int err;
	struct CMD_AUDIO_INPUT_MIC_POWER_ON cmd_on;
	struct CMD_AUDIO_INPUT_MIC_POWER_OFF cmd_off;

	if (state == 1) {
		AocCmdHdrSet(&(cmd_on.parent), CMD_AUDIO_INPUT_MIC_POWER_ON_ID,
			     sizeof(cmd_on));
		cmd_on.mic_index = iMic;
		err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd_on,
					sizeof(cmd_on), NULL, chip);
	} else {
		AocCmdHdrSet(&(cmd_off.parent),
			     CMD_AUDIO_INPUT_MIC_POWER_OFF_ID, sizeof(cmd_off));
		cmd_off.mic_index = iMic;
		err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd_off,
					sizeof(cmd_off), NULL, chip);
	}

	if (err < 0)
		pr_err("ERR:%d in set mic state\n", err);

	return err < 0 ? err : 0;
}

int aoc_get_builtin_mic_power_state(struct aoc_chip *chip, int iMic)
{
	int err;
	struct CMD_AUDIO_INPUT_MIC_GET_POWER_STATE cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_INPUT_MIC_GET_POWER_STATE_ID,
		     sizeof(cmd));

	cmd.mic_index = iMic;

	err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd),
				(uint8_t *)&cmd, chip);
	if (err < 0)
		pr_err("ERR:%d in get mic state\n", err);

	return err < 0 ? err : cmd.power_state;
}

int aoc_mic_clock_rate_get(struct aoc_chip *chip)
{
	int err;
	struct CMD_AUDIO_INPUT_GET_MIC_CLOCK_FREQUENCY cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_INPUT_GET_MIC_CLOCK_FREQUENCY_ID,
		     sizeof(cmd));

	err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd),
				(uint8_t *)&cmd, chip);
	if (err < 0) {
		pr_err("ERR:%d in get mic clock frequency\n", err);
		return err;
	}

	return cmd.mic_clock_frequency_hz;
}

int aoc_mic_hw_gain_get(struct aoc_chip *chip, int state)
{
	int err;
	/* TODO: the cmd structures for 3 states differ only in names */
	struct CMD_AUDIO_INPUT_GET_MIC_CURRENT_HW_GAIN cmd;
	int cmd_id;

	switch (state) {
	case MIC_LOW_POWER_GAIN:
		cmd_id = CMD_AUDIO_INPUT_GET_MIC_LOW_POWER_HW_GAIN_ID;
		break;
	case MIC_HIGH_POWER_GAIN:
		cmd_id = CMD_AUDIO_INPUT_GET_MIC_HIGH_POWER_HW_GAIN_ID;
		break;
	default:
		cmd_id = CMD_AUDIO_INPUT_GET_MIC_CURRENT_HW_GAIN_ID;
	}

	AocCmdHdrSet(&(cmd.parent), cmd_id, sizeof(cmd));

	err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd),
				(uint8_t *)&cmd, chip);
	if (err < 0) {
		pr_err("ERR:%d in get current mic hw gain\n", err);
		return err;
	}

	return cmd.mic_hw_gain_cb;
}

int aoc_mic_hw_gain_set(struct aoc_chip *chip, int state, int gain)
{
	int err;
	/* TODO: the cmd structures for 3 states differ only in names */
	struct CMD_AUDIO_INPUT_SET_MIC_LOW_POWER_HW_GAIN cmd;
	int cmd_id;

	switch (state) {
	case MIC_LOW_POWER_GAIN:
		cmd_id = CMD_AUDIO_INPUT_SET_MIC_LOW_POWER_HW_GAIN_ID;
		break;
	case MIC_HIGH_POWER_GAIN:
		cmd_id = CMD_AUDIO_INPUT_SET_MIC_HIGH_POWER_HW_GAIN_ID;
		break;
	default:
		cmd_id = CMD_AUDIO_INPUT_SET_MIC_LOW_POWER_HW_GAIN_ID;
	}

	AocCmdHdrSet(&(cmd.parent), cmd_id, sizeof(cmd));
	cmd.mic_hw_gain_cb = gain;
	pr_debug("power state =%d, gain = %d\n", state, cmd.mic_hw_gain_cb);

	err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd),
				(uint8_t *)&cmd, chip);
	if (err < 0) {
		pr_err("ERR:%d in set mic hw gain\n", err);
		return err;
	}

	return 0;
}

int aoc_mic_dc_blocker_get(struct aoc_chip *chip)
{
	int err;
	struct CMD_AUDIO_INPUT_GET_MIC_DC_BLOCKER cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_INPUT_GET_MIC_DC_BLOCKER_ID,
		     sizeof(cmd));

	err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd),
				(uint8_t *)&cmd, chip);
	if (err < 0) {
		pr_err("ERR:%d in get mic dc blocker state\n", err);
		return err;
	}

	return cmd.dc_blocker_enabled;
}

int aoc_mic_dc_blocker_set(struct aoc_chip *chip, int enable)
{
	int err;
	struct CMD_AUDIO_INPUT_SET_MIC_DC_BLOCKER cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_INPUT_SET_MIC_DC_BLOCKER_ID,
		     sizeof(cmd));
	cmd.dc_blocker_enabled = enable;

	err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd),
				NULL, chip);
	if (err < 0)
		pr_err("ERR:%d in set mic dc blocker state as %d\n", err,
		       enable);

	return err;
}

int aoc_sidetone_cfg_get(struct aoc_chip *chip, int param, long *val)
{
	int err = 0;
	struct CMD_AUDIO_OUTPUT_GET_SIDETONE cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_GET_SIDETONE_ID, sizeof(cmd));

	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), (uint8_t *)&cmd,
				chip);
	if (err < 0) {
		pr_err("ERR:%d in get mic dc blocker state\n", err);
		return err;
	}

	pr_info("%s, sidetone- vol:%d, Eq num stage: %d, mic:%d\n", __func__, cmd.volume,
		cmd.eq_num_stages, cmd.mic_select);

	chip->sidetone_cfg = cmd;

	if (val) {
		switch (param) {
		case SIDETONE_CFG_VOL:
			*val = cmd.volume;
			break;
		case SIDETONE_CFG_STAGE_NUM:
			*val = cmd.eq_num_stages;
			break;
		case SIDETONE_CFG_MIC_ID:
			*val = cmd.mic_select;
			break;
		default:
			err = -EINVAL;
			pr_err("ERR:%d invalid param for sidetone cfg\n", err);
		}
	}

	return err;
}

int aoc_sidetone_cfg_set(struct aoc_chip *chip, int param, long val)
{
	int err = 0;
	struct CMD_AUDIO_OUTPUT_SET_SIDETONE cmd;

	aoc_sidetone_cfg_get(chip, 0, NULL); //get the updated data from AoC before setting
	cmd.eq_num_stages = chip->sidetone_cfg.eq_num_stages;
	cmd.mic_select = chip->sidetone_cfg.mic_select;
	cmd.volume = chip->sidetone_cfg.volume;

	switch (param) {
	case SIDETONE_CFG_VOL:
		cmd.volume = val;
		break;
	case SIDETONE_CFG_STAGE_NUM:
		cmd.eq_num_stages = val;
		break;
	case SIDETONE_CFG_MIC_ID:
		cmd.mic_select = val;
		break;
	default:
		return -EINVAL;
	}

	pr_info("%s, side tone- vol:%d, Eq num stage: %d, mic:%d\n", __func__, cmd.volume,
		cmd.eq_num_stages, cmd.mic_select);

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_SET_SIDETONE_ID, sizeof(cmd));
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), (uint8_t *)&cmd,
				chip);
	if (err < 0) {
		pr_err("ERR:%d in set sidetone params\n", err);
		return err;
	}

	return err;
}

int aoc_sidetone_eq_get(struct aoc_chip *chip, int biquad_idx, long *val)
{
	int i, err = 0;
	struct CMD_AUDIO_OUTPUT_GET_SIDETONE_EQ cmd;
	int n_params = ARRAY_SIZE(cmd.coeffs);

	pr_info("%s: sidetone eq %d - %d params\n", __func__, biquad_idx, n_params);

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_GET_SIDETONE_EQ_ID, sizeof(cmd));

	cmd.stage_num = biquad_idx;
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), (uint8_t *)&cmd,
				chip);
	if (err < 0) {
		pr_err("ERR:%d in get sidetone eq param\n", err);
		return err;
	}

	if (val) {
		for (i = 0; i < n_params; i++)
			val[i] = *(uint32_t *)&cmd.coeffs[i];
	}

	return 0;
}

int aoc_sidetone_eq_set(struct aoc_chip *chip, int biquad_idx, long *val)
{
	int i, err = 0;
	uint32_t tmp;
	struct CMD_AUDIO_OUTPUT_SET_SIDETONE_EQ cmd;
	int n_params = ARRAY_SIZE(cmd.coeffs);

	pr_info("%s: sidetone eq %d - %d params\n", __func__, biquad_idx, n_params);

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_SET_SIDETONE_EQ_ID, sizeof(cmd));
	cmd.stage_num = biquad_idx;
	for (i = 0; i < n_params; i++) {
		tmp = (uint32_t)val[i];
		cmd.coeffs[i] = *(float *)(&tmp);
	}
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), (uint8_t *)&cmd,
				chip);
	if (err < 0) {
		pr_err("ERR:%d in set sidetone eq param\n", err);
		return err;
	}

	return 0;
}

int aoc_incall_capture_enable_get(struct aoc_chip *chip, int stream, long *val)
{
	int err;
	struct CMD_AUDIO_OUTPUT_TELE_CAPT cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_GET_TELE_CAPT_ID, sizeof(cmd));

	cmd.ring = stream;
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), (uint8_t *)&cmd,
				chip);
	if (err < 0) {
		pr_err("ERR:%d in get voice capture stream %d state\n", err, stream);
		return err;
	}

	if (val)
		*val = cmd.mode;

	chip->incall_capture_state[stream] = cmd.mode;
	pr_info("%s: get voice call capture stream %d state %d\n", __func__, stream, cmd.mode);

	return 0;
}

int aoc_incall_capture_enable_set(struct aoc_chip *chip, int stream, long val)
{
	int err;
	struct CMD_AUDIO_OUTPUT_TELE_CAPT cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_SET_TELE_CAPT_ID, sizeof(cmd));
	cmd.ring = stream;
	cmd.mode = val;

	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), (uint8_t *)&cmd,
				chip);
	if (err < 0) {
		pr_err("ERR:%d in set incall capture stream %d state as %d\n", err, stream,
		       cmd.mode);
		return err;
	}

	chip->incall_capture_state[stream] = val;
	pr_info("%s: set incall capture stream %d state as %d\n", __func__, stream, cmd.mode);

	return 0;
}

int aoc_incall_playback_enable_get(struct aoc_chip *chip, int stream, long *val)
{
	int err;
	struct CMD_AUDIO_INPUT_INCALL_MUSIC cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_INPUT_GET_INCALL_MUSIC_ID, sizeof(cmd));

	cmd.ring = stream;
	err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), (uint8_t *)&cmd,
				chip);
	if (err < 0) {
		pr_err("ERR:%d in get voice playback stream %d state\n", err, stream);
		return err;
	}

	if (val)
		*val = cmd.enable;

	pr_info("%s: get incall playback stream %d state %d\n", __func__, stream, cmd.enable);

	return 0;
}

int aoc_incall_playback_enable_set(struct aoc_chip *chip, int stream, long val)
{
	int err;
	struct CMD_AUDIO_INPUT_INCALL_MUSIC cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_INPUT_SET_INCALL_MUSIC_ID, sizeof(cmd));
	cmd.ring = stream;
	cmd.enable = val;

	err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), (uint8_t *)&cmd,
				chip);
	if (err < 0) {
		pr_err("ERR:%d in set incall playback stream %d state as %d\n", err, stream,
		       cmd.enable);
		return err;
	}

	pr_info("%s: set incall playback stream %d state as %d\n", __func__, stream, cmd.enable);

	return 0;
}

/* TODO: temporary solution for mic muting, has to be revised using DSP modules instead of mixer */
int aoc_voice_call_mic_mute(struct aoc_chip *chip, int mute)
{
	int err;
	int gain = (mute == 1) ? -700 : chip->default_mic_hw_gain;

	pr_debug("voice call mic mute: %d\n", mute);
	if ((err = aoc_mic_hw_gain_set(chip, MIC_HIGH_POWER_GAIN, gain))) {
		pr_err("ERR: fail in muting mic in voice call\n");
		return err;
	}

	return 0;
}

int aoc_get_builtin_mic_process_mode(struct aoc_chip *chip)
{
	int err;
	struct CMD_AUDIO_INPUT_GET_AP_MIC_INDEX cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_INPUT_GET_AP_MIC_INDEX_ID,
		     sizeof(cmd));

	err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd,
				sizeof(cmd), (uint8_t *)&cmd, chip);
	if (err < 0)
		pr_err("Error in get builtin_mic_process_mode!\n");

	return err < 0 ? err : cmd.mic_process_index;
}

int aoc_get_dsp_state(struct aoc_chip *chip)
{
	int err;
	struct CMD_AUDIO_OUTPUT_GET_DSP_STATE cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_GET_DSP_STATE_ID,
		     sizeof(cmd));

	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd,
				sizeof(cmd), (uint8_t *)&cmd, chip);
	if (err < 0)
		pr_err("Error in get aoc dsp state !\n");

	return err < 0 ? err : cmd.mode;
}

int aoc_get_asp_mode(struct aoc_chip *chip, int block, int component, int key)
{
	int err;
	struct CMD_AUDIO_OUTPUT_GET_PARAMETER cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_GET_PARAMETER_ID,
		     sizeof(cmd));

	/* Sink 0-4 with block ID from 16-20 */
	cmd.block = block;
	cmd.component = component;
	cmd.key = key;

	pr_info("get asp mode: block=%d, component=%d, key=%d\n", block, component, key);

	/* Send cmd to AOC */
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd,
				sizeof(cmd), (uint8_t *)&cmd, chip);
	if (err < 0) {
		pr_err("ERR:%d in getting dsp mode, block=%d, component=%d, key=%d\n",
		       err, block, component, key);
		return err;
	}

	return cmd.val;
}

int aoc_set_asp_mode(struct aoc_chip *chip, int block, int component, int key,
		     int val)
{
	int err;
	struct CMD_AUDIO_OUTPUT_SET_PARAMETER cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_SET_PARAMETER_ID,
		     sizeof(cmd));

	/* Sink 0-4 with block ID from 16-20 */
	cmd.block = block;
	cmd.component = component;
	cmd.key = key;
	cmd.val = val;

	pr_info("set asp mode: block=%d, component=%d, key=%d, val=%d\n", block,
		component, key, val);

	/* Send cmd to AOC */
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd,
				sizeof(cmd), NULL, chip);
	if (err < 0)
		pr_err("ERR:%d in dsp mode, block=%d, component=%d, key=%d, val=%d\n",
		       err, block, component, key, val);

	return err;
}

int aoc_get_sink_channel_bitmap(struct aoc_chip *chip, int sink)
{
	int err;
	struct CMD_AUDIO_OUTPUT_GET_SINKS_BITMAPS cmd;

	if (sink >= AUDIO_OUTPUT_SINKS) {
		pr_err("Err: sink id %d not exists!\n", sink);
		return -EINVAL;
	}

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_GET_SINKS_BITMAPS_ID, sizeof(cmd));

	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), (uint8_t *)&cmd,
				chip);
	if (err < 0) {
		pr_err("Err:%d in get aoc sink %d channel bitmap!\n", err, sink);
		return err;
	}

	return cmd.bitmap[sink];
}

int aoc_get_sink_mode(struct aoc_chip *chip, int sink)
{
	return chip->sink_mode[sink];
}

int aoc_set_sink_mode(struct aoc_chip *chip, int sink, int mode)
{
	int err;
	struct CMD_AUDIO_OUTPUT_SINK cmd;
	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_SINK_ID, sizeof(cmd));

	cmd.sink = sink;
	cmd.mode = mode;
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd,
				sizeof(cmd), (uint8_t *)&cmd, chip);
	if (err < 0) {
		pr_err("Error in get aoc sink processing state !\n");
		return err;
	}

	chip->sink_mode[sink] = mode;
	pr_info("sink state set :%d - %d\n", sink, cmd.mode);

	return 0;
}

int aoc_get_sink_state(struct aoc_chip *chip, int sink)
{
	int err;
	struct CMD_AUDIO_OUTPUT_GET_SINK_PROCESSING_STATE cmd;
	AocCmdHdrSet(&(cmd.parent),
		     CMD_AUDIO_OUTPUT_GET_SINK_PROCESSING_STATE_ID,
		     sizeof(cmd));

	cmd.sink = sink;
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd,
				sizeof(cmd), (uint8_t *)&cmd, chip);
	if (err < 0)
		pr_err("Error in get aoc sink processing state !\n");

	pr_info("sink_state:%d - %d\n", sink, cmd.mode);

	return err < 0 ? err : cmd.mode;
}

/* TODO: usb cfg may be devided into three commands, dev/ep-ids, rx, tx */
int aoc_set_usb_config(struct aoc_chip *chip)
{
	int err;
	struct CMD_AUDIO_OUTPUT_USB_CONFIG cmd = chip->usb_sink_cfg;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_USB_CONFIG_ID, sizeof(cmd));

	cmd.rx_enable = true;
	cmd.tx_enable = true;
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), NULL, chip);
	if (err < 0)
		pr_err("Err:%d in aoc set usb config!\n", err);

	return err;
}

static int
aoc_audio_playback_trigger_source(struct aoc_alsa_stream *alsa_stream, int cmd,
				  int src)
{
	int err;
	struct CMD_AUDIO_OUTPUT_SOURCE source;

	AocCmdHdrSet(&(source.parent), CMD_AUDIO_OUTPUT_SOURCE_ID,
		     sizeof(source));

	/* source on/off */
	source.source = src;
	switch (cmd) {
	case START:
		source.on = 1;
		break;
	case STOP:
		source.on = 0;
		break;
	default:
		pr_err("Invalid source operation (only On/Off allowed)\n");
		return -EINVAL;
	}

	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&source,
				sizeof(source), NULL, alsa_stream->chip);

	pr_debug("Source %d %s !\n", alsa_stream->idx,
		 cmd == START ? "on" : "off");

	return err;
}

static int aoc_audio_playback_source_on(struct aoc_chip *chip, int cmd, int src)
{
	int err;
	struct CMD_AUDIO_OUTPUT_SOURCE source;

	AocCmdHdrSet(&(source.parent), CMD_AUDIO_OUTPUT_SOURCE_ID, sizeof(source));

	/* source on/off */
	source.source = src;
	switch (cmd) {
	case START:
		source.on = 1;
		break;
	case STOP:
		source.on = 0;
		break;
	default:
		pr_err("Invalid source operation (only On/Off allowed)\n");
		return -EINVAL;
	}

	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&source, sizeof(source), NULL, chip);
	if (err < 0) {
		pr_err("ERR:%d in playback source %s\n", err, (cmd == START) ? "on" : "off");
		return err;
	}

	pr_debug("Source %d %s !\n", src, cmd == START ? "on" : "off");

	return 0;
}

static int aoc_audio_playback_trigger_bind(struct aoc_alsa_stream *alsa_stream,
					   int cmd, int src, int dst)
{
	int err;
	struct CMD_AUDIO_OUTPUT_BIND bind;

	AocCmdHdrSet(&(bind.parent), CMD_AUDIO_OUTPUT_BIND_ID, sizeof(bind));
	bind.bind = (cmd == START) ? 1 : 0;
	bind.src = src;
	bind.dst = dst;
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&bind,
				sizeof(bind), NULL, alsa_stream->chip);

	/* bind/unbind the source and dest */
	pr_debug("%s: src: %d- sink: %d!\n", cmd == START ? "bind" : "unbind",
		 src, dst);

	return err;
}

/* Bind/unbind the source and dest */
static int aoc_audio_path_bind(int src, int dst, int cmd, struct aoc_chip *chip)
{
	int err;
	struct CMD_AUDIO_OUTPUT_BIND bind;

	if (dst < 0)
		return 0;

	pr_info("%s: src:%d - sink:%d!\n", cmd == START ? "bind" : "unbind", src, dst);

	AocCmdHdrSet(&(bind.parent), CMD_AUDIO_OUTPUT_BIND_ID, sizeof(bind));
	bind.bind = (cmd == START) ? 1 : 0;
	bind.src = src;
	bind.dst = dst;
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&bind, sizeof(bind), NULL, chip);

	if (err < 0)
		pr_err("ERR:%d %s: src:%d - sink:%d!\n", err, (cmd == START) ? "bind" : "unbind",
		       src, dst);

	return err;
}

int aoc_audio_path_open(struct aoc_chip *chip, int src, int dest)
{
	/* voice call capture or playback */
	if ((src == 3 && dest == -1) || src == 4)
		return aoc_phonecall_path_open(chip, src, dest);

	if (src == IDX_VOIP)
		return aoc_voipcall_path_open(chip, src, dest);

	return aoc_audio_path_bind(src, dest, START, chip);
}

int aoc_audio_path_close(struct aoc_chip *chip, int src, int dest)
{
	/* voice call capture or playback */
	if ((src == 3 && dest == -1) || src == 4)
		return aoc_phonecall_path_close(chip, src, dest);

	if (src == IDX_VOIP)
		return aoc_voipcall_path_close(chip, src, dest);

	return aoc_audio_path_bind(src, dest, STOP, chip);
}

static int aoc_audio_playback_set_params(struct aoc_alsa_stream *alsa_stream,
					 uint32_t channels, uint32_t samplerate,
					 uint32_t bps, bool pcm_float_fmt,
					 int source_mode)
{
	int err;
	struct CMD_AUDIO_OUTPUT_EP_SETUP cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_EP_SETUP_ID, sizeof(cmd));
	cmd.d.channel = alsa_stream->entry_point_idx;
	cmd.d.watermark = PLAYBACK_WATERMARK_DEFAULT;
	cmd.d.length = 0;
	cmd.d.address = 0;
	cmd.d.wraparound = true;
	cmd.d.metadata.offset = 0;

	switch (bps) {
	case 32:
		cmd.d.metadata.bits = WIDTH_32_BIT;
		break;
	case 24:
		cmd.d.metadata.bits = WIDTH_24_BIT;
		break;
	case 16:
		cmd.d.metadata.bits = WIDTH_16_BIT;
		break;
	case 8:
		cmd.d.metadata.bits = WIDTH_8_BIT;
		break;
	default:
		cmd.d.metadata.bits = WIDTH_32_BIT;
	}

	cmd.d.metadata.format =
		(pcm_float_fmt) ? FRMT_FLOATING_POINT : FRMT_FIXED_POINT;
	cmd.d.metadata.chan = channels;

	switch (samplerate) {
	case 48000:
		cmd.d.metadata.sr = SR_48KHZ;
		break;
	case 44100:
		cmd.d.metadata.sr = SR_44K1HZ;
		break;
	case 16000:
		cmd.d.metadata.sr = SR_16KHZ;
		break;
	case 8000:
		cmd.d.metadata.sr = SR_8KHZ;
		break;
	default:
		cmd.d.metadata.sr = SR_48KHZ;
	}

	pr_debug("chan =%d, sr=%d, bits=%d\n", cmd.d.metadata.chan,
		 cmd.d.metadata.sr, cmd.d.metadata.bits);

	switch (source_mode) {
	case PLAYBACK_MODE:
		cmd.mode = ENTRYPOINT_MODE_PLAYBACK;
		break;
	case HAPTICS_MODE:
		cmd.mode = ENTRYPOINT_MODE_HAPTICS;
		break;
	case OFFLOAD_MODE:
		cmd.mode = ENTRYPOINT_MODE_DECODE_OFFLOAD;
		break;
	default:
		cmd.mode = ENTRYPOINT_MODE_PLAYBACK;
	}

	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd,
				sizeof(cmd), NULL, alsa_stream->chip);
	if (err < 0)
		pr_err("ERR:%d in playback set parameters\n", err);

	return err;
}

static int aoc_audio_capture_set_params(struct aoc_alsa_stream *alsa_stream,
					uint32_t channels, uint32_t samplerate,
					uint32_t bps, bool pcm_float_fmt)
{
	int i, mic_id, n_mic, err = 0;
	struct CMD_AUDIO_INPUT_MIC_RECORD_AP_SET_PARAMS cmd;
	struct aoc_chip *chip = alsa_stream->chip;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_INPUT_MIC_RECORD_AP_SET_PARAMS_ID,
		     sizeof(cmd));

	/* TODO: the output of spatial module is stereo */
	if (channels < 1 || channels > NUM_OF_BUILTIN_MIC ||
	    (channels != 2 && chip->mic_spatial_module_enable)) {
		pr_err("ERR: wrong channel number %u for capture\n", channels);
		err = -EINVAL;
		goto exit;
	}

	/* TODO: more checks on mic id */
	cmd.pdm_mask = 0; /* Default to usb/bt mic capturing */
	if (chip->audio_capture_mic_source == BUILTIN_MIC) {
		n_mic = (chip->mic_spatial_module_enable) ? N_MIC_IN_SPATIAL_MODULE : channels;
		for (i = 0; i < n_mic; i++) {
			mic_id = chip->buildin_mic_id_list[i];
			if (mic_id != -1) {
				cmd.pdm_mask = cmd.pdm_mask | (1 << mic_id);
			} else {
				pr_err("ERR: wrong mic id -1\n");
				err = -EINVAL;
				goto exit;
			}
		}
	}

	cmd.period_ms = 10; /*TODO: how to make it configuratable*/
	cmd.num_periods = 4; /*TODO: how to make it configuratable*/

	switch (samplerate) {
	case 48000:
		cmd.sample_rate = SR_48KHZ;
		break;
	case 44100:
		cmd.sample_rate = SR_44K1HZ;
		break;
	case 16000:
		cmd.sample_rate = SR_16KHZ;
		break;
	case 8000:
		cmd.sample_rate = SR_8KHZ;
		break;
	default:
		cmd.sample_rate = SR_48KHZ;
	}

	switch (bps) {
	case 32:
		cmd.requested_format.bits = WIDTH_32_BIT;
		break;
	case 24:
		/* TODO: tinycap limitation */
		cmd.requested_format.bits = WIDTH_32_BIT;
		break;
	case 16:
		cmd.requested_format.bits = WIDTH_16_BIT;
		break;
	case 8:
		cmd.requested_format.bits = WIDTH_8_BIT;
		break;
	default:
		cmd.requested_format.bits = WIDTH_32_BIT;
	}
	cmd.requested_format.sr =
		cmd.sample_rate; /* TODO: double check format*/
	cmd.requested_format.format =
		(pcm_float_fmt) ? FRMT_FLOATING_POINT : FRMT_FIXED_POINT;

	cmd.requested_format.chan = channels;

	if (chip->mic_spatial_module_enable)
		cmd.mic_process_index = AP_MIC_PROCESS_SPATIAL;
	else
		cmd.mic_process_index = AP_MIC_PROCESS_RAW;

	err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd),
				NULL, chip);
	if (err < 0) {
		pr_err("ERR:%d in capture parameter setup\n", err);
		goto exit;
	}

	pr_debug("Flush aoc ring buffer\n");
	if (!aoc_ring_flush_read_data(alsa_stream->dev->service, AOC_UP, 0)) {
		pr_err("ERR: capture ring buffer flush fail\n");
		err = -EINVAL;
	}

exit:
	return err;
}

static int aoc_audio_capture_spatial_module_trigger(struct aoc_chip *chip,
						    int record_cmd)
{
	int err = 0;
	struct CMD_HDR cmd;
	pr_info("%s: %d", __func__, record_cmd);
	AocCmdHdrSet(&cmd,
		     (record_cmd == START) ? CMD_AUDIO_INPUT_SPATIAL_START_ID :
						   CMD_AUDIO_INPUT_SPATIAL_STOP_ID,
		     sizeof(cmd));

	err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd),
				NULL, chip);
	if (err < 0)
		pr_err("ERR:%d in spatial module trigger\n", err);

	return err;
}

static int aoc_audio_capture_mic_input(struct aoc_chip *chip,
				 int input_cmd, int mic_input_source)
{
	int err;
	struct CMD_HDR cmd0; /* For ap mic input STOP */
	struct CMD_AUDIO_INPUT_AP_INPUT_START cmd1;

	if (input_cmd == START) {
		AocCmdHdrSet(&(cmd1.parent),
			     CMD_AUDIO_INPUT_AP_INPUT_START_ID,
			     sizeof(cmd1));

		cmd1.mic_input_source = mic_input_source;
		err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd1,
					sizeof(cmd1), NULL, chip);
		if (err < 0)
			pr_err("ERR:%d audio capture mic input start fail!\n", err);

	} else {
		AocCmdHdrSet(&cmd0, CMD_AUDIO_INPUT_AP_INPUT_STOP_ID,
			     sizeof(cmd0));

		err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd0,
					sizeof(cmd0), NULL, chip);
		if (err < 0)
			pr_err("ERR:%d audio capture mic input stop fail!\n", err);
	}

	return err;
}

/* Start or stop the stream */
static int aoc_audio_capture_trigger(struct aoc_alsa_stream *alsa_stream, int record_cmd)
{
	int err = 0;
	struct CMD_HDR cmd;
	struct aoc_chip *chip = alsa_stream->chip;
	int mic_input_source = 0;

	pr_info("%s: %d", __func__, record_cmd);

	if (chip->audio_capture_mic_source == BUILTIN_MIC) {
		AocCmdHdrSet(&cmd,
			     (record_cmd == START) ? CMD_AUDIO_INPUT_MIC_RECORD_AP_START_ID :
						     CMD_AUDIO_INPUT_MIC_RECORD_AP_STOP_ID,
			     sizeof(cmd));

		err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), NULL,
					chip);
	} else {
		switch (chip->audio_capture_mic_source) {
		case USB_MIC:
			mic_input_source = AP_INPUT_PROCESSOR_USB_INPUT_INDEX;
			break;
		case BT_MIC:
			mic_input_source = AP_INPUT_PROCESSOR_BT_INPUT_INDEX;
			break;
		default:
			pr_err("ERR in mic input source for audio capture mic source=%d\n",
			       chip->audio_capture_mic_source);
			err = EINVAL;
			goto exit;
		}
		err = aoc_audio_capture_mic_input(chip, record_cmd, mic_input_source);
	}

exit:
	if (err < 0)
		pr_err("ERR:%d in capture trigger\n", err);

	return err;
}

int aoc_lvm_enable_get(struct aoc_chip *chip, long *enable)
{
	int err;
	struct CMD_AUDIO_OUTPUT_GET_PARAMETER cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_GET_PARAMETER_ID, sizeof(cmd));

	cmd.block = 14; /* ABLOCK_DCDOFF */
	cmd.component = 0; /* LVM */
	cmd.key = 16; /* ASP_LVM_PARAM_ENABLE */
	pr_debug("lvm: %s\n", enable ? "enabled" : "disabled");

	/* Send cmd to AOC */
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), NULL, chip);
	if (err < 0) {
		pr_err("ERR:%d in lvm set\n", err);
		return err;
	}

	if (enable)
		*enable = cmd.val;

	return 0;
}

int aoc_lvm_enable_set(struct aoc_chip *chip, long enable)
{
	int err;
	struct CMD_AUDIO_OUTPUT_SET_PARAMETER cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_SET_PARAMETER_ID, sizeof(cmd));

	cmd.block = 14; /* ABLOCK_DCDOFF */
	cmd.component = 0;
	cmd.key = 16;
	cmd.val = enable;
	pr_debug("lvm: %s\n", enable ? "enabled" : "disabled");

	/* Send cmd to AOC */
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), NULL, chip);
	if (err < 0) {
		pr_err("ERR:%d in lvm set\n", err);
		return err;
	}

	return 0;
}

int aoc_decoder_ref_enable_get(struct aoc_chip *chip, long *enable)
{
	int err;
	struct CMD_AUDIO_OUTPUT_GET_PARAMETER cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_GET_PARAMETER_ID, sizeof(cmd));

	cmd.block = 14;
	cmd.component = 1;
	cmd.key = 0;
	pr_debug("Decoder ref: %s\n", enable ? "enabled" : "disabled");

	/* Send cmd to AOC */
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), NULL, chip);
	if (err < 0) {
		pr_err("ERR:%d in decoder ref get\n", err);
		return err;
	}

	if (enable)
		*enable = cmd.val;

	return 0;
}

int aoc_decoder_ref_enable_set(struct aoc_chip *chip, long enable)
{
	int err;
	struct CMD_AUDIO_OUTPUT_SET_PARAMETER cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_SET_PARAMETER_ID, sizeof(cmd));

	cmd.block = 14;
	cmd.component = 1;
	cmd.key = 0;
	cmd.val = enable;
	pr_debug("decoder ref: %s\n", enable ? "enabled" : "disabled");

	/* Send cmd to AOC */
	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), NULL, chip);
	if (err < 0) {
		pr_err("ERR:%d in decoder ref set\n", err);
		return err;
	}

	return 0;
}

int aoc_sidetone_enable(struct aoc_chip *chip, int enable)
{
	int err = 0;
	int cmd = (enable) ? START : STOP;
	int src, dest;

	src = SIDETONE;
	dest = ASNK_SPEAKER;

	pr_info("sidetone: %s \n", (enable) ? "Enabled" : "Disabled");

	/* Source on/off */
	err = aoc_audio_playback_source_on(chip, cmd, src);
	if (err < 0) {
		pr_err("ERR=%d, sidetone %s fail in source(%d) on/off\n", err,
		       (enable) ? "START" : "STOP", src);
		goto exit;
	}

	/* Bind/ubind - source 11 and sink 0 */
	err = aoc_audio_path_bind(src, dest, cmd, chip);
	if (err < 0)
		pr_err("ERR=%d, sidestone %s fail in binding src:%d - dest:%d\n", err,
		       (enable) ? "START" : "STOP", src, dest);

exit:
	return err;
}

int aoc_mic_loopback(struct aoc_chip *chip, int enable)
{
	int err;
	struct CMD_AUDIO_INPUT_ENABLE_MIC_LOOPBACK cmd;

	AocCmdHdrSet(&(cmd.parent),
		     ((enable == 1) ? CMD_AUDIO_INPUT_MIC_LOOPBACK_START_ID :
				      CMD_AUDIO_INPUT_MIC_LOOPBACK_STOP_ID),
		     sizeof(cmd));
	cmd.sample_rate = SR_48KHZ;

	err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd),
				NULL, chip);
	if (err < 0)
		pr_err("ERR:%d in mic loopback\n", err);

	return err;
}

/*
 * For capture: start recording
 * for playback: source on and bind source/sinks
 *
 * TODO: Capturing from four mics on board differ from other sources
 * (BT, USB,... I2S interface from headphone)
 */
int aoc_audio_start(struct aoc_alsa_stream *alsa_stream)
{
	int err = 0;
	int src;

	/* TODO: compress offload may not use START for source on, CHECK! */
	if (alsa_stream->cstream ||
	    (alsa_stream->substream->stream == SNDRV_PCM_STREAM_PLAYBACK)) {
		src = alsa_stream->entry_point_idx;
		err = aoc_audio_playback_trigger_source(alsa_stream, START,
							src);
		if (err < 0)
			pr_err("ERR:%d in source on\n", err);
	} else {
		err = aoc_audio_capture_trigger(alsa_stream, START);
		if (err < 0)
			pr_err("ERR:%d in capture start\n", err);
	}

	return err;
}

/*
 * For capture: stop recording
 * For playback: source off and unbind source/sinks
 */
int aoc_audio_stop(struct aoc_alsa_stream *alsa_stream)
{
	int err = 0;
	int src;

	if (alsa_stream->cstream ||
	    (alsa_stream->substream->stream == SNDRV_PCM_STREAM_PLAYBACK)) {
		src = alsa_stream->entry_point_idx;
		err = aoc_audio_playback_trigger_source(alsa_stream, STOP, src);
		if (err < 0)
			pr_err("ERR:%d in source off\n", err);
	} else {
		err = aoc_audio_capture_trigger(alsa_stream, STOP);
		if (err < 0)
			pr_err("ERR:%d in capture stop\n", err);
	}

	return err;
}

int aoc_audio_incall_start(struct aoc_alsa_stream *alsa_stream)
{
	int stream, err = 0;
	struct aoc_chip *chip = alsa_stream->chip;

	/* TODO: stream number inferred by pcm device idx, pb_0:18, cap_0:20, better way needed */
	if (alsa_stream->substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		stream = alsa_stream->entry_point_idx - 18;
		err = aoc_incall_playback_enable_set(chip, stream, 1);
		if (err < 0)
			pr_err("ERR:%d in incall playback start on\n", err);
	} else {
		stream = alsa_stream->entry_point_idx - 20;
		err = aoc_incall_capture_enable_set(chip, stream,
						    chip->incall_capture_state[stream]);
		if (err < 0)
			pr_err("ERR:%d in incall capture start on\n", err);
	}

	return err;
}

int aoc_audio_incall_stop(struct aoc_alsa_stream *alsa_stream)
{
	int stream, err = 0;
	struct aoc_chip *chip = alsa_stream->chip;

	/* TODO: stream number inferred by pcm device idx, pb_0:18, cap_0:20, better way needed */
	if (alsa_stream->substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		stream = alsa_stream->entry_point_idx - 18;
		err = aoc_incall_playback_enable_set(chip, stream, 0);
		if (err < 0)
			pr_err("ERR:%d in incall playback start on\n", err);
	} else {
		stream = alsa_stream->entry_point_idx - 20;
		err = aoc_incall_capture_enable_set(chip, stream, 0);
		if (err < 0)
			pr_err("ERR:%d in incall capture start on\n", err);
	}

	return err;
}

int aoc_audio_voip_start(struct aoc_alsa_stream *alsa_stream)
{
	return 0;
}

int aoc_audio_voip_stop(struct aoc_alsa_stream *alsa_stream)
{
	return 0;
}

/* TODO: this function is modified to deal with the issue where ALSA appl_ptr
 * and the reader pointer in AoC ringer buffer are out-of-sync due to overflow
 */
int aoc_audio_read(struct aoc_alsa_stream *alsa_stream, void *dest,
		   uint32_t count)
{
	int err = 0;
	void *tmp;
	struct aoc_service_dev *dev = alsa_stream->dev;
	int avail;

	avail = aoc_ring_bytes_available_to_read(dev->service, AOC_UP);

	if (unlikely(avail < count)) {
		pr_err("ERR: overrun in audio capture. avail = %d, toread = %d\n",
		       avail, count);
	}

	/* Only read bytes available in the ring buffer */
	count = avail < count ? avail : count;
	if (count == 0)
		return 0;

	tmp = (void *)(alsa_stream->substream->runtime->dma_area);
	err = aoc_service_read(dev, (void *)tmp, count, NONBLOCKING);
	if (unlikely(err != count)) {
		pr_err("ERR: %d bytes not read from ring buffer\n",
		       count - err);
		err = -EFAULT;
		goto out;
	}

	err = copy_to_user(dest, tmp, count);
	if (err != 0) {
		pr_err("ERR: %d bytes not copied to user space\n", err);
		err = -EFAULT;
	}

out:
	return err < 0 ? err : 0;
}

int aoc_audio_write(struct aoc_alsa_stream *alsa_stream, void *src,
		    uint32_t count)
{
	int err = 0;
	struct aoc_service_dev *dev = alsa_stream->dev;
	void *tmp;
	int avail;

	avail = aoc_ring_bytes_available_to_write(dev->service, AOC_DOWN);
	if (unlikely(avail < count)) {
		pr_err("ERR: inconsistent write/read pointers, avail = %d, towrite = %u\n",
		       avail, count);
		err = -EFAULT;
		goto out;
	}
	if (alsa_stream->substream)
		tmp = alsa_stream->substream->runtime->dma_area;
	else
		tmp = alsa_stream->cstream->runtime->buffer;

	err = copy_from_user(tmp, src, count);
	if (err != 0) {
		pr_err("ERR: %d bytes not read from user space\n", err);
		err = -EFAULT;
		goto out;
	}

	err = aoc_service_write(dev, tmp, count, NONBLOCKING);
	if (err != count) {
		pr_err("ERR: unwritten data - %d bytes\n", count - err);
		err = -EFAULT;
	}

out:
	return err < 0 ? err : 0;
}

/* PCM channel setup ??? */
static int aoc_audio_set_ctls_chan(struct aoc_alsa_stream *alsa_stream,
				   struct aoc_chip *chip)
{
	int err = 0;
	int src, dst, i;

	pr_debug(" Setting ALSA  volume(%d)\n", chip->volume);

	if (alsa_stream->substream &&
	    alsa_stream->substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		return 0;

	src = alsa_stream->entry_point_idx;
	for (i = 0; i < MAX_NUM_OF_SINKS_PER_STREAM; i++) {
		dst = alsa_stream->chip->sink_id_list[i];
		if (dst == -1)
			continue;

		err = aoc_audio_volume_set(chip, chip->volume, src, dst);
		if (err < 0) {
			pr_err("ERR:%d in volume setting\n", err);
			goto out;
		}
	}

out:
	return err;
}

int aoc_audio_set_ctls(struct aoc_chip *chip)
{
	int i;
	int err = 0;

	/* change ctls for all substreams */
	for (i = 0; i < MAX_NUM_OF_SUBSTREAMS; i++) {
		if (chip->avail_substreams & (1 << i)) {
			pr_debug(" Setting %d stream i =%d\n",
				 chip->avail_substreams, i);

			if (!chip->alsa_stream[i]) {
				pr_debug(
					" No ALSA stream available?! %i:%p (%x)\n",
					i, chip->alsa_stream[i],
					chip->avail_substreams);
				err = 0;
			} else if (aoc_audio_set_ctls_chan(chip->alsa_stream[i],
							   chip) != 0) {
				pr_err("ERR: couldn't set controls for stream %d\n",
				       i);
				err = -EINVAL;
			} else
				pr_debug("controls set for stream %d\n", i);
		}
	}
	return err;
}

int aoc_audio_set_params(struct aoc_alsa_stream *alsa_stream, uint32_t channels,
			 uint32_t samplerate, uint32_t bps, bool pcm_float_fmt, int source_mode)
{
	int err = 0;
	struct aoc_chip *chip = alsa_stream->chip;

	pr_debug("setting channels(%u), samplerate(%u), bits-per-sample(%u)\n", channels,
		 samplerate, bps);

	if (alsa_stream->cstream || alsa_stream->substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		err = aoc_audio_playback_set_params(alsa_stream, channels, samplerate, bps,
						    pcm_float_fmt, source_mode);
		if (err < 0)
			pr_err("ERR:%d playback audio param set fails\n", err);
	} else {
		err = aoc_audio_capture_set_params(alsa_stream, channels, samplerate, bps,
						   pcm_float_fmt);
		if (err < 0) {
			pr_err("ERR:%d capture audio param set fails\n", err);
			goto exit;
		}

		/* To deal with recording with spatial module enabled */
		if (chip->mic_spatial_module_enable) {
			err = aoc_audio_capture_spatial_module_trigger(chip, START);
			if (err < 0)
				pr_err("ERR:%d mic proc spatial module failed to start!\n", err);
		}
	}

exit:
	return err;
}

static int aoc_audio_modem_mic_input(struct aoc_chip *chip,
				 int input_cmd, int mic_input_source)
{
	int err;
	struct CMD_HDR cmd0; /* For modem mic input STOP */
	struct CMD_AUDIO_INPUT_MODEM_INPUT_START cmd1;

	if (input_cmd == START) {
		AocCmdHdrSet(&(cmd1.parent),
			     CMD_AUDIO_INPUT_MODEM_INPUT_START_ID,
			     sizeof(cmd1));

		cmd1.mic_input_source = mic_input_source;
		err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd1,
					sizeof(cmd1), NULL, chip);
		if (err < 0)
			pr_err("ERR:%d modem input start fail!\n", err);

	} else {
		AocCmdHdrSet(&cmd0, CMD_AUDIO_INPUT_MODEM_INPUT_STOP_ID,
			     sizeof(cmd0));

		err = aoc_audio_control(CMD_INPUT_CHANNEL, (uint8_t *)&cmd0,
					sizeof(cmd0), NULL, chip);
		if (err < 0)
			pr_err("ERR:%d modem input stop fail!\n", err);
	}

	return err;
}

int aoc_phonecall_path_open(struct aoc_chip *chip, int src, int dst)
{
	int err;
	int mic_input_source = 0;

	pr_info("Open phone call path - src:%d, dst:%d\n", src, dst);

	if (!chip->voice_call_audio_enable) {
		pr_info("phone call audio NOT enabled\n");
		return 0;
	}

	if (src != 4)
		return 0;

	/* Audio playback enabled for momdem output */
	err = aoc_audio_path_bind(8, dst, START, chip);
	if (err < 0) {
		pr_err("ERR:%d Telephony Downlink bind fail\n", err);
		goto exit;
	}

	/* Audio capture enabled for modem input */
	if (chip->voice_call_mic_source == DEFAULT_MIC) {
		/* Use default mic for voice call based on the playback sink */
		switch (dst) {
		case ASNK_SPEAKER:
			mic_input_source = MODEM_MIC_INPUT_INDEX;
			break;
		case ASNK_BT:
			mic_input_source = MODEM_BT_INPUT_INDEX;
			break;
		case ASNK_USB:
			mic_input_source = MODEM_USB_INPUT_INDEX;
			break;
		default:
			pr_err("ERR in mic input source for voice call, dst=%d\n", dst);
		}
	} else {
		/* Use user-specified mic for voice call independently of the playback sink */
		switch (chip->voice_call_mic_source) {
		case BUILTIN_MIC:
			mic_input_source = MODEM_MIC_INPUT_INDEX;
			break;
		case USB_MIC:
			mic_input_source = MODEM_USB_INPUT_INDEX;
			break;
		case BT_MIC:
			mic_input_source = MODEM_BT_INPUT_INDEX;
			break;
		case IN_CALL_MUSIC:
			mic_input_source = MODEM_INCALL_INPUT_INDEX;
			break;
		default:
			pr_err("ERR in mic input source for voice call, mic source=%d\n",
			       chip->voice_call_mic_source);
		}
	}

	pr_info("voice call mic input source: %d\n", mic_input_source);
	err = aoc_audio_modem_mic_input(chip, START, mic_input_source);
	if (err < 0)
		pr_err("ERR:%d modem input start fail\n", err);

exit:
	return err;
}

int aoc_phonecall_path_close(struct aoc_chip *chip, int src, int dst)
{
	int err;

	pr_info("close phone call path - src:%d, dst:%d\n", src, dst);

	if (!chip->voice_call_audio_enable) {
		pr_info("phone call audio NOT enabled\n");
		return 0;
	}

	if (src != 4)
		return 0;

	/* Audio capture disabled for modem input */
	err = aoc_audio_modem_mic_input(chip, STOP, 0);
	if (err < 0) {
		pr_err("ERR:%d modem input stop fail\n", err);
		goto exit;
	}

	/* Audio playback disabled for modem ouput */
	err = aoc_audio_path_bind(8, dst, STOP, chip);
	if (err < 0)
		pr_err("ERR:%d Telephony Downlink unbind fail\n", err);

exit:
	return err;
}

int aoc_voipcall_path_open(struct aoc_chip *chip, int src, int dst)
{
	int err;
	int mic_input_source = 0;

	pr_info("Open voip call path - src:%d, dst:%d\n", src, dst);

	if (!chip->voice_call_audio_enable) {
		pr_info("phone call audio NOT enabled\n");
		return 0;
	}

	if (dst >= 0) {
		/* Audio playback enabled for momdem output */
		err = aoc_audio_path_bind(8, dst, START, chip);
		if (err < 0) {
			pr_err("ERR:%d Telephony Downlink bind fail\n", err);
			goto exit;
		}
	}
	/* Audio capture enabled for modem input */
	if (chip->voice_call_mic_source == DEFAULT_MIC) {
		/* Use default mic for voice call based on the playback sink */
		switch (dst) {
		case ASNK_SPEAKER:
			mic_input_source = MODEM_MIC_INPUT_INDEX;
			break;
		case ASNK_BT:
			mic_input_source = MODEM_BT_INPUT_INDEX;
			break;
		case ASNK_USB:
			mic_input_source = MODEM_USB_INPUT_INDEX;
			break;
		default:
			pr_err("ERR in mic input source for voice call, dst=%d\n", dst);
		}
	} else {
		/* Use user-specified mic for voice call independently of the playback sink */
		switch (chip->voice_call_mic_source) {
		case BUILTIN_MIC:
			mic_input_source = MODEM_MIC_INPUT_INDEX;
			break;
		case USB_MIC:
			mic_input_source = MODEM_USB_INPUT_INDEX;
			break;
		case BT_MIC:
			mic_input_source = MODEM_BT_INPUT_INDEX;
			break;
		case IN_CALL_MUSIC: /* TODO: do we have this option for voip */
			mic_input_source = MODEM_INCALL_INPUT_INDEX;
			break;
		default:
			pr_err("ERR in mic input source for voice call, mic source=%d\n",
			       chip->voice_call_mic_source);
		}
	}

	pr_info("voice call mic input source: %d\n", mic_input_source);
	err = aoc_audio_modem_mic_input(chip, START, mic_input_source);
	if (err < 0)
		pr_err("ERR:%d modem input start fail\n", err);
exit:
	return err;
}

int aoc_voipcall_path_close(struct aoc_chip *chip, int src, int dst)
{
	int err;

	pr_info("close phone call path - src:%d, dst:%d\n", src, dst);

	if (!chip->voice_call_audio_enable) {
		pr_info("phone call audio NOT enabled\n");
		return 0;
	}

	if (dst >= 0) {
		/* Audio playback disabled for modem ouput */
		err = aoc_audio_path_bind(8, dst, STOP, chip);
		if (err < 0) {
			pr_err("ERR:%d Telephony Downlink unbind fail\n", err);
			goto exit;
		}
	}

	/* Audio capture disabled for modem input */
	err = aoc_audio_modem_mic_input(chip, STOP, 0);
	if (err < 0)
		pr_err("ERR:%d modem input stop fail\n", err);

exit:
	return err;
}

/* TODO: entry point idx and sink id should be specified  in the alsa_stream */
int prepare_phonecall(struct aoc_alsa_stream *alsa_stream)
{
	int err;
	int src = alsa_stream->entry_point_idx;

	/* TODO: check ptrs */
	if (!alsa_stream->chip->voice_call_audio_enable) {
		pr_info("phone call audio NOT enabled\n");
		return 0;
	}

	pr_debug("prepare phone call - dev %d\n", alsa_stream->entry_point_idx);
	if (src != 4)
		return 0;

	/* Binding modem to start audio flow */
	err = aoc_audio_playback_trigger_bind(alsa_stream, START, 3, 3);
	if (err < 0)
		pr_err("ERR:%d Telephony Uplink bind fail\n", err);

	return err;
}

int teardown_phonecall(struct aoc_alsa_stream *alsa_stream)
{
	int err = 0;
	int src = alsa_stream->entry_point_idx;

	if (!alsa_stream->chip->voice_call_audio_enable)
		return 0;

	pr_info("stop phone call - dev %d\n", alsa_stream->entry_point_idx);
	if (src != 4)
		return 0;

	/* Unbinding modem to stop audio flow */
	err = aoc_audio_playback_trigger_bind(alsa_stream, STOP, 3, 3);
	if (err < 0)
		pr_err("ERR:%d Telephony Uplink unbind fail\n", err);

	return err;
}

int prepare_voipcall(struct aoc_alsa_stream *alsa_stream)
{
	int err;
	struct aoc_chip *chip = alsa_stream->chip;

	/* TODO: check ptrs */
	if (!chip->voice_call_audio_enable) {
		pr_info("voip call audio NOT enabled\n");
		return 0;
	}

	if (alsa_stream->substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		chip->voip_tx_prepared = 1;
		if (chip->voip_rx_prepared)
			return 0;
	} else {
		chip->voip_rx_prepared = 1;
		if (chip->voip_tx_prepared)
			return 0;
	}

	pr_debug("prepare voip call - dev %d\n", alsa_stream->entry_point_idx);

	/* Set modem mode to voip call */
	/* Need mixer control for voip sampling rate or source mode */
	aoc_set_sink_mode(chip, 3, 1);

	/* Binding modem to start audio flow */
	err = aoc_audio_playback_trigger_bind(alsa_stream, START, 3, 3);
	if (err < 0)
		pr_err("ERR:%d Telephony Uplink bind fail\n", err);

	return err;
}

int teardown_voipcall(struct aoc_alsa_stream *alsa_stream)
{
	int err = 0;
	struct aoc_chip *chip = alsa_stream->chip;

	if (!chip->voice_call_audio_enable)
		return 0;

	if (alsa_stream->substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		chip->voip_tx_prepared = 0;
		if (chip->voip_rx_prepared)
			return 0;
	} else {
		chip->voip_rx_prepared = 0;
		if (chip->voip_tx_prepared)
			return 0;
	}

	pr_info("stop voip call - dev %d\n", alsa_stream->entry_point_idx);

	/* Unbinding modem to stop audio flow */
	err = aoc_audio_playback_trigger_bind(alsa_stream, STOP, 3, 3);
	if (err < 0)
		pr_err("ERR:%d Telephony Uplink unbind fail\n", err);

	/* TODO: further changes needed */
	aoc_set_sink_mode(chip, 3, 0);

	return err;
}

int aoc_compr_offload_setup(struct aoc_alsa_stream *alsa_stream, int type)
{
	int err;
	struct CMD_AUDIO_OUTPUT_DECODER_CFG cmd;

	/* TODO: refactor may be needed for passing codec info from HAL to AoC */
	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_DECODER_CFG_ID, sizeof(cmd));

	/* TODO: HAL only passes MP3 or AAC, need to consider/test other AAC options */
	cmd.cfg.format = (type == SND_AUDIOCODEC_MP3) ? AUDIO_OUTPUT_DECODER_MP3 :
							AUDIO_OUTPUT_DECODER_AAC_LC;
	cmd.cfg.samplerate = alsa_stream->params_rate;
	cmd.cfg.channels = alsa_stream->channels;
	cmd.address = 0;
	cmd.size = 0;

	pr_info("%s type=%d format=%d sr=%d chan=%d\n", __func__, type, cmd.cfg.format,
		cmd.cfg.samplerate, cmd.cfg.channels);

	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd, sizeof(cmd), NULL,
				alsa_stream->chip);
	if (err < 0) {
		pr_err("ERR:%d in set compress offload codec\n", err);
		return err;
	}

	return 0;
}

int aoc_compr_offload_get_io_samples(struct aoc_alsa_stream *alsa_stream)
{
	int err;
	struct CMD_AUDIO_OUTPUT_GET_EP_SAMPLES cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_GET_EP_TOT_SAMPLES_ID,
		     sizeof(cmd));
	cmd.source = alsa_stream->entry_point_idx;

	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd,
				sizeof(cmd), (uint8_t *)&cmd,
				alsa_stream->chip);
	if (err < 0)
		pr_err("ERR:%d in getting compress offload io-sample number\n",
		       err);

	return err < 0 ? err : cmd.samples;
}

int aoc_compr_offload_flush_buffer(struct aoc_alsa_stream *alsa_stream)
{
	int err;
	struct CMD_HDR cmd;

	AocCmdHdrSet(&cmd, CMD_AUDIO_OUTPUT_DECODE_FLUSH_RB_ID, sizeof(cmd));

	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd,
				sizeof(cmd), NULL, alsa_stream->chip);
	if (err < 0)
		pr_err("ERR:%d flush compress offload buffer fail!\n", err);

	return err;
}

int aoc_compr_pause(struct aoc_alsa_stream *alsa_stream)
{
	int err;

	err = aoc_audio_stop(alsa_stream);
	if (err < 0)
		pr_err("ERR:%d aoc_compr_pause fail\n", err);

	return 0;
}

int aoc_compr_resume(struct aoc_alsa_stream *alsa_stream)
{
	int err;

	err = aoc_audio_start(alsa_stream);
	if (err < 0)
		pr_err("ERR:%d aoc_compr_resume fail\n", err);

	return 0;
}

int aoc_audio_setup(struct aoc_alsa_stream *alsa_stream)
{
	return 0;
}

int aoc_audio_open(struct aoc_alsa_stream *alsa_stream)
{
	return 0;
}

int aoc_audio_close(struct aoc_alsa_stream *alsa_stream)
{
	int err = 0;
	struct aoc_chip *chip = alsa_stream->chip;
	struct snd_pcm_substream *substream = alsa_stream->substream;

	/* To deal with recording with spatial module enabled */
	if (substream && substream->stream == SNDRV_PCM_STREAM_CAPTURE &&
	    chip->mic_spatial_module_enable) {
		err = aoc_audio_capture_spatial_module_trigger(chip, STOP);
		if (err < 0)
			pr_err("ERR:%d mic proc spatial module failed to stop!\n", err);
	}

	return 0;
}

static void print_enc_param(struct AUDIO_OUTPUT_BT_A2DP_ENC_CFG *enc_cfg)
{
	int i;

	pr_info("codecType = %x\n", enc_cfg->codecType);
	pr_info("bitrate = %x\n", enc_cfg->bitrate);
	pr_info("peerMTU = %x\n", enc_cfg->peerMTU);
	for (i = 0;i < 6;i ++)
		pr_info("  params[%d] = %x\n", i, enc_cfg->params[i]);
}

int aoc_a2dp_set_enc_param(struct aoc_chip *chip, struct AUDIO_OUTPUT_BT_A2DP_ENC_CFG *cfg)
{
	int err = 0;
	struct CMD_AUDIO_OUTPUT_BT_A2DP_ENC_CFG cmd;

	AocCmdHdrSet(&(cmd.parent), CMD_AUDIO_OUTPUT_BT_A2DP_ENC_CFG_ID,
		     sizeof(cmd));
	memcpy(&cmd.bt_a2dp_enc_cfg, cfg, sizeof(*cfg));

	print_enc_param(&cmd.bt_a2dp_enc_cfg);

	err = aoc_audio_control(CMD_OUTPUT_CHANNEL, (uint8_t *)&cmd,
				sizeof(cmd), (uint8_t *)&cmd,
				chip);

	if (err < 0)
		pr_err("ERR:%d set enc parameter failed\n", err);

	return err;
}


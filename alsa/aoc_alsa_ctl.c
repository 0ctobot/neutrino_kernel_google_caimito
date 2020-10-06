// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Whitechapel AoC ALSA Driver - Mixer controls
 *
 * Copyright (c) 2019 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "aoc_alsa.h"

/* Volume maximum and minimum */
#define CTRL_VOL_MIN 0
#define CTRL_VOL_MAX 1000

static int snd_aoc_ctl_info(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_info *uinfo)
{
	if (kcontrol->private_value == PCM_PLAYBACK_VOLUME) {
		uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
		uinfo->count = 1;
		uinfo->value.integer.min = CTRL_VOL_MIN;
		uinfo->value.integer.max = CTRL_VOL_MAX;
	} else if (kcontrol->private_value == PCM_PLAYBACK_MUTE) {
		uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
		uinfo->count = 1;
		uinfo->value.integer.min = 0;
		uinfo->value.integer.max = 1;
	} else if (kcontrol->private_value == BUILDIN_MIC_POWER_STATE) {
		uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
		uinfo->count = NUM_OF_BUILTIN_MIC;
		uinfo->value.integer.min = 0;
		uinfo->value.integer.max = 1;
	} else if (kcontrol->private_value == BUILDIN_MIC_CAPTURE_LIST) {
		uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
		uinfo->count = NUM_OF_BUILTIN_MIC;
		uinfo->value.integer.min = -1;
		uinfo->value.integer.max = NUM_OF_BUILTIN_MIC - 1;
	}
	return 0;
}

/*
 * Toggle mute on/off depending on the value of nmute, and returns
 * 1 if the mute value was changed, otherwise 0
 */
static int toggle_mute(struct aoc_chip *chip, int nmute)
{
	if (chip->mute == nmute)
		return 0;

	if (chip->mute == CTRL_VOL_MUTE) {
		chip->volume = chip->old_volume;
		pr_debug("Unmuting, old_volume = %d, volume = %d\n",
			 chip->old_volume, chip->volume);
	} else {
		chip->old_volume = chip->volume;
		chip->volume = 0;
		pr_debug("Muting, old_volume = %d, volume = %d\n",
			 chip->old_volume, chip->volume);
	}

	chip->mute = nmute;
	return 1;
}

static int snd_aoc_ctl_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	BUG_ON(!chip && !(chip->avail_substreams & AVAIL_SUBSTREAMS_MASK));

	if (kcontrol->private_value == PCM_PLAYBACK_VOLUME)
		ucontrol->value.integer.value[0] = chip2alsa(chip->volume);
	else if (kcontrol->private_value == PCM_PLAYBACK_MUTE)
		ucontrol->value.integer.value[0] = chip->mute;

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int snd_aoc_ctl_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);
	int changed = 0;

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	if (kcontrol->private_value == PCM_PLAYBACK_VOLUME) {
		pr_debug(
			"volume change attempted.. volume = %d new_volume = %d\n",
			chip->volume, (int)ucontrol->value.integer.value[0]);
		if (chip->mute == CTRL_VOL_MUTE) {
			changed = 1;
			goto unlock;
		}
		if (changed || (ucontrol->value.integer.value[0] !=
				chip2alsa(chip->volume))) {
			chip->volume =
				alsa2chip(ucontrol->value.integer.value[0]);
			changed = 1;
		}
	} else if (kcontrol->private_value == PCM_PLAYBACK_MUTE) {
		pr_debug("mute attempted\n");
		changed = toggle_mute(chip, ucontrol->value.integer.value[0]);
	}

	if (changed) {
		if (aoc_audio_set_ctls(chip))
			pr_err("ERR: fail in set ALSA controls\n");
	}

unlock:
	mutex_unlock(&chip->audio_mutex);
	return changed;
}

static int
snd_aoc_buildin_mic_power_ctl_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	int i;
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	for (i = 0; i < NUM_OF_BUILTIN_MIC; i++)
		ucontrol->value.integer.value[i] =
			aoc_get_builtin_mic_power_state(chip, i);

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int
snd_aoc_buildin_mic_power_ctl_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	int i;
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	for (i = 0; i < NUM_OF_BUILTIN_MIC; i++)
		aoc_set_builtin_mic_power_state(
			chip, i, ucontrol->value.integer.value[i]);

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int
snd_aoc_buildin_mic_capture_list_ctl_get(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	int i;
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	for (i = 0; i < NUM_OF_BUILTIN_MIC; i++)
		ucontrol->value.integer.value[i] =
			chip->buildin_mic_id_list[i]; // geting power state;

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int
snd_aoc_buildin_mic_capture_list_ctl_put(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	int i;
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	for (i = 0; i < NUM_OF_BUILTIN_MIC; i++)
		chip->buildin_mic_id_list[i] =
			ucontrol->value.integer.value[i]; // geting power state;

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int mic_power_ctl_get(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	u32 mic_idx = (u32)mc->shift;

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	/* geting power statef from AoC ; */
	ucontrol->value.integer.value[0] =
		aoc_get_builtin_mic_power_state(chip, mic_idx);

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int mic_power_ctl_set(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	u32 mic_idx = (u32)mc->shift;

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	aoc_set_builtin_mic_power_state(chip, mic_idx,
					ucontrol->value.integer.value[0]);

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int mic_clock_rate_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	ucontrol->value.integer.value[0] = aoc_mic_clock_rate_get(chip);

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int mic_hw_gain_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);
	struct soc_mixer_control *mc = (struct soc_mixer_control *)kcontrol->private_value;
	u32 state = (u32)mc->shift;

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	ucontrol->value.integer.value[0] = aoc_mic_hw_gain_get(chip, state);

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int mic_dc_blocker_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	ucontrol->value.integer.value[0] = aoc_mic_dc_blocker_get(chip);

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int mic_dc_blocker_set(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	aoc_mic_dc_blocker_set(chip, ucontrol->value.integer.value[0]);

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int voice_call_mic_mute_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	ucontrol->value.integer.value[0] = chip->voice_call_mic_mute;

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int voice_call_mic_mute_set(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	if (chip->voice_call_mic_mute != ucontrol->value.integer.value[0]) {
		chip->voice_call_mic_mute = ucontrol->value.integer.value[0];
		aoc_voice_call_mic_mute(chip, ucontrol->value.integer.value[0]);
	}

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int voice_call_audio_enable_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	ucontrol->value.integer.value[0] = chip->voice_call_audio_enable;

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int voice_call_audio_enable_set(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	if (chip->voice_call_audio_enable != ucontrol->value.integer.value[0]) {
		chip->voice_call_audio_enable = ucontrol->value.integer.value[0];
	}

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static const char *dsp_state_texts[] = { "Idle", "Playback", "Telephony" };

static int aoc_dsp_state_ctl_info(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(dsp_state_texts),
				 dsp_state_texts);
}

static int aoc_dsp_state_ctl_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	ucontrol->value.enumerated.item[0] = aoc_get_dsp_state(chip);

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

static int aoc_sink_state_ctl_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct aoc_chip *chip = snd_kcontrol_chip(kcontrol);
	struct soc_enum *mc = (struct soc_enum *)kcontrol->private_value;
	u32 sink_idx = (u32)mc->shift_l;

	if (mutex_lock_interruptible(&chip->audio_mutex))
		return -EINTR;

	ucontrol->value.enumerated.item[0] = aoc_get_sink_state(chip, sink_idx);
	pr_debug("sink %d - %d\n", sink_idx,
		 ucontrol->value.enumerated.item[0]);

	mutex_unlock(&chip->audio_mutex);
	return 0;
}

/* TODO: seek better way to create a series of controls  */
static const char *sink_processing_state_texts[] = { "Idle", "Active",
						     "Bypass" };
static SOC_ENUM_SINGLE_DECL(sink_0_state_enum, 1, 0,
			    sink_processing_state_texts);
static SOC_ENUM_SINGLE_DECL(sink_1_state_enum, 1, 1,
			    sink_processing_state_texts);
static SOC_ENUM_SINGLE_DECL(sink_2_state_enum, 1, 2,
			    sink_processing_state_texts);
static SOC_ENUM_SINGLE_DECL(sink_3_state_enum, 1, 3,
			    sink_processing_state_texts);
static SOC_ENUM_SINGLE_DECL(sink_4_state_enum, 1, 4,
			    sink_processing_state_texts);

static struct snd_kcontrol_new snd_aoc_ctl[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "PCM Playback Volume",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.private_value = PCM_PLAYBACK_VOLUME,
		.info = snd_aoc_ctl_info,
		.get = snd_aoc_ctl_get,
		.put = snd_aoc_ctl_put,
		.count = 1,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "PCM Playback Switch",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.private_value = PCM_PLAYBACK_MUTE,
		.info = snd_aoc_ctl_info,
		.get = snd_aoc_ctl_get,
		.put = snd_aoc_ctl_put,
		.count = 1,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "BUILDIN MIC POWER STATE",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.private_value = BUILDIN_MIC_POWER_STATE,
		.info = snd_aoc_ctl_info,
		.get = snd_aoc_buildin_mic_power_ctl_get,
		.put = snd_aoc_buildin_mic_power_ctl_put,
		.count = 1,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "BUILDIN MIC ID CAPTURE LIST",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.private_value = BUILDIN_MIC_CAPTURE_LIST,
		.info = snd_aoc_ctl_info,
		.get = snd_aoc_buildin_mic_capture_list_ctl_get,
		.put = snd_aoc_buildin_mic_capture_list_ctl_put,
		.count = 1,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Audio DSP State",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = aoc_dsp_state_ctl_info,
		.get = aoc_dsp_state_ctl_get,
		.count = 1,
	},

	SOC_ENUM_EXT("Audio Sink 0 Processing State", sink_0_state_enum,
		     aoc_sink_state_ctl_get, aoc_sink_state_ctl_get),
	SOC_ENUM_EXT("Audio Sink 1 Processing State", sink_1_state_enum,
		     aoc_sink_state_ctl_get, aoc_sink_state_ctl_get),
	SOC_ENUM_EXT("Audio Sink 2 Processing State", sink_2_state_enum,
		     aoc_sink_state_ctl_get, aoc_sink_state_ctl_get),
	SOC_ENUM_EXT("Audio Sink 3 Processing State", sink_3_state_enum,
		     aoc_sink_state_ctl_get, aoc_sink_state_ctl_get),
	SOC_ENUM_EXT("Audio Sink 4 Processing State", sink_4_state_enum,
		     aoc_sink_state_ctl_get, aoc_sink_state_ctl_get),

	SOC_SINGLE_EXT("Voice Call Mic Mute", SND_SOC_NOPM, 0, 1, 0,
		       voice_call_mic_mute_get, voice_call_mic_mute_set),
	SOC_SINGLE_EXT("Voice Call Audio Enable", SND_SOC_NOPM, 0, 1, 0,
		       voice_call_audio_enable_get, voice_call_audio_enable_set),

	SOC_SINGLE_EXT("MIC0", SND_SOC_NOPM, BUILTIN_MIC0, 1, 0,
		       mic_power_ctl_get, mic_power_ctl_set),
	SOC_SINGLE_EXT("MIC1", SND_SOC_NOPM, BUILTIN_MIC1, 1, 0,
		       mic_power_ctl_get, mic_power_ctl_set),
	SOC_SINGLE_EXT("MIC2", SND_SOC_NOPM, BUILTIN_MIC2, 1, 0,
		       mic_power_ctl_get, mic_power_ctl_set),
	SOC_SINGLE_EXT("MIC3", SND_SOC_NOPM, BUILTIN_MIC3, 1, 0,
		       mic_power_ctl_get, mic_power_ctl_set),

	SOC_SINGLE_EXT("MIC Clock Rate", SND_SOC_NOPM, 0, 20000000, 0,
		       mic_clock_rate_get, NULL),
	SOC_SINGLE_EXT("MIC DC Blocker", SND_SOC_NOPM, 0, 1, 0,
		       mic_dc_blocker_get, mic_dc_blocker_set),

	SOC_SINGLE_EXT("MIC HW Gain in cB (Low Power Mode)", SND_SOC_NOPM,
		       MIC_LOW_POWER_GAIN, 4096, 0, mic_hw_gain_get, NULL),
	SOC_SINGLE_EXT("MIC HW Gain in cB (High Power Mode)", SND_SOC_NOPM,
		       MIC_HIGH_POWER_GAIN, 4096, 0, mic_hw_gain_get, NULL),
	SOC_SINGLE_EXT("MIC HW Gain in cB (Current)", SND_SOC_NOPM,
		       MIC_CURRENT_GAIN, 4096, 0, mic_hw_gain_get, NULL),
	SOC_SINGLE_EXT("MIC Recording Gain (dB)", SND_SOC_NOPM, 0, 100, 0, NULL,
		       NULL),
	SOC_SINGLE_EXT("Compress Offload Volume", SND_SOC_NOPM, 0, 100, 0, NULL,
		       NULL),
	SOC_SINGLE_EXT("Voice Call Rx Volume", SND_SOC_NOPM, 0, 100, 0, NULL,
		       NULL),
	SOC_SINGLE_EXT("VOIP Rx Volume", SND_SOC_NOPM, 0, 100, 0, NULL, NULL),
};

int snd_aoc_new_ctl(struct aoc_chip *chip)
{
	int err;
	unsigned int idx;

	strcpy(chip->card->mixername, "Aoc Mixer");
	for (idx = 0; idx < ARRAY_SIZE(snd_aoc_ctl); idx++) {
		err = snd_ctl_add(chip->card,
				  snd_ctl_new1(&snd_aoc_ctl[idx], chip));
		if (err < 0)
			return err;
	}

	return 0;
}
EXPORT_SYMBOL(snd_aoc_new_ctl);

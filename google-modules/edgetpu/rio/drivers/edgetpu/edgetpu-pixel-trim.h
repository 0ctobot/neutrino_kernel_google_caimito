/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Support for Pixel trim subsystem, if enabled.
 *
 * Copyright (C) 2025 Google LLC
 */

#ifndef __EDGETPU_PIXEL_TRIM_H__
#define __EDGETPU_PIXEL_TRIM_H__

#include "edgetpu-internal.h"

#if IS_ENABLED(CONFIG_PIXEL_TRIM)
#include <pixel_trim.h>

struct edgetpu_pixel_trim {
	bool registered;	/* true if successfully registered with pixel_trim */
	struct pixel_trim pixel_trim;
};

void edgetpu_pixel_trim_register(struct edgetpu_dev *etdev);
void edgetpu_pixel_trim_unregister(struct edgetpu_dev *etdev);

#else /* !IS_ENABLED(CONFIG_PIXEL_TRIM) */

struct edgetpu_pixel_trim {};

static inline void edgetpu_pixel_trim_register(struct edgetpu_dev *etdev) { }
static inline void edgetpu_pixel_trim_unregister(struct edgetpu_dev *etdev) {}

#endif /* IS_ENABLED(CONFIG_PIXEL_TRIM) */

#endif /* __EDGETPU_PIXEL_TRIM_H__ */

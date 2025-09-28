/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Platform device driver for inter-IP fence (IIF).
 *
 * Copyright (C) 2025 Google LLC
 */

#ifndef __IIF_IIF_PLATFORM_H__
#define __IIF_IIF_PLATFORM_H__

#include <linux/platform_device.h>

#define IIF_DRIVER_NAME "iif"

const char *iif_platform_get_driver_commit(void);

/**
 * iif_platform_is_iif_device() - Checks whether a platform_device is an IIF device.
 * @pdev: The platform_device to check.
 *
 * Returns true if @pdev is an IIF device.
 */
bool iif_platform_is_iif_device(struct platform_device *pdev);

#endif /* __IIF_IIF_PLATFORM_H__ */

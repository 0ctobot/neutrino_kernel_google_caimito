/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Define configuration macros.
 *
 * Copyright (C) 2023 Google LLC
 */

#ifndef __GCIP_CONFIG_H__
#define __GCIP_CONFIG_H__

#include <linux/version.h>

/* Macros to check the availability of features and APIs */

/* TODO(b/298697777): temporarily check 6.1.25 until previous kernel version no longer in use. */
#define GCIP_HAS_VMA_FLAGS_API (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 25))

#define GCIP_IOMMU_MAP_HAS_GFP (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0))

#endif /* __GCIP_CONFIG_H__ */

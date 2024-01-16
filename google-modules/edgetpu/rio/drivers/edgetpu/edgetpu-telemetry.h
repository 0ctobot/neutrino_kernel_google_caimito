/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Edge TPU ML accelerator telemetry: logging and tracing.
 *
 * Copyright (C) 2019-2020 Google, Inc.
 */
#ifndef __EDGETPU_TELEMETRY_H__
#define __EDGETPU_TELEMETRY_H__

#include <linux/mm_types.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>

#include <gcip/gcip-telemetry.h>

#include "edgetpu-internal.h"
#include "edgetpu-kci.h"

/* Buffer size must be a power of 2 */
#define EDGETPU_TELEMETRY_LOG_BUFFER_SIZE (16 * 4096)
#define EDGETPU_TELEMETRY_TRACE_BUFFER_SIZE (64 * 4096)
#define EDGETPU_TELEMETRY_HWTRACE_BUFFER_DEFAULT_SIZE (SZ_1M * 64)

/*
 * HWTRACE (and other optional debug features) use the 32-bit IOVA space above the FW carveout
 * remap region.
 */
#define EDGETPU_TELEMETRY_HWTRACE_IOVA	0x18000000

/* Return true if the telemetry entry has a TPU mapping established, else false. */
static inline bool edgetpu_telemetry_mapped(struct gcip_telemetry *tel)
{
	return tel->memory.dma_addr;
}

#if IS_ENABLED(CONFIG_EDGETPU_TELEMETRY_TRACE)
/*
 * Allocates resources needed for @etdev->telemetry LOG and TRACE.
 *
 * Returns 0 on success, or a negative errno on error.
 */
int edgetpu_telemetry_init(struct edgetpu_dev *etdev);

/*
 * Allocates resources needed for @etdev->telemetry HWTRACE.
 * @buffer_size: Size in bytes of buffer, must be a power of 2 and >= telemetry header size (128)
 *
 * Returns 0 on success, or a negative errno on error.
 */
int edgetpu_telemetry_hwtrace_init(struct edgetpu_dev *etdev, size_t buffer_size);

/*
 * Disable the telemetry if enabled, release resources, including LOG, TRACE, and HWTRACE.
 */
void edgetpu_telemetry_exit(struct edgetpu_dev *etdev);

/*
 * Sends the KCI commands mapping LOG and TRACE (and HWTRACE if enabled) telemetry buffers to
 * firmware.
 *
 * Returns the code of KCI response, or a negative errno on error.
 */
int edgetpu_telemetry_kci(struct edgetpu_dev *etdev);

/*
 * Sets the eventfd to notify the runtime when LOG/TRACE/HWTRACE telemetry data is available.
 *
 * Returns 0 on success, or a negative errno on error.
 */
int edgetpu_telemetry_set_event(struct edgetpu_dev *etdev, struct gcip_telemetry *tel, u32 eventfd);
/* Removes previously set event. */
void edgetpu_telemetry_unset_event(struct edgetpu_dev *etdev, struct gcip_telemetry *tel);

/* Checks LOG/TRACE/HWTRACE telemetries and signals associated eventfds if needed. */
void edgetpu_telemetry_irq_handler(struct edgetpu_dev *etdev);

/* debugfs mappings dump */
void edgetpu_telemetry_mappings_show(struct edgetpu_dev *etdev,
				     struct seq_file *s);

/*
 * Map telemetry buffer into user space.
 */
int edgetpu_mmap_telemetry_buffer(struct edgetpu_dev *etdev, struct gcip_telemetry *tel,
				  struct vm_area_struct *vma);
#else
static inline
int edgetpu_telemetry_init(struct edgetpu_dev *etdev)
{
	return 0;
}

static inline
int edgetpu_telemetry_hwtrace_init(struct edgetpu_dev *etdev, size_t buffer_size)
{
	return 0;
}

static inline
void edgetpu_telemetry_exit(struct edgetpu_dev *etdev)
{
}

static inline
int edgetpu_telemetry_kci(struct edgetpu_dev *etdev)
{
	return 0;
}

static inline
int edgetpu_telemetry_set_event(struct edgetpu_dev *etdev, struct gcip_telemetry *tel,
                                u32 eventfd)
{
	return 0;
}

static inline
void edgetpu_telemetry_unset_event(struct edgetpu_dev *etdev, struct gcip_telemetry *tel)
{
}

static inline
void edgetpu_telemetry_irq_handler(struct edgetpu_dev *etdev)
{
}

static inline
void edgetpu_telemetry_mappings_show(struct edgetpu_dev *etdev,
                                     struct seq_file *s)
{
}

static inline
int edgetpu_mmap_telemetry_buffer(struct edgetpu_dev *etdev, struct gcip_telemetry *tel,
                                  struct vm_area_struct *vma)
{
	return -ENODEV;
}
#endif

#endif /* __EDGETPU_TELEMETRY_H__ */

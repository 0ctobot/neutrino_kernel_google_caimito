/* BEGIN-INTERNAL */
/*
 * Google LWIS IOCTL Commands and Data Structures
 *
 * Copyright (c) 2018 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
/* END-INTERNAL */
#ifndef LWIS_COMMANDS_H_
#define LWIS_COMMANDS_H_

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#endif /* __KERNEL__ */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/*
 *  IOCTL Types and Data Structures
 */

struct lwis_device_info {
};

struct lwis_buffer_info {
	// IOCTL input for BUFFER_ENROLL
	int fd;
	bool dma_read;
	bool dma_write;
	// IOCTL output for BUFFER_ENROLL, input for BUFFER_DISENROLL
	uint64_t dma_vaddr;
};

struct lwis_io_data {
	uint64_t offset;
	uint64_t val;
	int access_size;
};

struct lwis_io_msg {
	int bid;
	struct lwis_io_data *buf;
	int num_entries;
	int offset_bitwidth;
};

/* The first 4096 event IDs are reserved for generic events shared by all
 * devices.
 *
 * The rest are specific to device specializations
 */
#define LWIS_EVENT_ID_INVALID 0
#define LWIS_EVENT_ID_HEARTBEAT 1
// ...
#define LWIS_EVENT_ID_START_OF_SPECIALIZED_RANGE 4096

struct lwis_event_info {
	// IOCTL Inputs
	size_t payload_buffer_size;
	void *payload_buffer;
	// IOCTL Outputs
	int64_t event_id;
	uint64_t event_counter;
	uint64_t timestamp_ns;
	size_t payload_size;
};

#define LWIS_EVENT_CONTROL_FLAG_ENABLE (1ULL << 0)
#define LWIS_EVENT_CONTROL_FLAG_QUEUE_ENABLE (1ULL << 1)

struct lwis_event_control {
	// IOCTL Inputs
	int64_t event_id;
	// IOCTL Outputs
	uint64_t flags;
};

/*
 *  IOCTL Commands
 */

#define LWIS_IOC_TYPE 'L'

#define LWIS_GET_DEVICE_INFO _IOWR(LWIS_IOC_TYPE, 1, struct lwis_device_info)
#define LWIS_BUFFER_ENROLL _IOWR(LWIS_IOC_TYPE, 2, struct lwis_buffer_info)
#define LWIS_BUFFER_DISENROLL _IOWR(LWIS_IOC_TYPE, 3, struct lwis_buffer_info)
#define LWIS_REG_READ _IOWR(LWIS_IOC_TYPE, 4, struct lwis_io_msg)
#define LWIS_REG_WRITE _IOWR(LWIS_IOC_TYPE, 5, struct lwis_io_msg)
#define LWIS_DEVICE_ENABLE _IO(LWIS_IOC_TYPE, 6)
#define LWIS_DEVICE_DISABLE _IO(LWIS_IOC_TYPE, 7)

#define LWIS_EVENT_CONTROL_GET                                                 \
	_IOWR(LWIS_IOC_TYPE, 20, struct lwis_event_control)
#define LWIS_EVENT_CONTROL_SET                                                 \
	_IOW(LWIS_IOC_TYPE, 21, struct lwis_event_control)
#define LWIS_EVENT_DEQUEUE _IOWR(LWIS_IOC_TYPE, 22, struct lwis_event_info)

#ifdef __cplusplus
}  /* extern "C" */
#endif /* __cplusplus */

#endif /* LWIS_COMMANDS_H_ */

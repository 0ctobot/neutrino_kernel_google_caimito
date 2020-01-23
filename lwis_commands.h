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

/*
 * lwis_device_types
 * top  : top level device that overlooks all the LWIS devices. Will be used to
 *        list the information of the other LWIS devices in the system.
 * i2c  : for controlling i2c devices
 * ioreg: for controlling mapped register I/O devices
 */
enum lwis_device_types {
	DEVICE_TYPE_UNKNOWN = -1,
	DEVICE_TYPE_TOP = 0,
	DEVICE_TYPE_I2C,
	DEVICE_TYPE_IOREG,
	NUM_DEVICE_TYPES
};

/* Device tree strings have a maximum length of 31, according to specs.
   Adding 1 byte for the null character. */
#define LWIS_MAX_DEVICE_NAME_STRING 32

struct lwis_device_info {
	int id;
	enum lwis_device_types type;
	char name[LWIS_MAX_DEVICE_NAME_STRING];
};

enum lwis_dma_alloc_flags {
	// Allocates a cached buffer.
	LWIS_DMA_BUFFER_CACHED = 1UL << 0,
	// Allocates a buffer which is not initialized to 0 to avoid
	// initialization overhead.
	LWIS_DMA_BUFFER_UNINITIALIZED = 1UL << 1,
	// Allocates a buffer which is stored in contiguous memory.
	LWIS_DMA_BUFFER_CONTIGUOUS = 1UL << 2,
};

struct lwis_alloc_buffer_info {
	// IOCTL input for BUFFER_ALLOC
	size_t size;
	uint32_t flags; // lwis_dma_alloc_flags
	// IOCTL output for BUFFER_ALLOC
	int dma_fd;
};

struct lwis_buffer_info {
	// IOCTL input for BUFFER_ENROLL
	int fd;
	bool dma_read;
	bool dma_write;
	// IOCTL output for BUFFER_ENROLL
	uint64_t dma_vaddr;
};

enum lwis_io_entry_types {
	LWIS_IO_ENTRY_READ,
	LWIS_IO_ENTRY_READ_BATCH,
	LWIS_IO_ENTRY_WRITE,
	LWIS_IO_ENTRY_WRITE_BATCH,
	LWIS_IO_ENTRY_MODIFY,
};

// For io_entry read and write types.
struct lwis_io_entry_rw {
	int bid;
	uint64_t offset;
	uint64_t val;
};

struct lwis_io_entry_rw_batch {
	int bid;
	uint64_t offset;
	size_t size_in_bytes;
	uint8_t *buf;
};

// For io_entry modify types.
struct lwis_io_entry_modify {
	int bid;
	uint64_t offset;
	uint64_t val;
	uint64_t val_mask;
};

struct lwis_io_entry {
	int type;
	union {
		struct lwis_io_entry_rw rw;
		struct lwis_io_entry_rw_batch rw_batch;
		struct lwis_io_entry_modify mod;
	};
};

/* The first 4096 event IDs are reserved for generic events shared by all
 * devices.
 *
 * The rest are specific to device specializations
 */
// Event NONE and INVALID are intended to be sharing the same ID.
#define LWIS_EVENT_ID_NONE 0
#define LWIS_EVENT_ID_INVALID 0
#define LWIS_EVENT_ID_HEARTBEAT 1
// ...
#define LWIS_EVENT_ID_START_OF_SPECIALIZED_RANGE 4096

// Event flags used for transaction events.
#define LWIS_TRANSACTION_EVENT_FLAG (1ULL << 63)
#define LWIS_TRANSACTION_FAILURE_EVENT_FLAG (1ULL << 62)

struct lwis_event_info {
	// IOCTL Inputs
	size_t payload_buffer_size;
	void *payload_buffer;
	// IOCTL Outputs
	int64_t event_id;
	int64_t event_counter;
	int64_t timestamp_ns;
	size_t payload_size;
};

#define LWIS_EVENT_CONTROL_FLAG_IRQ_ENABLE (1ULL << 0)
#define LWIS_EVENT_CONTROL_FLAG_QUEUE_ENABLE (1ULL << 1)

struct lwis_event_control {
	// IOCTL Inputs
	int64_t event_id;
	// IOCTL Outputs
	uint64_t flags;
};

#define LWIS_EVENT_COUNTER_ON_NEXT_OCCURRENCE (-1LL)
struct lwis_transaction_info {
	// Input
	int trigger_device_id;
	int64_t trigger_event_id;
	int64_t trigger_event_counter;
	size_t num_io_entries;
	struct lwis_io_entry *io_entries;
	bool run_in_event_context;
	int64_t emit_success_event_id;
	int64_t emit_error_event_id;
	// Output
	int64_t id;
};

// Actual size of this struct depends on num_entries
struct lwis_transaction_response_header {
	int64_t id;
	int error_code;
	size_t num_entries;
	size_t results_size_bytes;
};

struct lwis_io_result {
	int bid;
	uint64_t offset;
	size_t num_value_bytes;
	uint8_t values[];
};

/*
 *  IOCTL Commands
 */

#define LWIS_IOC_TYPE 'L'

#define LWIS_GET_DEVICE_INFO _IOWR(LWIS_IOC_TYPE, 1, struct lwis_device_info)
#define LWIS_BUFFER_ENROLL _IOWR(LWIS_IOC_TYPE, 2, struct lwis_buffer_info)
#define LWIS_BUFFER_DISENROLL _IOWR(LWIS_IOC_TYPE, 3, uint64_t)
#define LWIS_REG_READ _IOWR(LWIS_IOC_TYPE, 4, struct lwis_io_entry)
#define LWIS_REG_WRITE _IOWR(LWIS_IOC_TYPE, 5, struct lwis_io_entry)
#define LWIS_DEVICE_ENABLE _IO(LWIS_IOC_TYPE, 6)
#define LWIS_DEVICE_DISABLE _IO(LWIS_IOC_TYPE, 7)
#define LWIS_BUFFER_ALLOC _IOWR(LWIS_IOC_TYPE, 8, struct lwis_alloc_buffer_info)
#define LWIS_BUFFER_FREE _IOWR(LWIS_IOC_TYPE, 9, int)
#define LWIS_TIME_QUERY _IOWR(LWIS_IOC_TYPE, 10, int64_t)

#define LWIS_EVENT_CONTROL_GET                                                 \
	_IOWR(LWIS_IOC_TYPE, 20, struct lwis_event_control)
#define LWIS_EVENT_CONTROL_SET                                                 \
	_IOW(LWIS_IOC_TYPE, 21, struct lwis_event_control)
#define LWIS_EVENT_DEQUEUE _IOWR(LWIS_IOC_TYPE, 22, struct lwis_event_info)

#define LWIS_TRANSACTION_SUBMIT                                                \
	_IOWR(LWIS_IOC_TYPE, 30, struct lwis_transaction_info)
#define LWIS_TRANSACTION_CANCEL _IOWR(LWIS_IOC_TYPE, 31, int64_t)

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* LWIS_COMMANDS_H_ */

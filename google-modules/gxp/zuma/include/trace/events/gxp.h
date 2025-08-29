/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Trace events for gxp
 *
 * Copyright (c) 2023-2024 Google LLC
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM gxp

#if !defined(_TRACE_GXP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_GXP_H

#include <linux/stringify.h>
#include <linux/tracepoint.h>

#define GXP_TRACE_SYSTEM __stringify(TRACE_SYSTEM)

TRACE_EVENT(gxp_mapping_create_start,

	    TP_PROTO(u64 user_address, size_t size),

	    TP_ARGS(user_address, size),

	    TP_STRUCT__entry(__field(u64, user_address) __field(size_t, size)),

	    TP_fast_assign(__entry->user_address = user_address; __entry->size = size;),

	    TP_printk("user_address = %#llX, size = %ld", __entry->user_address, __entry->size));

TRACE_EVENT(gxp_mapping_create_end,

	    TP_PROTO(u64 user_address, size_t size, int nents),

	    TP_ARGS(user_address, size, nents),

	    TP_STRUCT__entry(__field(u64, user_address) __field(size_t, size) __field(int, nents)),

	    TP_fast_assign(__entry->user_address = user_address; __entry->size = size;
			   __entry->nents = nents;),

	    TP_printk("user_address = %#llX, size = %ld, nents = %d", __entry->user_address,
		      __entry->size, __entry->nents));

TRACE_EVENT(gxp_mapping_destroy_start,

	    TP_PROTO(dma_addr_t device_address, size_t size),

	    TP_ARGS(device_address, size),

	    TP_STRUCT__entry(__field(dma_addr_t, device_address) __field(size_t, size)),

	    TP_fast_assign(__entry->device_address = device_address; __entry->size = size;),

	    TP_printk("device_address = %#llX, size = %ld", __entry->device_address,
		      __entry->size));

TRACE_EVENT(gxp_mapping_destroy_end,

	    TP_PROTO(dma_addr_t device_address, size_t size),

	    TP_ARGS(device_address, size),

	    TP_STRUCT__entry(__field(dma_addr_t, device_address) __field(size_t, size)),

	    TP_fast_assign(__entry->device_address = device_address; __entry->size = size;),

	    TP_printk("device_address = %#llX, size = %ld", __entry->device_address,
		      __entry->size));

TRACE_EVENT(gxp_dmabuf_mapping_create_start,

	    TP_PROTO(int fd),

	    TP_ARGS(fd),

	    TP_STRUCT__entry(__field(int, fd)),

	    TP_fast_assign(__entry->fd = fd;),

	    TP_printk("fd = %d", __entry->fd));

TRACE_EVENT(gxp_dmabuf_mapping_create_end,

	    TP_PROTO(dma_addr_t device_address, size_t size),

	    TP_ARGS(device_address, size),

	    TP_STRUCT__entry(__field(dma_addr_t, device_address) __field(size_t, size)),

	    TP_fast_assign(__entry->device_address = device_address; __entry->size = size;),

	    TP_printk("device_address = %#llX, size = %ld", __entry->device_address,
		      __entry->size));

TRACE_EVENT(gxp_dmabuf_mapping_destroy_start,

	    TP_PROTO(dma_addr_t device_address, size_t size),

	    TP_ARGS(device_address, size),

	    TP_STRUCT__entry(__field(dma_addr_t, device_address) __field(size_t, size)),

	    TP_fast_assign(__entry->device_address = device_address; __entry->size = size;),

	    TP_printk("device_address = %#llX, size = %ld", __entry->device_address,
		      __entry->size));

TRACE_EVENT(gxp_dmabuf_mapping_destroy_end,

	    TP_PROTO(dma_addr_t device_address, size_t size),

	    TP_ARGS(device_address, size),

	    TP_STRUCT__entry(__field(dma_addr_t, device_address) __field(size_t, size)),

	    TP_fast_assign(__entry->device_address = device_address; __entry->size = size;),

	    TP_printk("device_address = %#llX, size = %ld", __entry->device_address,
		      __entry->size));

TRACE_EVENT(gxp_vd_block_ready_start,

	    TP_PROTO(int vdid),

	    TP_ARGS(vdid),

	    TP_STRUCT__entry(__field(int, vdid)),

	    TP_fast_assign(__entry->vdid = vdid;),

	    TP_printk("vdid = %d", __entry->vdid));

TRACE_EVENT(gxp_vd_block_ready_end,

	    TP_PROTO(int vdid),

	    TP_ARGS(vdid),

	    TP_STRUCT__entry(__field(int, vdid)),

	    TP_fast_assign(__entry->vdid = vdid;),

	    TP_printk("vdid = %d", __entry->vdid));

TRACE_EVENT(gxp_vd_block_unready_start,

	    TP_PROTO(int vdid),

	    TP_ARGS(vdid),

	    TP_STRUCT__entry(__field(int, vdid)),

	    TP_fast_assign(__entry->vdid = vdid;),

	    TP_printk("vdid = %d", __entry->vdid));

TRACE_EVENT(gxp_vd_block_unready_end,

	    TP_PROTO(int vdid),

	    TP_ARGS(vdid),

	    TP_STRUCT__entry(__field(int, vdid)),

	    TP_fast_assign(__entry->vdid = vdid;),

	    TP_printk("vdid = %d", __entry->vdid));

TRACE_EVENT(gxp_vd_allocate_start,

	    TP_PROTO(u16 requested_cores),

	    TP_ARGS(requested_cores),

	    TP_STRUCT__entry(__field(u16, requested_cores)),

	    TP_fast_assign(__entry->requested_cores = requested_cores;),

	    TP_printk("requested_cores = %d", __entry->requested_cores));

TRACE_EVENT(gxp_vd_allocate_end,

	    TP_PROTO(int vdid),

	    TP_ARGS(vdid),

	    TP_STRUCT__entry(__field(int, vdid)),

	    TP_fast_assign(__entry->vdid = vdid;),

	    TP_printk("vdid = %d", __entry->vdid));

TRACE_EVENT(gxp_vd_release_start,

	    TP_PROTO(int vdid),

	    TP_ARGS(vdid),

	    TP_STRUCT__entry(__field(int, vdid)),

	    TP_fast_assign(__entry->vdid = vdid;),

	    TP_printk("vdid = %d", __entry->vdid));

TRACE_EVENT(gxp_vd_release_end,

	    TP_PROTO(int vdid),

	    TP_ARGS(vdid),

	    TP_STRUCT__entry(__field(int, vdid)),

	    TP_fast_assign(__entry->vdid = vdid;),

	    TP_printk("vdid = %d", __entry->vdid));

TRACE_EVENT(gxp_uci_cmd_start,

	    TP_PROTO(u64 seq),

	    TP_ARGS(seq),

	    TP_STRUCT__entry(__field(u64, seq)),

	    TP_fast_assign(__entry->seq = seq;),

	    TP_printk("seq = %llu", __entry->seq));

TRACE_EVENT(gxp_uci_cmd_end,

	    TP_PROTO(u64 seq),

	    TP_ARGS(seq),

	    TP_STRUCT__entry(__field(u64, seq)),

	    TP_fast_assign(__entry->seq = seq;),

	    TP_printk("seq = %llu", __entry->seq));

/* Use a fake argument placeholder because the macro requires user to pass in some variables. */
TRACE_EVENT(gxp_uci_rsp_start,

	    TP_PROTO(int placeholder),

	    TP_ARGS(placeholder),

	    TP_STRUCT__entry(),

	    TP_fast_assign(),

	    TP_printk("%s", "irq triggered"));

TRACE_EVENT(gxp_uci_rsp_end,

	    TP_PROTO(u64 seq),

	    TP_ARGS(seq),

	    TP_STRUCT__entry(__field(u64, seq)),

	    TP_fast_assign(__entry->seq = seq;),

	    TP_printk("seq = %llu", __entry->seq));

TRACE_EVENT(gxp_uci_signal_outfence_before,

	    TP_PROTO(u64 seq),

	    TP_ARGS(seq),

	    TP_STRUCT__entry(__field(u64, seq)),

	    TP_fast_assign(__entry->seq = seq;),

	    TP_printk("seq = %llu", __entry->seq));

TRACE_EVENT(gxp_iif_unblocked_start,

	TP_PROTO(int id, int signal_error),

	TP_ARGS(id, signal_error),

	TP_STRUCT__entry(
		__field(int, id)
		__field(int, signal_error)
	),

	TP_fast_assign(
		__entry->id = id;
		__entry->signal_error = signal_error;
	),

	TP_printk("fence id = %d, signal_error = %d", __entry->id, __entry->signal_error)
);

TRACE_EVENT(gxp_iif_unblocked_end,

	TP_PROTO(u32 fence_id),

	TP_ARGS(fence_id),

	TP_STRUCT__entry(
		__field(u32, fence_id)
	),

	TP_fast_assign(
		__entry->fence_id = fence_id;
	),

	TP_printk("fence id = %u", __entry->fence_id)
);

#endif /* _TRACE_GXP_H */

/* This part must be outside protection */
#include <trace/define_trace.h>

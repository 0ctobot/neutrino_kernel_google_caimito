/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 *
 * (C) COPYRIGHT 2020-2023 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 */

#ifndef _KBASE_DEBUG_KTRACE_CSF_H_
#define _KBASE_DEBUG_KTRACE_CSF_H_

/*
 * KTrace target for internal ringbuffer
 */
#if KBASE_KTRACE_TARGET_RBUF
/**
 * kbasep_ktrace_add_csf - internal function to add trace about CSF
 * @kbdev:    kbase device
 * @code:     trace code
 * @group:    queue group, or NULL if no queue group
 * @queue:    queue, or NULL if no queue
 * @flags:    flags about the message
 * @info_val: generic information about @code to add to the trace
 *
 * PRIVATE: do not use directly. Use KBASE_KTRACE_ADD_CSF() instead.
 */

void kbasep_ktrace_add_csf(struct kbase_device *kbdev, enum kbase_ktrace_code code,
			   struct kbase_queue_group *group, struct kbase_queue *queue,
			   kbase_ktrace_flag_t flags, u64 info_val);

/**
 * kbasep_ktrace_add_csf_kcpu - internal function to add trace about the CSF
 *				KCPU queues.
 * @kbdev:      kbase device
 * @code:       trace code
 * @queue:      queue, or NULL if no queue
 * @info_val1:  Main infoval variable with information based on the KCPU
 *              ktrace call. Refer to mali_kbase_debug_ktrace_codes_csf.h
 *              for information on the infoval values.
 * @info_val2:  Extra infoval variable with information based on the KCPU
 *              ktrace call. Refer to mali_kbase_debug_ktrace_codes_csf.h
 *              for information on the infoval values.
 *
 * PRIVATE: do not use directly. Use KBASE_KTRACE_ADD_CSF_KCPU() instead.
 */
void kbasep_ktrace_add_csf_kcpu(struct kbase_device *kbdev, enum kbase_ktrace_code code,
				struct kbase_kcpu_command_queue *queue, u64 info_val1,
				u64 info_val2);

#define KBASE_KTRACE_RBUF_ADD_CSF(kbdev, code, group, queue, flags, info_val) \
	kbasep_ktrace_add_csf(kbdev, KBASE_KTRACE_CODE(code), group, queue, flags, info_val)

#define KBASE_KTRACE_RBUF_ADD_CSF_KCPU(kbdev, code, queue, info_val1, info_val2) \
	kbasep_ktrace_add_csf_kcpu(kbdev, KBASE_KTRACE_CODE(code), queue, info_val1, info_val2)

#else /* KBASE_KTRACE_TARGET_RBUF */

#define KBASE_KTRACE_RBUF_ADD_CSF(kbdev, code, group, queue, flags, info_val) \
	do {                                                                  \
		CSTD_UNUSED(kbdev);                                           \
		CSTD_NOP(code);                                               \
		CSTD_UNUSED(group);                                           \
		CSTD_UNUSED(queue);                                           \
		CSTD_UNUSED(flags);                                           \
		CSTD_UNUSED(info_val);                                        \
		CSTD_NOP(0);                                                  \
	} while (0)

#define KBASE_KTRACE_RBUF_ADD_CSF_KCPU(kbdev, code, queue, info_val1, info_val2) \
	do {                                                                     \
		CSTD_UNUSED(kbdev);                                              \
		CSTD_NOP(code);                                                  \
		CSTD_UNUSED(queue);                                              \
		CSTD_UNUSED(info_val1);                                          \
		CSTD_UNUSED(info_val2);                                          \
	} while (0)

#endif /* KBASE_KTRACE_TARGET_RBUF */

/*
 * KTrace target for Linux's ftrace
 *
 * Note: the header file(s) that define the trace_mali_<...> tracepoints are
 * included by the parent header file
 */
#if KBASE_KTRACE_TARGET_FTRACE

#define KBASE_KTRACE_FTRACE_ADD_CSF(kbdev, code, group, queue, info_val) \
	trace_mali_##code(kbdev, group, queue, info_val)

#define KBASE_KTRACE_FTRACE_ADD_KCPU(code, queue, info_val1, info_val2) \
	trace_mali_##code(queue, info_val1, info_val2)

#else /* KBASE_KTRACE_TARGET_FTRACE */

#define KBASE_KTRACE_FTRACE_ADD_CSF(kbdev, code, group, queue, info_val) \
	do {                                                             \
		CSTD_UNUSED(kbdev);                                      \
		CSTD_NOP(code);                                          \
		CSTD_UNUSED(group);                                      \
		CSTD_UNUSED(queue);                                      \
		CSTD_UNUSED(info_val);                                   \
		CSTD_NOP(0);                                             \
	} while (0)

#define KBASE_KTRACE_FTRACE_ADD_KCPU(code, queue, info_val1, info_val2) \
	do {                                                            \
		CSTD_NOP(code);                                         \
		CSTD_UNUSED(queue);                                     \
		CSTD_UNUSED(info_val1);                                 \
		CSTD_UNUSED(info_val2);                                 \
	} while (0)

#endif /* KBASE_KTRACE_TARGET_FTRACE */

/*
 * Master set of macros to route KTrace to any of the targets
 */

/* 1) Per‐code feature flags for the CSF “group” path (all off by default) */
#define ENABLE_KTRACE_SCHEDULER_EVICT_CTX_SLOTS_START 0
#define ENABLE_KTRACE_SCHEDULER_EVICT_CTX_SLOTS_END 0
#define ENABLE_KTRACE_CSF_FIRMWARE_BOOT 1
#define ENABLE_KTRACE_CSF_FIRMWARE_REBOOT 1
#define ENABLE_KTRACE_SCHEDULER_TOCK_INVOKE 0
#define ENABLE_KTRACE_SCHEDULER_TICK_INVOKE 0
#define ENABLE_KTRACE_SCHEDULER_TOCK_START 0
#define ENABLE_KTRACE_SCHEDULER_TOCK_END 0
#define ENABLE_KTRACE_SCHEDULER_TICK_START 0
#define ENABLE_KTRACE_SCHEDULER_TICK_END 0
#define ENABLE_KTRACE_SCHEDULER_RESET_START 0
#define ENABLE_KTRACE_SCHEDULER_RESET_END 0
#define ENABLE_KTRACE_SCHEDULER_PROTM_WAIT_QUIT_START 0
#define ENABLE_KTRACE_SCHEDULER_PROTM_WAIT_QUIT_END 0
#define ENABLE_KTRACE_SCHEDULER_GROUP_SYNC_UPDATE_EVENT 0
#define ENABLE_KTRACE_CSF_SYNC_UPDATE_NOTIFY_GPU_EVENT 0
#define ENABLE_KTRACE_CSF_INTERRUPT_START 0
#define ENABLE_KTRACE_CSF_INTERRUPT_END 0
#define ENABLE_KTRACE_CSG_INTERRUPT_PROCESS_START 0
#define ENABLE_KTRACE_CSF_INTERRUPT_GLB_REQ_ACK 0
#define ENABLE_KTRACE_SCHEDULER_GPU_IDLE_EVENT_CAN_SUSPEND 0
#define ENABLE_KTRACE_SCHEDULER_TICK_ADVANCE 0
#define ENABLE_KTRACE_SCHEDULER_TICK_NOADVANCE 0
#define ENABLE_KTRACE_SCHEDULER_RUNNABLE_KCTX_INSERT 0
#define ENABLE_KTRACE_SCHEDULER_RUNNABLE_KCTX_REMOVE 0
#define ENABLE_KTRACE_SCHEDULER_RUNNABLE_KCTX_ROTATE 0
#define ENABLE_KTRACE_SCHEDULER_RUNNABLE_KCTX_HEAD 0
#define ENABLE_KTRACE_SCHEDULER_GPU_IDLE_WORKER_START 0
#define ENABLE_KTRACE_SCHEDULER_GPU_IDLE_WORKER_END 0
#define ENABLE_KTRACE_SCHEDULER_GROUP_SYNC_UPDATE_WORKER_START 0
#define ENABLE_KTRACE_SCHEDULER_GROUP_SYNC_UPDATE_WORKER_END 0
#define ENABLE_KTRACE_SCHEDULER_UPDATE_IDLE_SLOTS_ACK 0
#define ENABLE_KTRACE_SCHEDULER_GPU_IDLE_WORKER_HANDLING_START 0
#define ENABLE_KTRACE_SCHEDULER_GPU_IDLE_WORKER_HANDLING_END 0
#define ENABLE_KTRACE_CSF_FIRMWARE_MCU_HALTED 1
#define ENABLE_KTRACE_CSF_FIRMWARE_MCU_SLEEP 1
#define ENABLE_KTRACE_KBASE_YIELD_NOW 0
#define ENABLE_KTRACE_KBASE_YIELD_IDLE 0
#define ENABLE_KTRACE_CSG_SLOT_START_REQ 0
#define ENABLE_KTRACE_CSG_SLOT_STOP_REQ 0
#define ENABLE_KTRACE_CSG_SLOT_RUNNING 0
#define ENABLE_KTRACE_CSG_SLOT_STOPPED 0
#define ENABLE_KTRACE_CSG_SLOT_CLEANED 0
#define ENABLE_KTRACE_CSG_UPDATE_IDLE_SLOT_REQ 0
#define ENABLE_KTRACE_CSG_SLOT_IDLE_SET 0
#define ENABLE_KTRACE_CSG_INTERRUPT_NO_NON_IDLE_GROUPS 0
#define ENABLE_KTRACE_CSG_INTERRUPT_NON_IDLE_GROUPS 0
#define ENABLE_KTRACE_CSG_SLOT_IDLE_CLEAR 0
#define ENABLE_KTRACE_CSG_SLOT_PRIO_UPDATE 0
#define ENABLE_KTRACE_CSG_INTERRUPT_SYNC_UPDATE 0
#define ENABLE_KTRACE_CSG_INTERRUPT_IDLE 0
#define ENABLE_KTRACE_CSG_INTERRUPT_PROGRESS_TIMER_EVENT 0
#define ENABLE_KTRACE_CSG_INTERRUPT_PROCESS_END 0
#define ENABLE_KTRACE_GROUP_SYNC_UPDATE_DONE 0
#define ENABLE_KTRACE_GROUP_DESCHEDULE 0
#define ENABLE_KTRACE_GROUP_SCHEDULE 0
#define ENABLE_KTRACE_GROUP_EVICT 0
#define ENABLE_KTRACE_GROUP_RUNNABLE_INSERT 0
#define ENABLE_KTRACE_GROUP_RUNNABLE_REMOVE 0
#define ENABLE_KTRACE_GROUP_RUNNABLE_ROTATE 0
#define ENABLE_KTRACE_GROUP_RUNNABLE_HEAD 0
#define ENABLE_KTRACE_GROUP_IDLE_WAIT_INSERT 0
#define ENABLE_KTRACE_GROUP_IDLE_WAIT_REMOVE 0
#define ENABLE_KTRACE_GROUP_IDLE_WAIT_HEAD 0
#define ENABLE_KTRACE_SCHEDULER_PROTM_ENTER_CHECK 0
#define ENABLE_KTRACE_SCHEDULER_PROTM_ENTER 0
#define ENABLE_KTRACE_SCHEDULER_PROTM_EXIT 0
#define ENABLE_KTRACE_SCHEDULER_TOP_GRP 0
#define ENABLE_KTRACE_SCHEDULER_NONIDLE_OFFSLOT_GRP_INC 0
#define ENABLE_KTRACE_SCHEDULER_NONIDLE_OFFSLOT_GRP_DEC 0
#define ENABLE_KTRACE_SCHEDULER_HANDLE_IDLE_SLOTS 0
#define ENABLE_KTRACE_PROTM_EVENT_WORKER_START 0
#define ENABLE_KTRACE_PROTM_EVENT_WORKER_END 0
#define ENABLE_KTRACE_SCHED_BUSY 0
#define ENABLE_KTRACE_SCHED_INACTIVE 0
#define ENABLE_KTRACE_SCHED_SUSPENDED 0
#define ENABLE_KTRACE_SCHED_SLEEPING 0
#define ENABLE_KTRACE_CSF_FIRMWARE_GLB_IDLE_TIMER_CHANGED 0
#define ENABLE_KTRACE_CSF_FIRMWARE_SLEEP_ON_IDLE_CHANGED 0
#define ENABLE_KTRACE_CSF_GROUP_INACTIVE 0
#define ENABLE_KTRACE_CSF_GROUP_RUNNABLE 0
#define ENABLE_KTRACE_CSF_GROUP_IDLE 0
#define ENABLE_KTRACE_CSF_GROUP_SUSPENDED 0
#define ENABLE_KTRACE_CSF_GROUP_SUSPENDED_ON_IDLE 0
#define ENABLE_KTRACE_CSF_GROUP_SUSPENDED_ON_WAIT_SYNC 0
#define ENABLE_KTRACE_CSF_GROUP_FAULT_EVICTED 1
#define ENABLE_KTRACE_CSF_GROUP_TERMINATED 1

// clang-format off
/* 2) Helper macro: expand bare name to enum then compare */
#define IS_KTRACE_CSF_GRP_ENABLED(code) \
	(KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_EVICT_CTX_SLOTS_START ? \
		       ENABLE_KTRACE_SCHEDULER_EVICT_CTX_SLOTS_START : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_EVICT_CTX_SLOTS_END ? \
		       ENABLE_KTRACE_SCHEDULER_EVICT_CTX_SLOTS_END : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_FIRMWARE_BOOT ? \
		       ENABLE_KTRACE_CSF_FIRMWARE_BOOT : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_FIRMWARE_REBOOT ? \
		       ENABLE_KTRACE_CSF_FIRMWARE_REBOOT : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_TOCK_INVOKE ? \
		       ENABLE_KTRACE_SCHEDULER_TOCK_INVOKE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_TICK_INVOKE ? \
		       ENABLE_KTRACE_SCHEDULER_TICK_INVOKE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_TOCK_START ? \
		       ENABLE_KTRACE_SCHEDULER_TOCK_START : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_TOCK_END ? \
		       ENABLE_KTRACE_SCHEDULER_TOCK_END : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_TICK_START ? \
		       ENABLE_KTRACE_SCHEDULER_TICK_START : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_TICK_END ? \
		       ENABLE_KTRACE_SCHEDULER_TICK_END : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_RESET_START ? \
		       ENABLE_KTRACE_SCHEDULER_RESET_START : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_RESET_END ? \
		       ENABLE_KTRACE_SCHEDULER_RESET_END : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_PROTM_WAIT_QUIT_START ? \
		       ENABLE_KTRACE_SCHEDULER_PROTM_WAIT_QUIT_START : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_PROTM_WAIT_QUIT_END ? \
		       ENABLE_KTRACE_SCHEDULER_PROTM_WAIT_QUIT_END : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_GROUP_SYNC_UPDATE_EVENT ? \
		       ENABLE_KTRACE_SCHEDULER_GROUP_SYNC_UPDATE_EVENT : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_SYNC_UPDATE_NOTIFY_GPU_EVENT ? \
		       ENABLE_KTRACE_CSF_SYNC_UPDATE_NOTIFY_GPU_EVENT : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_INTERRUPT_START ? \
		       ENABLE_KTRACE_CSF_INTERRUPT_START : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_INTERRUPT_END ? \
		       ENABLE_KTRACE_CSF_INTERRUPT_END : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_INTERRUPT_PROCESS_START ? \
		       ENABLE_KTRACE_CSG_INTERRUPT_PROCESS_START : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_INTERRUPT_GLB_REQ_ACK ? \
		       ENABLE_KTRACE_CSF_INTERRUPT_GLB_REQ_ACK : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_GPU_IDLE_EVENT_CAN_SUSPEND ? \
		       ENABLE_KTRACE_SCHEDULER_GPU_IDLE_EVENT_CAN_SUSPEND : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_TICK_ADVANCE ? \
		       ENABLE_KTRACE_SCHEDULER_TICK_ADVANCE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_TICK_NOADVANCE ? \
		       ENABLE_KTRACE_SCHEDULER_TICK_NOADVANCE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_RUNNABLE_KCTX_INSERT ? \
		       ENABLE_KTRACE_SCHEDULER_RUNNABLE_KCTX_INSERT : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_RUNNABLE_KCTX_REMOVE ? \
		       ENABLE_KTRACE_SCHEDULER_RUNNABLE_KCTX_REMOVE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_RUNNABLE_KCTX_ROTATE ? \
		       ENABLE_KTRACE_SCHEDULER_RUNNABLE_KCTX_ROTATE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_RUNNABLE_KCTX_HEAD ? \
		       ENABLE_KTRACE_SCHEDULER_RUNNABLE_KCTX_HEAD : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_GPU_IDLE_WORKER_START ? \
		       ENABLE_KTRACE_SCHEDULER_GPU_IDLE_WORKER_START : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_GPU_IDLE_WORKER_END ? \
		       ENABLE_KTRACE_SCHEDULER_GPU_IDLE_WORKER_END : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_GROUP_SYNC_UPDATE_WORKER_START ? \
		       ENABLE_KTRACE_SCHEDULER_GROUP_SYNC_UPDATE_WORKER_START : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_GROUP_SYNC_UPDATE_WORKER_END ? \
		       ENABLE_KTRACE_SCHEDULER_GROUP_SYNC_UPDATE_WORKER_END : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_UPDATE_IDLE_SLOTS_ACK ? \
		       ENABLE_KTRACE_SCHEDULER_UPDATE_IDLE_SLOTS_ACK : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_GPU_IDLE_WORKER_HANDLING_START ? \
		       ENABLE_KTRACE_SCHEDULER_GPU_IDLE_WORKER_HANDLING_START : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_GPU_IDLE_WORKER_HANDLING_END ? \
		       ENABLE_KTRACE_SCHEDULER_GPU_IDLE_WORKER_HANDLING_END : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_FIRMWARE_MCU_HALTED ? \
		       ENABLE_KTRACE_CSF_FIRMWARE_MCU_HALTED : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_FIRMWARE_MCU_SLEEP ? \
		       ENABLE_KTRACE_CSF_FIRMWARE_MCU_SLEEP : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_KBASE_YIELD_NOW ? \
		       ENABLE_KTRACE_KBASE_YIELD_NOW : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_KBASE_YIELD_IDLE ? \
		       ENABLE_KTRACE_KBASE_YIELD_IDLE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_SLOT_START_REQ ? \
		       ENABLE_KTRACE_CSG_SLOT_START_REQ : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_SLOT_STOP_REQ ? \
		       ENABLE_KTRACE_CSG_SLOT_STOP_REQ : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_SLOT_RUNNING ? \
		       ENABLE_KTRACE_CSG_SLOT_RUNNING : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_SLOT_STOPPED ? \
		       ENABLE_KTRACE_CSG_SLOT_STOPPED : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_SLOT_CLEANED ? \
		       ENABLE_KTRACE_CSG_SLOT_CLEANED : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_UPDATE_IDLE_SLOT_REQ ? \
		       ENABLE_KTRACE_CSG_UPDATE_IDLE_SLOT_REQ : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_SLOT_IDLE_SET ? \
		       ENABLE_KTRACE_CSG_SLOT_IDLE_SET : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_INTERRUPT_NO_NON_IDLE_GROUPS ? \
		       ENABLE_KTRACE_CSG_INTERRUPT_NO_NON_IDLE_GROUPS : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_INTERRUPT_NON_IDLE_GROUPS ? \
		       ENABLE_KTRACE_CSG_INTERRUPT_NON_IDLE_GROUPS : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_SLOT_IDLE_CLEAR ? \
		       ENABLE_KTRACE_CSG_SLOT_IDLE_CLEAR : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_SLOT_PRIO_UPDATE ? \
		       ENABLE_KTRACE_CSG_SLOT_PRIO_UPDATE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_INTERRUPT_SYNC_UPDATE ? \
		       ENABLE_KTRACE_CSG_INTERRUPT_SYNC_UPDATE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_INTERRUPT_IDLE ? \
		       ENABLE_KTRACE_CSG_INTERRUPT_IDLE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_INTERRUPT_PROGRESS_TIMER_EVENT ? \
		       ENABLE_KTRACE_CSG_INTERRUPT_PROGRESS_TIMER_EVENT : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSG_INTERRUPT_PROCESS_END ? \
		       ENABLE_KTRACE_CSG_INTERRUPT_PROCESS_END : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_GROUP_SYNC_UPDATE_DONE ? \
		       ENABLE_KTRACE_GROUP_SYNC_UPDATE_DONE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_GROUP_DESCHEDULE ? \
		       ENABLE_KTRACE_GROUP_DESCHEDULE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_GROUP_SCHEDULE ? \
		       ENABLE_KTRACE_GROUP_SCHEDULE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_GROUP_EVICT ? \
		       ENABLE_KTRACE_GROUP_EVICT : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_GROUP_RUNNABLE_INSERT ? \
		       ENABLE_KTRACE_GROUP_RUNNABLE_INSERT : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_GROUP_RUNNABLE_REMOVE ? \
		       ENABLE_KTRACE_GROUP_RUNNABLE_REMOVE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_GROUP_RUNNABLE_ROTATE ? \
		       ENABLE_KTRACE_GROUP_RUNNABLE_ROTATE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_GROUP_RUNNABLE_HEAD ? \
		       ENABLE_KTRACE_GROUP_RUNNABLE_HEAD : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_GROUP_IDLE_WAIT_INSERT ? \
		       ENABLE_KTRACE_GROUP_IDLE_WAIT_INSERT : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_GROUP_IDLE_WAIT_REMOVE ? \
		       ENABLE_KTRACE_GROUP_IDLE_WAIT_REMOVE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_GROUP_IDLE_WAIT_HEAD ? \
		       ENABLE_KTRACE_GROUP_IDLE_WAIT_HEAD : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_PROTM_ENTER_CHECK ? \
		       ENABLE_KTRACE_SCHEDULER_PROTM_ENTER_CHECK : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_PROTM_ENTER ? \
		       ENABLE_KTRACE_SCHEDULER_PROTM_ENTER : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_PROTM_EXIT ? \
		       ENABLE_KTRACE_SCHEDULER_PROTM_EXIT : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_TOP_GRP ? \
		       ENABLE_KTRACE_SCHEDULER_TOP_GRP : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_NONIDLE_OFFSLOT_GRP_INC ? \
		       ENABLE_KTRACE_SCHEDULER_NONIDLE_OFFSLOT_GRP_INC : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_NONIDLE_OFFSLOT_GRP_DEC ? \
		       ENABLE_KTRACE_SCHEDULER_NONIDLE_OFFSLOT_GRP_DEC : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHEDULER_HANDLE_IDLE_SLOTS ? \
		       ENABLE_KTRACE_SCHEDULER_HANDLE_IDLE_SLOTS : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PROTM_EVENT_WORKER_START ? \
		       ENABLE_KTRACE_PROTM_EVENT_WORKER_START : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PROTM_EVENT_WORKER_END ? \
		       ENABLE_KTRACE_PROTM_EVENT_WORKER_END : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHED_BUSY ? \
		       ENABLE_KTRACE_SCHED_BUSY : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHED_INACTIVE ? \
		       ENABLE_KTRACE_SCHED_INACTIVE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHED_SUSPENDED ? \
		       ENABLE_KTRACE_SCHED_SUSPENDED : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHED_SLEEPING ? \
		       ENABLE_KTRACE_SCHED_SLEEPING : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_FIRMWARE_GLB_IDLE_TIMER_CHANGED ? \
		       ENABLE_KTRACE_CSF_FIRMWARE_GLB_IDLE_TIMER_CHANGED : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_FIRMWARE_SLEEP_ON_IDLE_CHANGED ? \
		       ENABLE_KTRACE_CSF_FIRMWARE_SLEEP_ON_IDLE_CHANGED : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_GROUP_INACTIVE ? \
		       ENABLE_KTRACE_CSF_GROUP_INACTIVE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_GROUP_RUNNABLE ? \
		       ENABLE_KTRACE_CSF_GROUP_RUNNABLE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_GROUP_IDLE ? \
		       ENABLE_KTRACE_CSF_GROUP_IDLE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_GROUP_SUSPENDED ? \
		       ENABLE_KTRACE_CSF_GROUP_SUSPENDED : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_GROUP_SUSPENDED_ON_IDLE ? \
		       ENABLE_KTRACE_CSF_GROUP_SUSPENDED_ON_IDLE : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_GROUP_SUSPENDED_ON_WAIT_SYNC ? \
		       ENABLE_KTRACE_CSF_GROUP_SUSPENDED_ON_WAIT_SYNC : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_GROUP_FAULT_EVICTED ? \
		       ENABLE_KTRACE_CSF_GROUP_FAULT_EVICTED : \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSF_GROUP_TERMINATED ? \
		       ENABLE_KTRACE_CSF_GROUP_TERMINATED : \
		       0)
// clang-format on
/**
 * KBASE_KTRACE_ADD_CSF_GRP - Add trace values about a group, with info
 * @kbdev:    kbase device
 * @code:     trace code
 * @group:    queue group, or NULL if no queue group
 * @info_val: generic information about @code to add to the trace
 *
 * Note: Any functions called through this macro will still be evaluated in
 * Release builds (CONFIG_MALI_DEBUG not defined). Therefore, when
 * KBASE_KTRACE_ENABLE == 0 any functions called to get the parameters supplied
 * to this macro must:
 * a) be static or static inline, and
 * b) just return 0 and have no other statements present in the body.
 */
#undef KBASE_KTRACE_ADD_CSF_GRP
#define KBASE_KTRACE_ADD_CSF_GRP(kbdev, code, group, info_val)                                 \
	do {                                                                                   \
		/* capture values that could come from non-pure fn calls */                    \
		struct kbase_queue_group *__group = group;                                     \
		u64 __info_val = info_val;                                                     \
		if (IS_KTRACE_CSF_GRP_ENABLED(code)) {                                         \
			KBASE_KTRACE_RBUF_ADD_CSF(kbdev, code, __group, NULL, 0u, __info_val); \
			KBASE_KTRACE_FTRACE_ADD_CSF(kbdev, code, __group, NULL, __info_val);   \
		}                                                                              \
	} while (0)

/*
 * 1) Per-code feature flags (all off by default)
 *    These map 1:1 to the CSI_* and QUEUE_* names.
 */
#define ENABLE_KTRACE_CSI_START 0
#define ENABLE_KTRACE_CSI_STOP 0
#define ENABLE_KTRACE_CSI_STOP_REQ 0
#define ENABLE_KTRACE_CSI_INTERRUPT_FAULT 1
#define ENABLE_KTRACE_CSI_INTERRUPT_TILER_OOM 1
#define ENABLE_KTRACE_CSI_INTERRUPT_PROTM_PEND 0
#define ENABLE_KTRACE_CSI_PROTM_ACK 0
#define ENABLE_KTRACE_CSI_PROTM_PEND_SET 0
#define ENABLE_KTRACE_CSI_PROTM_PEND_CLEAR 0

#define ENABLE_KTRACE_QUEUE_START 0
#define ENABLE_KTRACE_QUEUE_STOP 0
#define ENABLE_KTRACE_QUEUE_SYNC_UPDATE_EVAL_START 0
#define ENABLE_KTRACE_QUEUE_SYNC_UPDATE_EVAL_END 0
#define ENABLE_KTRACE_QUEUE_SYNC_UPDATE_WAIT_STATUS 0
#define ENABLE_KTRACE_QUEUE_SYNC_UPDATE_CUR_VAL 0
#define ENABLE_KTRACE_QUEUE_SYNC_UPDATE_TEST_VAL 0
#define ENABLE_KTRACE_QUEUE_SYNC_UPDATE_BLOCKED_REASON 0

/*
 * 2) Helper macro: expand the bare name into its real enum, then compare.
 *
 *    KBASE_KTRACE_CODE(code) expands to KBASE_KTRACE_CODE_<code>,
 *    which is the actual enum member.
 */
#define IS_KTRACE_CSF_GRP_Q_ENABLED(code)                                                \
	(KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSI_START ?                        \
		       ENABLE_KTRACE_CSI_START :                                               \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSI_STOP ?                         \
		       ENABLE_KTRACE_CSI_STOP :                                                \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSI_STOP_REQ ?                     \
		       ENABLE_KTRACE_CSI_STOP_REQ :                                            \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSI_INTERRUPT_FAULT ?              \
		       ENABLE_KTRACE_CSI_INTERRUPT_FAULT :                                     \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSI_INTERRUPT_TILER_OOM ?          \
		       ENABLE_KTRACE_CSI_INTERRUPT_TILER_OOM :                                 \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSI_INTERRUPT_PROTM_PEND ?         \
		       ENABLE_KTRACE_CSI_INTERRUPT_PROTM_PEND :                                \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSI_PROTM_ACK ?                    \
		       ENABLE_KTRACE_CSI_PROTM_ACK :                                           \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSI_PROTM_PEND_SET ?               \
		       ENABLE_KTRACE_CSI_PROTM_PEND_SET :                                      \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CSI_PROTM_PEND_CLEAR ?             \
		       ENABLE_KTRACE_CSI_PROTM_PEND_CLEAR :                                    \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_QUEUE_START ?                      \
		       ENABLE_KTRACE_QUEUE_START :                                             \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_QUEUE_STOP ?                       \
		       ENABLE_KTRACE_QUEUE_STOP :                                              \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_QUEUE_SYNC_UPDATE_EVAL_START ?     \
		       ENABLE_KTRACE_QUEUE_SYNC_UPDATE_EVAL_START :                            \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_QUEUE_SYNC_UPDATE_EVAL_END ?       \
		       ENABLE_KTRACE_QUEUE_SYNC_UPDATE_EVAL_END :                              \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_QUEUE_SYNC_UPDATE_WAIT_STATUS ?    \
		       ENABLE_KTRACE_QUEUE_SYNC_UPDATE_WAIT_STATUS :                           \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_QUEUE_SYNC_UPDATE_CUR_VAL ?        \
		       ENABLE_KTRACE_QUEUE_SYNC_UPDATE_CUR_VAL :                               \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_QUEUE_SYNC_UPDATE_TEST_VAL ?       \
		       ENABLE_KTRACE_QUEUE_SYNC_UPDATE_TEST_VAL :                              \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_QUEUE_SYNC_UPDATE_BLOCKED_REASON ? \
		       ENABLE_KTRACE_QUEUE_SYNC_UPDATE_BLOCKED_REASON :                        \
		       0)

/*
 * 3) Undefine and re-define the “Q” variant so that *every* call
 *    to KBASE_KTRACE_ADD_CSF_GRP_Q first tests our helper.
 */

/**
 * KBASE_KTRACE_ADD_CSF_GRP_Q - Add trace values about a group, queue, with info
 * @kbdev:    kbase device
 * @code:     trace code
 * @group:    queue group, or NULL if no queue group
 * @queue:    queue, or NULL if no queue
 * @info_val: generic information about @code to add to the trace
 *
 * Note: Any functions called through this macro will still be evaluated in
 * Release builds (CONFIG_MALI_DEBUG not defined). Therefore, when
 * KBASE_KTRACE_ENABLE == 0 any functions called to get the parameters supplied
 * to this macro must:
 * a) be static or static inline, and
 * b) just return 0 and have no other statements present in the body.
 */
#undef KBASE_KTRACE_ADD_CSF_GRP_Q
#define KBASE_KTRACE_ADD_CSF_GRP_Q(kbdev, code, group, queue, info_val)                       \
	do {                                                                                  \
		struct kbase_queue_group *__group = (group);                                  \
		struct kbase_queue *__queue = (queue);                                        \
		u64 __info = (info_val);                                                      \
		if (IS_KTRACE_CSF_GRP_Q_ENABLED(code)) {                                      \
			KBASE_KTRACE_RBUF_ADD_CSF(kbdev, code, __group, __queue, 0u, __info); \
			KBASE_KTRACE_FTRACE_ADD_CSF(kbdev, code, __group, __queue, __info);   \
		}                                                                             \
	} while (0)

/*
 * KCPU command-queue codes feature flags (all off by default)
 * These map 1:1 to the KCPU_* trace‐code names.
 */
#define ENABLE_KTRACE_KCPU_QUEUE_CREATE 0
#define ENABLE_KTRACE_KCPU_QUEUE_DELETE 0
#define ENABLE_KTRACE_KCPU_CQS_SET 0
#define ENABLE_KTRACE_KCPU_CQS_WAIT_START 0
#define ENABLE_KTRACE_KCPU_CQS_WAIT_END 0
#define ENABLE_KTRACE_KCPU_FENCE_SIGNAL 1
#define ENABLE_KTRACE_KCPU_FENCE_WAIT_START 0
#define ENABLE_KTRACE_KCPU_FENCE_WAIT_END 0

#define IS_KTRACE_CSF_KCPU_ENABLED(code)                                      \
	(KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_KCPU_QUEUE_CREATE ?     \
		       ENABLE_KTRACE_KCPU_QUEUE_CREATE :                            \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_KCPU_QUEUE_DELETE ?     \
		       ENABLE_KTRACE_KCPU_QUEUE_DELETE :                            \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_KCPU_CQS_SET ?          \
		       ENABLE_KTRACE_KCPU_CQS_SET :                                 \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_KCPU_CQS_WAIT_START ?   \
		       ENABLE_KTRACE_KCPU_CQS_WAIT_START :                          \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_KCPU_CQS_WAIT_END ?     \
		       ENABLE_KTRACE_KCPU_CQS_WAIT_END :                            \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_KCPU_FENCE_SIGNAL ?     \
		       ENABLE_KTRACE_KCPU_FENCE_SIGNAL :                            \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_KCPU_FENCE_WAIT_START ? \
		       ENABLE_KTRACE_KCPU_FENCE_WAIT_START :                        \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_KCPU_FENCE_WAIT_END ?   \
		       ENABLE_KTRACE_KCPU_FENCE_WAIT_END :                          \
		       0)

#undef KBASE_KTRACE_ADD_CSF_KCPU
#define KBASE_KTRACE_ADD_CSF_KCPU(kbdev, code, queue, info_val1, info_val2)                    \
	do {                                                                                   \
		struct kbase_kcpu_command_queue *__queue = queue;                              \
		u64 __info_val1 = info_val1;                                                   \
		u64 __info_val2 = info_val2;                                                   \
		if (IS_KTRACE_CSF_KCPU_ENABLED(code)) {                                        \
			KBASE_KTRACE_RBUF_ADD_CSF_KCPU(kbdev, code, __queue, __info_val1,      \
						       __info_val2);                           \
			KBASE_KTRACE_FTRACE_ADD_KCPU(code, __queue, __info_val1, __info_val2); \
		}                                                                              \
	} while (0)

#endif /* _KBASE_DEBUG_KTRACE_CSF_H_ */

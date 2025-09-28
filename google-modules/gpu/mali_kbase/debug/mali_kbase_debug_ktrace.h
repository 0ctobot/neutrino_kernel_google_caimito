/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 *
 * (C) COPYRIGHT 2020-2024 ARM Limited. All rights reserved.
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

/*
 * DOC: Kbase's own trace, 'KTrace'
 *
 * Low overhead trace specific to kbase, aimed at:
 * - common use-cases for tracing kbase specific functionality to do with
 *   running work on the GPU
 * - easy 1-line addition of new types of trace
 *
 * KTrace can be recorded in one or more of the following targets:
 * - KBASE_KTRACE_TARGET_RBUF: low overhead ringbuffer protected by an
 *   irq-spinlock, output available via dev_dbg() and debugfs file
 * - KBASE_KTRACE_TARGET_FTRACE: ftrace based tracepoints under 'mali' events
 */

#ifndef _KBASE_DEBUG_KTRACE_H_
#define _KBASE_DEBUG_KTRACE_H_

#if KBASE_KTRACE_TARGET_FTRACE
#include "mali_linux_trace.h"
#endif

#include "debug/backend/mali_kbase_debug_ktrace_csf.h"

/**
 * kbase_ktrace_init - initialize kbase ktrace.
 * @kbdev: kbase device
 * Return: 0 if successful or a negative error code on failure.
 */
int kbase_ktrace_init(struct kbase_device *kbdev);

/**
 * kbase_ktrace_term - terminate kbase ktrace.
 * @kbdev: kbase device
 */
void kbase_ktrace_term(struct kbase_device *kbdev);

/**
 * kbase_ktrace_hook_wrapper - wrapper so that dumping ktrace can be done via a
 *                             callback.
 * @param: kbase device, cast to void pointer
 */
void kbase_ktrace_hook_wrapper(void *param);

#if IS_ENABLED(CONFIG_DEBUG_FS)
/**
 * kbase_ktrace_debugfs_init - initialize kbase ktrace for debugfs usage, if
 *                             the selected targets support it.
 * @kbdev: kbase device
 *
 * There is no matching 'term' call, debugfs_remove_recursive() is sufficient.
 */
void kbase_ktrace_debugfs_init(struct kbase_device *kbdev);
#endif /* CONFIG_DEBUG_FS */

/*
 * KTrace target for internal ringbuffer
 */
#if KBASE_KTRACE_TARGET_RBUF
/**
 * kbasep_ktrace_initialized - Check whether kbase ktrace is initialized
 *
 * @ktrace: ktrace of kbase device.
 *
 * Return: true if ktrace has been initialized.
 */
static inline bool kbasep_ktrace_initialized(struct kbase_ktrace *ktrace)
{
	return ktrace->rbuf != NULL;
}

/**
 * kbasep_ktrace_add - internal function to add trace to the ringbuffer.
 * @kbdev:    kbase device
 * @code:     ktrace code
 * @kctx:     kbase context, or NULL if no context
 * @flags:    flags about the message
 * @info_val: generic information about @code to add to the trace
 *
 * PRIVATE: do not use directly. Use KBASE_KTRACE_ADD() instead.
 */
void kbasep_ktrace_add(struct kbase_device *kbdev, enum kbase_ktrace_code code,
		       struct kbase_context *kctx, kbase_ktrace_flag_t flags, u64 info_val);

/**
 * kbasep_ktrace_clear - clear the trace ringbuffer
 * @kbdev: kbase device
 *
 * PRIVATE: do not use directly. Use KBASE_KTRACE_CLEAR() instead.
 */
void kbasep_ktrace_clear(struct kbase_device *kbdev);

/**
 * kbasep_ktrace_dump - dump ktrace ringbuffer to dev_dbg(), then clear it
 * @kbdev: kbase device
 *
 * PRIVATE: do not use directly. Use KBASE_KTRACE_DUMP() instead.
 */
void kbasep_ktrace_dump(struct kbase_device *kbdev);

/**
 * kbasep_ktrace_copy - copy ktrace buffer.
 * Elements in the buffer will be ordered from earliest to latest.
 * Precondition: ktrace lock must be held.
 *
 * @kbdev: kbase device
 * @msgs: a region of memory of size data_size that the ktrace buffer will be copied to
 * @num_msgs: the size of data. 
 * Return: The number of elements copied.
 */
 u32 kbasep_ktrace_copy(struct kbase_device* kbdev, struct kbase_ktrace_msg* msgs, u32 num_msgs);

#define KBASE_KTRACE_RBUF_ADD(kbdev, code, kctx, info_val) \
	kbasep_ktrace_add(kbdev, KBASE_KTRACE_CODE(code), kctx, 0, info_val)

#define KBASE_KTRACE_RBUF_CLEAR(kbdev) kbasep_ktrace_clear(kbdev)

#define KBASE_KTRACE_RBUF_DUMP(kbdev) kbasep_ktrace_dump(kbdev)

#else /* KBASE_KTRACE_TARGET_RBUF */

#define KBASE_KTRACE_RBUF_ADD(kbdev, code, kctx, info_val) \
	do {                                               \
		CSTD_UNUSED(kbdev);                        \
		CSTD_NOP(code);                            \
		CSTD_UNUSED(kctx);                         \
		CSTD_UNUSED(info_val);                     \
		CSTD_NOP(0);                               \
	} while (0)

#define KBASE_KTRACE_RBUF_CLEAR(kbdev) \
	do {                           \
		CSTD_UNUSED(kbdev);    \
		CSTD_NOP(0);           \
	} while (0)
#define KBASE_KTRACE_RBUF_DUMP(kbdev) \
	do {                          \
		CSTD_UNUSED(kbdev);   \
		CSTD_NOP(0);          \
	} while (0)
#endif /* KBASE_KTRACE_TARGET_RBUF */

/*
 * KTrace target for Linux's ftrace
 */
#if KBASE_KTRACE_TARGET_FTRACE

#define KBASE_KTRACE_FTRACE_ADD(kbdev, code, kctx, info_val) trace_mali_##code(kctx, info_val)

#else /* KBASE_KTRACE_TARGET_FTRACE */
#define KBASE_KTRACE_FTRACE_ADD(kbdev, code, kctx, info_val) \
	do {                                                 \
		CSTD_UNUSED(kbdev);                          \
		CSTD_NOP(code);                              \
		CSTD_UNUSED(kctx);                           \
		CSTD_UNUSED(info_val);                       \
		CSTD_NOP(0);                                 \
	} while (0)
#endif /* KBASE_KTRACE_TARGET_FTRACE */

/* No 'clear' implementation for ftrace yet */
#define KBASE_KTRACE_FTRACE_CLEAR(kbdev) \
	do {                             \
		CSTD_UNUSED(kbdev);      \
		CSTD_NOP(0);             \
	} while (0)

/* No 'dump' implementation for ftrace yet */
#define KBASE_KTRACE_FTRACE_DUMP(kbdev) \
	do {                            \
		CSTD_UNUSED(kbdev);     \
		CSTD_NOP(0);            \
	} while (0)

/*
 * Master set of macros to route KTrace to any of the targets
 */

#define ENABLE_KTRACE_CORE_CTX_DESTROY 0
#define ENABLE_KTRACE_CORE_CTX_HWINSTR_TERM 0
#define ENABLE_KTRACE_CORE_GPU_IRQ 0
#define ENABLE_KTRACE_CORE_PWR_IRQ 0
#define ENABLE_KTRACE_CORE_GPU_IRQ_CLEAR 0
#define ENABLE_KTRACE_CORE_GPU_IRQ_DONE 0
#define ENABLE_KTRACE_CORE_GPU_SOFT_RESET 1
#define ENABLE_KTRACE_CORE_GPU_HARD_RESET 1
#define ENABLE_KTRACE_CORE_GPU_PRFCNT_CLEAR 0
#define ENABLE_KTRACE_CORE_GPU_PRFCNT_SAMPLE 0
#define ENABLE_KTRACE_CORE_GPU_CLEAN_INV_CACHES 0

#define ENABLE_KTRACE_PM_JOB_SUBMIT_AFTER_POWERING_UP 0
#define ENABLE_KTRACE_PM_JOB_SUBMIT_AFTER_POWERED_UP 0
#define ENABLE_KTRACE_PM_PWRON 0
#define ENABLE_KTRACE_PM_PWRON_TILER 1
#define ENABLE_KTRACE_PM_PWRON_L2 0
#define ENABLE_KTRACE_PM_PWROFF 0
#define ENABLE_KTRACE_PM_PWROFF_TILER 1
#define ENABLE_KTRACE_PM_PWROFF_L2 0
#define ENABLE_KTRACE_PM_CORES_POWERED 0
#define ENABLE_KTRACE_PM_CORES_POWERED_TILER 0
#define ENABLE_KTRACE_PM_CORES_POWERED_L2 0
#define ENABLE_KTRACE_PM_PWRON_NEURAL 0
#define ENABLE_KTRACE_PM_PWROFF_NEURAL 0
#define ENABLE_KTRACE_PM_CORES_POWERED_NEURAL 0
#define ENABLE_KTRACE_PM_CORES_CHANGE_DESIRED 0
#define ENABLE_KTRACE_PM_CORES_CHANGE_DESIRED_TILER 0
#define ENABLE_KTRACE_PM_CORES_CHANGE_AVAILABLE 0
#define ENABLE_KTRACE_PM_CORES_CHANGE_AVAILABLE_TILER 0
#define ENABLE_KTRACE_PM_CORES_CHANGE_AVAILABLE_L2 0
#define ENABLE_KTRACE_PM_CORES_AVAILABLE 0
#define ENABLE_KTRACE_PM_CORES_AVAILABLE_TILER 0
#define ENABLE_KTRACE_PM_DESIRED_REACHED 0
#define ENABLE_KTRACE_PM_DESIRED_REACHED_TILER 0
#define ENABLE_KTRACE_PM_RELEASE_CHANGE_SHADER_NEEDED 0
#define ENABLE_KTRACE_PM_RELEASE_CHANGE_TILER_NEEDED 0
#define ENABLE_KTRACE_PM_REQUEST_CHANGE_SHADER_NEEDED 0
#define ENABLE_KTRACE_PM_REQUEST_CHANGE_TILER_NEEDED 0
#define ENABLE_KTRACE_PM_WAKE_WAITERS 0
#define ENABLE_KTRACE_PM_CONTEXT_ACTIVE 0
#define ENABLE_KTRACE_PM_CONTEXT_IDLE 0
#define ENABLE_KTRACE_PM_GPU_ON 0
#define ENABLE_KTRACE_PM_GPU_OFF 0
#define ENABLE_KTRACE_PM_SET_POLICY 0
#define ENABLE_KTRACE_PM_CA_SET_POLICY 0
#define ENABLE_KTRACE_PM_CURRENT_POLICY_INIT 0
#define ENABLE_KTRACE_PM_CURRENT_POLICY_TERM 0
#define ENABLE_KTRACE_PM_POWEROFF_WAIT_WQ 0
#define ENABLE_KTRACE_PM_RUNTIME_SUSPEND_CALLBACK 0
#define ENABLE_KTRACE_PM_RUNTIME_RESUME_CALLBACK 0
/* plus ENABLE_KTRACE_PM_L2_<STATE> for each PM_L2_… state from mali_kbase_pm_l2_states.h */

#define ENABLE_KTRACE_SCHED_RETAIN_CTX_NOLOCK 0
#define ENABLE_KTRACE_SCHED_RELEASE_CTX 0

#define ENABLE_KTRACE_ARB_VM_STATE 0
#define ENABLE_KTRACE_ARB_VM_EVT 0
#define ENABLE_KTRACE_ARB_GPU_GRANTED 0
#define ENABLE_KTRACE_ARB_GPU_LOST 0
#define ENABLE_KTRACE_ARB_GPU_STARTED 0
#define ENABLE_KTRACE_ARB_GPU_STOP_REQUESTED 0
#define ENABLE_KTRACE_ARB_GPU_STOPPED 0
#define ENABLE_KTRACE_ARB_GPU_REQUESTED 0
/* New Tiler chunk alloc and free ktrace points added*/
#define ENABLE_KTRACE_TILER_CHUNK_ALLOC 1
#define ENABLE_KTRACE_TILER_CHUNK_ALLOC_SIZE 1
#define ENABLE_KTRACE_TILER_CHUNK_FREE 1
#define ENABLE_KTRACE_TILER_CHUNK_FREE_SIZE 1

#define IS_KTRACE_ADD_ENABLED(code)                                                     \
	(KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CORE_CTX_DESTROY ?                \
		       ENABLE_KTRACE_CORE_CTX_DESTROY :                                       \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CORE_CTX_HWINSTR_TERM ?           \
		       ENABLE_KTRACE_CORE_CTX_HWINSTR_TERM :                                  \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CORE_GPU_IRQ ?                    \
		       ENABLE_KTRACE_CORE_GPU_IRQ :                                           \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CORE_PWR_IRQ ?                    \
		       ENABLE_KTRACE_CORE_PWR_IRQ :                                           \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CORE_GPU_IRQ_CLEAR ?              \
		       ENABLE_KTRACE_CORE_GPU_IRQ_CLEAR :                                     \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CORE_GPU_IRQ_DONE ?               \
		       ENABLE_KTRACE_CORE_GPU_IRQ_DONE :                                      \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CORE_GPU_SOFT_RESET ?             \
		       ENABLE_KTRACE_CORE_GPU_SOFT_RESET :                                    \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CORE_GPU_HARD_RESET ?             \
		       ENABLE_KTRACE_CORE_GPU_HARD_RESET :                                    \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CORE_GPU_PRFCNT_CLEAR ?           \
		       ENABLE_KTRACE_CORE_GPU_PRFCNT_CLEAR :                                  \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CORE_GPU_PRFCNT_SAMPLE ?          \
		       ENABLE_KTRACE_CORE_GPU_PRFCNT_SAMPLE :                                 \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_CORE_GPU_CLEAN_INV_CACHES ?       \
		       ENABLE_KTRACE_CORE_GPU_CLEAN_INV_CACHES :                              \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_JOB_SUBMIT_AFTER_POWERING_UP ? \
		       ENABLE_KTRACE_PM_JOB_SUBMIT_AFTER_POWERING_UP :                        \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_JOB_SUBMIT_AFTER_POWERED_UP ?  \
		       ENABLE_KTRACE_PM_JOB_SUBMIT_AFTER_POWERED_UP :                         \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_PWRON ?                        \
		       ENABLE_KTRACE_PM_PWRON :                                               \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_PWRON_TILER ?                  \
		       ENABLE_KTRACE_PM_PWRON_TILER :                                         \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_PWRON_L2 ?                     \
		       ENABLE_KTRACE_PM_PWRON_L2 :                                            \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_PWROFF ?                       \
		       ENABLE_KTRACE_PM_PWROFF :                                              \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_PWROFF_TILER ?                 \
		       ENABLE_KTRACE_PM_PWROFF_TILER :                                        \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_PWROFF_L2 ?                    \
		       ENABLE_KTRACE_PM_PWROFF_L2 :                                           \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CORES_POWERED ?                \
		       ENABLE_KTRACE_PM_CORES_POWERED :                                       \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CORES_POWERED_TILER ?          \
		       ENABLE_KTRACE_PM_CORES_POWERED_TILER :                                 \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CORES_POWERED_L2 ?             \
		       ENABLE_KTRACE_PM_CORES_POWERED_L2 :                                    \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_PWRON_NEURAL ?                 \
		       ENABLE_KTRACE_PM_PWRON_NEURAL :                                        \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_PWROFF_NEURAL ?                \
		       ENABLE_KTRACE_PM_PWROFF_NEURAL :                                       \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CORES_POWERED_NEURAL ?         \
		       ENABLE_KTRACE_PM_CORES_POWERED_NEURAL :                                \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CORES_CHANGE_DESIRED ?         \
		       ENABLE_KTRACE_PM_CORES_CHANGE_DESIRED :                                \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CORES_CHANGE_DESIRED_TILER ?   \
		       ENABLE_KTRACE_PM_CORES_CHANGE_DESIRED_TILER :                          \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CORES_CHANGE_AVAILABLE ?       \
		       ENABLE_KTRACE_PM_CORES_CHANGE_AVAILABLE :                              \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CORES_CHANGE_AVAILABLE_TILER ? \
		       ENABLE_KTRACE_PM_CORES_CHANGE_AVAILABLE_TILER :                        \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CORES_CHANGE_AVAILABLE_L2 ?    \
		       ENABLE_KTRACE_PM_CORES_CHANGE_AVAILABLE_L2 :                           \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CORES_AVAILABLE ?              \
		       ENABLE_KTRACE_PM_CORES_AVAILABLE :                                     \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CORES_AVAILABLE_TILER ?        \
		       ENABLE_KTRACE_PM_CORES_AVAILABLE_TILER :                               \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_DESIRED_REACHED ?              \
		       ENABLE_KTRACE_PM_DESIRED_REACHED :                                     \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_DESIRED_REACHED_TILER ?        \
		       ENABLE_KTRACE_PM_DESIRED_REACHED_TILER :                               \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_RELEASE_CHANGE_SHADER_NEEDED ? \
		       ENABLE_KTRACE_PM_RELEASE_CHANGE_SHADER_NEEDED :                        \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_RELEASE_CHANGE_TILER_NEEDED ?  \
		       ENABLE_KTRACE_PM_RELEASE_CHANGE_TILER_NEEDED :                         \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_REQUEST_CHANGE_SHADER_NEEDED ? \
		       ENABLE_KTRACE_PM_REQUEST_CHANGE_SHADER_NEEDED :                        \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_REQUEST_CHANGE_TILER_NEEDED ?  \
		       ENABLE_KTRACE_PM_REQUEST_CHANGE_TILER_NEEDED :                         \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_WAKE_WAITERS ?                 \
		       ENABLE_KTRACE_PM_WAKE_WAITERS :                                        \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CONTEXT_ACTIVE ?               \
		       ENABLE_KTRACE_PM_CONTEXT_ACTIVE :                                      \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CONTEXT_IDLE ?                 \
		       ENABLE_KTRACE_PM_CONTEXT_IDLE :                                        \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_GPU_ON ?                       \
		       ENABLE_KTRACE_PM_GPU_ON :                                              \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_GPU_OFF ?                      \
		       ENABLE_KTRACE_PM_GPU_OFF :                                             \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_SET_POLICY ?                   \
		       ENABLE_KTRACE_PM_SET_POLICY :                                          \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CA_SET_POLICY ?                \
		       ENABLE_KTRACE_PM_CA_SET_POLICY :                                       \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CURRENT_POLICY_INIT ?          \
		       ENABLE_KTRACE_PM_CURRENT_POLICY_INIT :                                 \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_CURRENT_POLICY_TERM ?          \
		       ENABLE_KTRACE_PM_CURRENT_POLICY_TERM :                                 \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_POWEROFF_WAIT_WQ ?             \
		       ENABLE_KTRACE_PM_POWEROFF_WAIT_WQ :                                    \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_RUNTIME_SUSPEND_CALLBACK ?     \
		       ENABLE_KTRACE_PM_RUNTIME_SUSPEND_CALLBACK :                            \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_PM_RUNTIME_RESUME_CALLBACK ?      \
		       ENABLE_KTRACE_PM_RUNTIME_RESUME_CALLBACK :                             \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHED_RETAIN_CTX_NOLOCK ?         \
		       ENABLE_KTRACE_SCHED_RETAIN_CTX_NOLOCK :                                \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_SCHED_RELEASE_CTX ?               \
		       ENABLE_KTRACE_SCHED_RELEASE_CTX :                                      \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_ARB_VM_STATE ?                    \
		       ENABLE_KTRACE_ARB_VM_STATE :                                           \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_ARB_VM_EVT ?                      \
		       ENABLE_KTRACE_ARB_VM_EVT :                                             \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_ARB_GPU_GRANTED ?                 \
		       ENABLE_KTRACE_ARB_GPU_GRANTED :                                        \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_ARB_GPU_LOST ?                    \
		       ENABLE_KTRACE_ARB_GPU_LOST :                                           \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_ARB_GPU_STARTED ?                 \
		       ENABLE_KTRACE_ARB_GPU_STARTED :                                        \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_ARB_GPU_STOP_REQUESTED ?          \
		       ENABLE_KTRACE_ARB_GPU_STOP_REQUESTED :                                 \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_ARB_GPU_STOPPED ?                 \
		       ENABLE_KTRACE_ARB_GPU_STOPPED :                                        \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_ARB_GPU_REQUESTED ?               \
		       ENABLE_KTRACE_ARB_GPU_REQUESTED :                                      \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_TILER_CHUNK_ALLOC ?               \
		       ENABLE_KTRACE_TILER_CHUNK_ALLOC :                                      \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_TILER_CHUNK_ALLOC_SIZE ?          \
		       ENABLE_KTRACE_TILER_CHUNK_ALLOC_SIZE :                                 \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_TILER_CHUNK_FREE ?                \
		       ENABLE_KTRACE_TILER_CHUNK_FREE :                                       \
	 KBASE_KTRACE_CODE(code) == KBASE_KTRACE_CODE_TILER_CHUNK_FREE_SIZE ?           \
		       ENABLE_KTRACE_TILER_CHUNK_FREE_SIZE :                                  \
		       0)

/**
 * KBASE_KTRACE_ADD - Add trace values
 * @kbdev:    kbase device
 * @code:     trace code
 * @kctx:     kbase context, or NULL if no context
 * @info_val: generic information about @code to add to the trace
 *
 * Note: Any functions called through this macro will still be evaluated in
 * Release builds (CONFIG_MALI_DEBUG not defined). Therefore, when
 * KBASE_KTRACE_ENABLE == 0 any functions called to get the parameters supplied
 * to this macro must:
 * a) be static or static inline, and
 * b) just return 0 and have no other statements present in the body.
 */
#define KBASE_KTRACE_ADD(kbdev, code, kctx, info_val)                             \
	do {                                                                      \
		/* capture values that could come from non-pure function calls */ \
		u64 __info_val = info_val;                                        \
		if (IS_KTRACE_ADD_ENABLED(code)) {                                \
			KBASE_KTRACE_RBUF_ADD(kbdev, code, kctx, __info_val);     \
			KBASE_KTRACE_FTRACE_ADD(kbdev, code, kctx, __info_val);   \
		}                                                                 \
	} while (0)

/**
 * KBASE_KTRACE_CLEAR - Clear the trace, if applicable to the target(s)
 * @kbdev:    kbase device
 */
#define KBASE_KTRACE_CLEAR(kbdev)                 \
	do {                                      \
		KBASE_KTRACE_RBUF_CLEAR(kbdev);   \
		KBASE_KTRACE_FTRACE_CLEAR(kbdev); \
	} while (0)

/**
 * KBASE_KTRACE_DUMP - Dump the trace, if applicable to the target(s)
 * @kbdev:    kbase device
 */
#define KBASE_KTRACE_DUMP(kbdev)                 \
	do {                                     \
		KBASE_KTRACE_RBUF_DUMP(kbdev);   \
		KBASE_KTRACE_FTRACE_DUMP(kbdev); \
	} while (0)

#endif /* _KBASE_DEBUG_KTRACE_H_ */

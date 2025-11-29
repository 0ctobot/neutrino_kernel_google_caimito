#ifndef KSU_SUSFS_DEF_H
#define KSU_SUSFS_DEF_H

#include <linux/bits.h>

/* Shared with userspace ksu_susfs tool */
#define SUSFS_MAGIC 0xFAFAFAFA
#define CMD_SUSFS_HIDE_SUS_MNTS_FOR_ALL_PROCS 0x55561
#define CMD_SUSFS_ENABLE_LOG 0x555a0
#define CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG 0x555b0
#define CMD_SUSFS_SHOW_VERSION 0x555e1
#define CMD_SUSFS_SHOW_ENABLED_FEATURES 0x555e2
#define CMD_SUSFS_SHOW_VARIANT 0x555e3
#define CMD_SUSFS_ENABLE_AVC_LOG_SPOOFING 0x60010
#define SUSFS_MAX_LEN_PATHNAME 256
#define SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE 8192
#define SUSFS_ENABLED_FEATURES_SIZE 8192
#define SUSFS_MAX_VERSION_BUFSIZE 16
#define SUSFS_MAX_VARIANT_BUFSIZE 16
#define DEFAULT_KSU_MNT_ID 500000
#define DEFAULT_KSU_MNT_GROUP_ID 5000
#define TIF_PROC_UMOUNTED 33
#define AS_FLAGS_SUS_MOUNT 34
#define BIT_SUS_MOUNT BIT(34)
#define MAGIC_MOUNT_WORKDIR "/debug_ramdisk/workdir"

static inline bool susfs_is_current_proc_umounted(void) {
	return test_ti_thread_flag(&current->thread_info, TIF_PROC_UMOUNTED);
}

static inline void susfs_set_current_proc_umounted(void) {
	set_ti_thread_flag(&current->thread_info, TIF_PROC_UMOUNTED);
}
#endif

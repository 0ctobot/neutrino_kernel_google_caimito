#ifndef KSU_SUSFS_DEF_H
#define KSU_SUSFS_DEF_H

#include <linux/bits.h>

/* Shared with userspace ksu_susfs tool */
#define CMD_SUSFS_ADD_SUS_MOUNT 0x55560
#define CMD_SUSFS_HIDE_SUS_MNTS_FOR_ALL_PROCS 0x55561
#define CMD_SUSFS_UMOUNT_FOR_ZYGOTE_ISO_SERVICE 0x55562
#define CMD_SUSFS_ADD_TRY_UMOUNT 0x55580
#define CMD_SUSFS_ENABLE_LOG 0x555a0
#define CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG 0x555b0
#define CMD_SUSFS_SHOW_VERSION 0x555e1
#define CMD_SUSFS_SHOW_ENABLED_FEATURES 0x555e2
#define CMD_SUSFS_SHOW_VARIANT 0x555e3
#define CMD_SUSFS_ENABLE_AVC_LOG_SPOOFING 0x60010
#define SUSFS_MAX_LEN_PATHNAME 256
#define SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE 4096
#define TRY_UMOUNT_DEFAULT 0
#define TRY_UMOUNT_DETACH 1
#define DEFAULT_KSU_MNT_ID 300000
#define DEFAULT_KSU_MNT_GROUP_ID 3000
#define TIF_PROC_UMOUNTED 33
#define AS_FLAGS_SUS_MOUNT 25
#define BIT_SUS_MOUNT BIT(25)
#define MAGIC_MOUNT_WORKDIR "/debug_ramdisk/workdir"
#define DATA_ADB_UMOUNT_FOR_ZYGOTE_SYSTEM_PROCESS "/data/adb/susfs_umount_for_zygote_system_process"
#define DATA_ADB_NO_AUTO_ADD_SUS_BIND_MOUNT "/data/adb/susfs_no_auto_add_sus_bind_mount"
#define DATA_ADB_NO_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT "/data/adb/susfs_no_auto_add_sus_ksu_default_mount"
#define DATA_ADB_NO_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT "/data/adb/susfs_no_auto_add_try_umount_for_bind_mount"

static inline bool susfs_is_current_proc_umounted(void) {
	return test_ti_thread_flag(&current->thread_info, TIF_PROC_UMOUNTED);
}

static inline void susfs_set_current_proc_umounted(void) {
	set_ti_thread_flag(&current->thread_info, TIF_PROC_UMOUNTED);
}
#endif

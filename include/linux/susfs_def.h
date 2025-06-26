#ifndef KSU_SUSFS_DEF_H
#define KSU_SUSFS_DEF_H

#include <linux/bits.h>

/* ENUM */

/* Shared with userspace ksu_susfs tool */
#define CMD_SUSFS_ADD_SUS_PATH 0x55550
#define CMD_SUSFS_SET_ANDROID_DATA_ROOT_PATH 0x55551
#define CMD_SUSFS_SET_SDCARD_ROOT_PATH 0x55552
#define CMD_SUSFS_ADD_SUS_PATH_LOOP 0x55553
#define CMD_SUSFS_ADD_SUS_MOUNT 0x55560
#define CMD_SUSFS_HIDE_SUS_MNTS_FOR_ALL_PROCS 0x55561
#define CMD_SUSFS_UMOUNT_FOR_ZYGOTE_ISO_SERVICE 0x55562
#define CMD_SUSFS_ADD_TRY_UMOUNT 0x55580
#define CMD_SUSFS_ENABLE_LOG 0x555a0
#define CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG 0x555b0
#define CMD_SUSFS_RUN_UMOUNT_FOR_CURRENT_MNT_NS 0x555d0
#define CMD_SUSFS_SHOW_VERSION 0x555e1
#define CMD_SUSFS_SHOW_ENABLED_FEATURES 0x555e2
#define CMD_SUSFS_SHOW_VARIANT 0x555e3
/* 256 should cover most paths, define custom length if needed */
#define SUSFS_MAX_LEN_PATHNAME 256
#define SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE 4096
#define TRY_UMOUNT_DEFAULT 0
#define TRY_UMOUNT_DETACH 1
#define DEFAULT_SUS_MNT_ID 100000
#define DEFAULT_SUS_MNT_ID_FOR_KSU_PROC_UNSHARE 1000000
#define DEFAULT_SUS_MNT_GROUP_ID 1000
#define TIF_NON_ROOT_USER_APP_PROC 33
#define AS_FLAGS_SUS_PATH 24
#define AS_FLAGS_SUS_MOUNT 25
#define AS_FLAGS_ANDROID_DATA_ROOT_DIR 28
#define AS_FLAGS_SDCARD_ROOT_DIR 29
#define BIT_SUS_PATH BIT(24)
#define BIT_SUS_MOUNT BIT(25)
#define BIT_SUS_KSTAT BIT(26)
#define BIT_ANDROID_DATA_ROOT_DIR BIT(28)
#define BIT_ANDROID_SDCARD_ROOT_DIR BIT(29)
#define ND_STATE_LOOKUP_LAST 32
#define ND_STATE_OPEN_LAST 64
#define ND_STATE_LAST_SDCARD_SUS_PATH 128
#define ND_FLAGS_LOOKUP_LAST 0x2000000
#define MAGIC_MOUNT_WORKDIR "/debug_ramdisk/workdir"
#define DATA_ADB_UMOUNT_FOR_ZYGOTE_SYSTEM_PROCESS "/data/adb/susfs_umount_for_zygote_system_process"
#define DATA_ADB_NO_AUTO_ADD_SUS_BIND_MOUNT "/data/adb/susfs_no_auto_add_sus_bind_mount"
#define DATA_ADB_NO_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT "/data/adb/susfs_no_auto_add_sus_ksu_default_mount"
#define DATA_ADB_NO_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT "/data/adb/susfs_no_auto_add_try_umount_for_bind_mount"

static inline bool susfs_is_current_non_root_user_app_proc(void) {
	return test_ti_thread_flag(&current->thread_info, TIF_NON_ROOT_USER_APP_PROC);
}

static inline void susfs_set_current_non_root_user_app_proc(void) {
	set_ti_thread_flag(&current->thread_info, TIF_NON_ROOT_USER_APP_PROC);
}

static inline bool susfs_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str++ != *prefix++)
            return false;
    }
    return true;
}
#endif

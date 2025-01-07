#ifndef KSU_SUSFS_DEF_H
#define KSU_SUSFS_DEF_H

#include <linux/bits.h>

/* ENUM */

/* Shared with userspace ksu_susfs tool. */
#define CMD_SUSFS_ADD_SUS_PATH 0x55550
#define CMD_SUSFS_ADD_SUS_MOUNT 0x55560
#define CMD_SUSFS_ADD_TRY_UMOUNT 0x55580
#define CMD_SUSFS_ENABLE_LOG 0x555a0
#define CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG 0x555b0
#define CMD_SUSFS_RUN_UMOUNT_FOR_CURRENT_MNT_NS 0x555d0
#define CMD_SUSFS_SHOW_VERSION 0x555e1
#define CMD_SUSFS_SHOW_ENABLED_FEATURES 0x555e2
#define CMD_SUSFS_SHOW_VARIANT 0x555e3

/* 256 should address many paths already unless you are doing some
 * strange experimental stuff, then set your own desired length. */
#define SUSFS_MAX_LEN_PATHNAME 256
#define SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE 4096
#define TRY_UMOUNT_DEFAULT 0
#define TRY_UMOUNT_DETACH 1

/*
 * inode->i_state => storing flag 'INODE_STATE_'
 * mount->mnt.susfs_orig_mnt_id => storing original mnt_id
 * task_struct->susfs_last_fake_mnt_id => storing last valid fake mnt_id
 * task_struct->susfs_task_state => storing flag 'TASK_STRUCT_KABI'
 */

#define INODE_STATE_SUS_PATH BIT(24)
#define INODE_STATE_SUS_MOUNT BIT(25)
#define TASK_STRUCT_NON_ROOT_USER_APP_PROC BIT(24)

#define MAGIC_MOUNT_WORKDIR "/debug_ramdisk/workdir"
#endif

#ifndef KSU_SUSFS_H
#define KSU_SUSFS_H

#include <linux/version.h>
#include <linux/types.h>
#include <linux/utsname.h>
#include <linux/mount.h>
#include <linux/hashtable.h>
#include <linux/path.h>

/* ENUM */

/* Shared with userspace ksu_susfs tool. */
#define CMD_SUSFS_ADD_SUS_MOUNT 0x55560
#define CMD_SUSFS_ADD_TRY_UMOUNT 0x55580
#define CMD_SUSFS_ENABLE_LOG 0x555a0
#define CMD_SUSFS_SET_BOOTCONFIG 0x555b0

/* 256 should address many paths already unless you are doing some
 * strange experimental stuff, then set your own desired length. */
#define SUSFS_MAX_LEN_PATHNAME 256
#define SUSFS_FAKE_BOOT_CONFIG_SIZE 4096
#define TRY_UMOUNT_DEFAULT 0
#define TRY_UMOUNT_DETACH 1

/*
 * inode->i_state => storing flag 'INODE_STATE_'
 * inode->android_kabi_reserved1 => storing fake i_ino
 * inode->android_kabi_reserved2 => storing fake i_dev
 * inode->super_block->android_kabi_reserved1 => storing fake i_nlink
 * inode->super_block->android_kabi_reserved2 => storing fake i_size
 * inode->super_block->android_kabi_reserved3 => storing fake i_blocks
 * user_struct->android_kabi_reserved1 => storing flag 'USER_STRUCT_KABI1_'
 */

/* 1 << 25 */
#define INODE_STATE_SUS_MOUNT 33554432
/* 1 << 0 */
#define TASK_STRUCT_KABI1_IS_ZYGOTE 1
/* 1 << 24, for distinguishing root/non-root granted user app process. */
#define USER_STRUCT_KABI1_NON_ROOT_USER_APP_PROFILE 16777216

/* MACRO */

#define getname_safe(name) (name == NULL ? ERR_PTR(-EINVAL) : getname(name))
#define putname_safe(name) (IS_ERR(name) ? NULL : putname(name))

/* STRUCT */

/* sus_mount */
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
struct st_susfs_sus_mount {
	char                                    target_pathname[SUSFS_MAX_LEN_PATHNAME];
	unsigned long                           target_dev;
};
struct st_susfs_sus_mount_list {
	struct list_head                        list;
	struct st_susfs_sus_mount               info;
};
#endif
/* try_umount */
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
struct st_susfs_try_umount {
	char                                    target_pathname[SUSFS_MAX_LEN_PATHNAME];
	int                                     mnt_mode;
};
struct st_susfs_try_umount_list {
	struct list_head                        list;
	struct st_susfs_try_umount              info;
};
#endif

/* FORWARD DECLARATION */

/* sus_mount */
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
int susfs_add_sus_mount(struct st_susfs_sus_mount* __user user_info);
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_BIND_MOUNT
int susfs_auto_add_bind_mount(const char *pathname, struct path *path_target);
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_DEFAULT_MOUNT
void susfs_auto_add_default_mount(const char __user *to_pathname);
#endif
#endif
/* try_umount */
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
int susfs_add_try_umount(struct st_susfs_try_umount* __user user_info);
void susfs_try_umount(uid_t target_uid);
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT
void susfs_auto_add_try_umount(struct path *path);
#endif
#endif
/* set_log */
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
void susfs_set_log(bool enabled);
#endif
/* spoof_bootconfig */
#ifdef CONFIG_KSU_SUSFS_SPOOF_BOOTCONFIG
int susfs_set_bootconfig(char* __user user_fake_boot_config);
int susfs_spoof_bootconfig(struct seq_file *m);
#endif
/* susfs_init */
void susfs_init(void);
#endif

#ifndef KSU_SUSFS_H
#define KSU_SUSFS_H

#include <linux/version.h>
#include <linux/types.h>
#include <linux/utsname.h>
#include <linux/hashtable.h>
#include <linux/path.h>

#define SUSFS_VERSION "v1.5.3"
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,0,0)
#define SUSFS_VARIANT "NON-GKI"
#else
#define SUSFS_VARIANT "GKI"
#endif

/* ENUM */

/* Shared with userspace ksu_susfs tool. */
#define CMD_SUSFS_ADD_SUS_PATH 0x55550
#define CMD_SUSFS_ADD_SUS_MOUNT 0x55560
#define CMD_SUSFS_ADD_TRY_UMOUNT 0x55580
#define CMD_SUSFS_ENABLE_LOG 0x555a0
#define CMD_SUSFS_SET_BOOTCONFIG 0x555b0
#define CMD_SUSFS_RUN_UMOUNT_FOR_CURRENT_MNT_NS 0x555d0
#define CMD_SUSFS_SHOW_VERSION 0x555e1
#define CMD_SUSFS_SHOW_ENABLED_FEATURES 0x555e2
#define CMD_SUSFS_SHOW_VARIANT 0x555e3

/* 256 should address many paths already unless you are doing some
 * strange experimental stuff, then set your own desired length. */
#define SUSFS_MAX_LEN_PATHNAME 256
#define SUSFS_FAKE_BOOT_CONFIG_SIZE 4096
#define TRY_UMOUNT_DEFAULT 0
#define TRY_UMOUNT_DETACH 1

/*
 * inode->i_state => storing flag 'INODE_STATE_'
 * mount->mnt.android_kabi_reserved4 => storing original mnt_id
 * task_struct->android_kabi_reserved8 => storing last valid fake mnt_id
 * user_struct->android_kabi_reserved2 => storing flag 'USER_STRUCT_KABI2_'
 */

/* 1 << 24 */
#define INODE_STATE_SUS_PATH 16777216
/* 1 << 25 */
#define INODE_STATE_SUS_MOUNT 33554432
/* 1 << 24, for distinguishing root/non-root granted user app process. */
#define USER_STRUCT_KABI2_NON_ROOT_USER_APP_PROFILE 16777216

/* MACRO */

#define getname_safe(name) (name == NULL ? ERR_PTR(-EINVAL) : getname(name))
#define putname_safe(name) (IS_ERR(name) ? NULL : putname(name))

/* STRUCT */

/* sus_path */
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
struct st_susfs_sus_path {
	unsigned long                           target_ino;
	char                                    target_pathname[SUSFS_MAX_LEN_PATHNAME];
};
struct st_susfs_sus_path_hlist {
	unsigned long                           target_ino;
	char                                    target_pathname[SUSFS_MAX_LEN_PATHNAME];
	struct hlist_node                       node;
};
#endif
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

/* sus_path */
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
int susfs_add_sus_path(struct st_susfs_sus_path* __user user_info);
int susfs_sus_ino_for_filldir64(unsigned long ino);
#endif
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

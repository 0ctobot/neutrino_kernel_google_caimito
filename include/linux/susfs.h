#ifndef KSU_SUSFS_H
#define KSU_SUSFS_H

#include <linux/version.h>
#include <linux/types.h>
#include <linux/utsname.h>
#include <linux/hashtable.h>
#include <linux/path.h>
#include <linux/susfs_def.h>

#define SUSFS_VERSION "v1.5.4"
#define SUSFS_VARIANT "GKI"

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
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT
int susfs_auto_add_sus_bind_mount(const char *pathname, struct path *path_target);
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT
void susfs_auto_add_sus_ksu_default_mount(const char __user *to_pathname);
#endif
#endif
/* try_umount */
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
int susfs_add_try_umount(struct st_susfs_try_umount* __user user_info);
void susfs_try_umount(uid_t target_uid);
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT
void susfs_auto_add_try_umount_for_bind_mount(struct path *path);
#endif
#endif
/* set_log */
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
void susfs_set_log(bool enabled);
#endif
/* spoof_cmdline_or_bootconfig */
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
int susfs_set_cmdline_or_bootconfig(char* __user user_fake_boot_config);
int susfs_spoof_cmdline_or_bootconfig(struct seq_file *m);
#endif
/* susfs_init */
void susfs_init(void);
#endif

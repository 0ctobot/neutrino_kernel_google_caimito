#include <linux/version.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/printk.h>
#include <linux/namei.h>
#include <linux/list.h>
#include <linux/init_task.h>
#include <linux/spinlock.h>
#include <linux/stat.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/fdtable.h>
#include <linux/statfs.h>
#include <linux/susfs.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,15,0)
#include "pnode.h"
#endif

static spinlock_t susfs_spin_lock;
#define MAGIC_MOUNT_WORKDIR "/debug_ramdisk/workdir"
static const size_t strlen_debug_ramdisk_workdir = strlen(MAGIC_MOUNT_WORKDIR);

extern bool susfs_is_current_ksu_domain(void);
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
extern void ksu_try_umount(const char *mnt, bool check_mnt, int flags);
#endif

#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
bool susfs_is_log_enabled __read_mostly = true;
#define SUSFS_LOGI(fmt, ...) \
	if (susfs_is_log_enabled) \
	    pr_info("susfs:[%u][%d][%s] " fmt, \
	            current_uid().val, current->pid, __func__, ##__VA_ARGS__)
#define SUSFS_LOGE(fmt, ...) \
	if (susfs_is_log_enabled) \
	    pr_err("susfs:[%u][%d][%s] " fmt, \
	           current_uid().val, current->pid, __func__, ##__VA_ARGS__)
#else
#define SUSFS_LOGI(fmt, ...)
#define SUSFS_LOGE(fmt, ...)
#endif

/* sus_path */
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
static DEFINE_HASHTABLE(SUS_PATH_HLIST, 10);
static int susfs_update_sus_path_inode(char *target_pathname) {
	struct path p;
	struct inode *inode = NULL;

	if (kern_path(target_pathname, LOOKUP_FOLLOW, &p)) {
		SUSFS_LOGE("Failed opening file '%s'\n", target_pathname);
		return 1;
	}

	/* tmpfs paths will cause wrong comparisons in susfs_sus_ino_for_filldir64() */
	if (strcmp(p.mnt->mnt_sb->s_type->name, "tmpfs") == 0) {
		SUSFS_LOGE("target_pathname: '%s' cannot be added since its filesystem is 'tmpfs'\n",
		           target_pathname);
		path_put(&p);
		return 1;
	}

	inode = d_inode(p.dentry);
	if (!inode) {
		SUSFS_LOGE("inode is NULL\n");
		path_put(&p);
		return 1;
	}

	spin_lock(&inode->i_lock);
	inode->i_state |= INODE_STATE_SUS_PATH;
	spin_unlock(&inode->i_lock);

	path_put(&p);
	return 0;
}

int susfs_add_sus_path(struct st_susfs_sus_path* __user user_info) {
	struct st_susfs_sus_path info;
	struct st_susfs_sus_path_hlist *new_entry, *tmp_entry;
	struct hlist_node *tmp_node;
	int bkt;
	bool update_hlist = false;

	if (copy_from_user(&info, user_info, sizeof(info))) {
		SUSFS_LOGE("failed copying from userspace\n");
		return 1;
	}

	spin_lock(&susfs_spin_lock);
	hash_for_each_safe(SUS_PATH_HLIST, bkt, tmp_node, tmp_entry, node) {
	if (!strcmp(tmp_entry->target_pathname, info.target_pathname)) {
			hash_del(&tmp_entry->node);
			kfree(tmp_entry);
			update_hlist = true;
			break;
		}
	}
	spin_unlock(&susfs_spin_lock);

	new_entry = kmalloc(sizeof(struct st_susfs_sus_path_hlist), GFP_KERNEL);
	if (!new_entry) {
		SUSFS_LOGE("not enough memory\n");
		return 1;
	}

	new_entry->target_ino = info.target_ino;
	strncpy(new_entry->target_pathname, info.target_pathname, SUSFS_MAX_LEN_PATHNAME-1);
	if (susfs_update_sus_path_inode(new_entry->target_pathname)) {
		kfree(new_entry);
		return 1;
	}
	spin_lock(&susfs_spin_lock);
	hash_add(SUS_PATH_HLIST, &new_entry->node, info.target_ino);
	if (update_hlist) {
		SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s' is successfully updated to SUS_PATH_HLIST\n",
		           new_entry->target_ino, new_entry->target_pathname);	
	} else {
		SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s' is successfully added to SUS_PATH_HLIST\n",
		           new_entry->target_ino, new_entry->target_pathname);
	}
	spin_unlock(&susfs_spin_lock);
	return 0;
}

int susfs_sus_ino_for_filldir64(unsigned long ino) {
	struct st_susfs_sus_path_hlist *entry;

	hash_for_each_possible(SUS_PATH_HLIST, entry, node, ino) {
		if (entry->target_ino == ino)
			return 1;
	}
	return 0;
}
#endif

/* sus_mount */
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
static LIST_HEAD(LH_SUS_MOUNT);
static void susfs_update_sus_mount_inode(char *target_pathname) {
	struct path p;
	struct inode *inode = NULL;
	int err = 0;

	err = kern_path(target_pathname, LOOKUP_FOLLOW, &p);
	if (err) {
		SUSFS_LOGE("Failed opening file '%s'\n", target_pathname);
		return;
	}

	inode = d_inode(p.dentry);
	if (!inode) {
		path_put(&p);
		SUSFS_LOGE("inode is NULL\n");
		return;
	}

	spin_lock(&inode->i_lock);
	inode->i_state |= INODE_STATE_SUS_MOUNT;
	spin_unlock(&inode->i_lock);

	path_put(&p);
}

int susfs_add_sus_mount(struct st_susfs_sus_mount* __user user_info) {
	struct st_susfs_sus_mount_list *cursor = NULL, *temp = NULL;
	struct st_susfs_sus_mount_list *new_list = NULL;
	struct st_susfs_sus_mount info;

	if (copy_from_user(&info, user_info, sizeof(info))) {
		SUSFS_LOGE("failed copying from userspace\n");
		return 1;
	}

#if defined(__ARCH_WANT_STAT64) || defined(__ARCH_WANT_COMPAT_STAT64)
#ifdef CONFIG_MIPS
	info.target_dev = new_decode_dev(info.target_dev);
#else
	info.target_dev = huge_decode_dev(info.target_dev);
#endif
#else
	info.target_dev = old_decode_dev(info.target_dev);
#endif

	list_for_each_entry_safe(cursor, temp, &LH_SUS_MOUNT, list) {
		if (unlikely(!strcmp(cursor->info.target_pathname, info.target_pathname))) {
			spin_lock(&susfs_spin_lock);
			memcpy(&cursor->info, &info, sizeof(info));
			susfs_update_sus_mount_inode(cursor->info.target_pathname);
			SUSFS_LOGI("target_pathname: '%s', target_dev: '%lu', is successfully updated to LH_SUS_MOUNT\n",
			           cursor->info.target_pathname, cursor->info.target_dev);
			spin_unlock(&susfs_spin_lock);
			return 0;
		}
	}

	new_list = kmalloc(sizeof(struct st_susfs_sus_mount_list), GFP_KERNEL);
	if (!new_list) {
		SUSFS_LOGE("not enough memory\n");
		return 1;
	}

	memcpy(&new_list->info, &info, sizeof(info));
	susfs_update_sus_mount_inode(new_list->info.target_pathname);

	INIT_LIST_HEAD(&new_list->list);
	spin_lock(&susfs_spin_lock);
	list_add_tail(&new_list->list, &LH_SUS_MOUNT);
	SUSFS_LOGI("target_pathname: '%s', target_dev: '%lu', is successfully added to LH_SUS_MOUNT\n",
	           new_list->info.target_pathname, new_list->info.target_dev);
	spin_unlock(&susfs_spin_lock);
	return 0;
}

#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_BIND_MOUNT
int susfs_auto_add_bind_mount(const char *pathname, struct path *path_target) {
	struct inode *inode;

	inode = path_target->dentry->d_inode;
	if (!inode) return 1;
	spin_lock(&inode->i_lock);
	inode->i_state |= INODE_STATE_SUS_MOUNT;
	SUSFS_LOGI("set SUS_MOUNT inode state for source bind mount path '%s'\n", pathname);
	spin_unlock(&inode->i_lock);
	return 0;
}
#endif

#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_DEFAULT_MOUNT
void susfs_auto_add_default_mount(const char __user *to_pathname) {
	char pathname[SUSFS_MAX_LEN_PATHNAME];
	struct path path;
	struct inode *inode;

	/* Here we need to retrieve the new struct path. */
	if (strncpy_from_user(pathname, to_pathname, SUSFS_MAX_LEN_PATHNAME-1) < 0)
		return;
	if ((!strncmp(pathname, "/data/adb/modules", 17) ||
		 !strncmp(pathname, "/debug_ramdisk", 14) ||
		 !strncmp(pathname, "/system", 7) ||
		 !strncmp(pathname, "/system_ext", 11) ||
		 !strncmp(pathname, "/vendor", 7) ||
		 !strncmp(pathname, "/product", 8) ||
		 !strncmp(pathname, "/odm", 4)) &&
		 !kern_path(pathname, LOOKUP_FOLLOW, &path)) {
		goto set_inode_sus_mount;
	}
	return;
set_inode_sus_mount:
	inode = path.dentry->d_inode;
	if (!inode) return;
	spin_lock(&inode->i_lock);
	if (!(inode->i_state & INODE_STATE_SUS_MOUNT)) {
		inode->i_state |= INODE_STATE_SUS_MOUNT;
		SUSFS_LOGI("set SUS_MOUNT inode state for default KSU mount path '%s'\n", pathname);
	}
	spin_unlock(&inode->i_lock);
	path_put(&path);
}
#endif
#endif

/* try_umount */
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
static LIST_HEAD(LH_TRY_UMOUNT_PATH);
int susfs_add_try_umount(struct st_susfs_try_umount* __user user_info) {
	struct st_susfs_try_umount_list *cursor = NULL, *temp = NULL;
	struct st_susfs_try_umount_list *new_list = NULL;
	struct st_susfs_try_umount info;

	if (copy_from_user(&info, user_info, sizeof(info))) {
		SUSFS_LOGE("failed copying from userspace\n");
		return 1;
	}

	list_for_each_entry_safe(cursor, temp, &LH_TRY_UMOUNT_PATH, list) {
		if (unlikely(!strcmp(info.target_pathname, cursor->info.target_pathname))) {
			SUSFS_LOGE("target_pathname: '%s' is already created in LH_TRY_UMOUNT_PATH\n",
			           info.target_pathname);
			return 1;
		}
	}

	new_list = kmalloc(sizeof(struct st_susfs_try_umount_list), GFP_KERNEL);
	if (!new_list) {
		SUSFS_LOGE("not enough memory\n");
		return 1;
	}

	memcpy(&new_list->info, &info, sizeof(info));

	INIT_LIST_HEAD(&new_list->list);
	spin_lock(&susfs_spin_lock);
	list_add_tail(&new_list->list, &LH_TRY_UMOUNT_PATH);
	spin_unlock(&susfs_spin_lock);
	SUSFS_LOGI("target_pathname: '%s', mnt_mode: %d, is successfully added to LH_TRY_UMOUNT_PATH\n",
	           new_list->info.target_pathname, new_list->info.mnt_mode);
	return 0;
}

void susfs_try_umount(uid_t target_uid) {
	struct st_susfs_try_umount_list *cursor = NULL, *temp = NULL;

	list_for_each_entry_safe(cursor, temp, &LH_TRY_UMOUNT_PATH, list) {
		SUSFS_LOGI("umounting '%s' for uid: %d\n", cursor->info.target_pathname, target_uid);
		if (cursor->info.mnt_mode == TRY_UMOUNT_DEFAULT) {
			ksu_try_umount(cursor->info.target_pathname, false, 0);
		} else if (cursor->info.mnt_mode == TRY_UMOUNT_DETACH) {
			ksu_try_umount(cursor->info.target_pathname, false, MNT_DETACH);
		} else {
			SUSFS_LOGE("failed umounting '%s' for uid: %d, mnt_mode '%d' not supported\n",
			           cursor->info.target_pathname, target_uid, cursor->info.mnt_mode);
		}
	}
}

#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT
void susfs_auto_add_try_umount(struct path *path) {
	struct st_susfs_try_umount_list *cursor = NULL, *temp = NULL;
	struct st_susfs_try_umount_list *new_list = NULL;
	char *pathname = NULL, *dpath = NULL;
	bool is_magic_mount_path = false;

	pathname = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!pathname) {
		SUSFS_LOGE("not enough memory\n");
		return;
	}

	dpath = d_path(path, pathname, PAGE_SIZE);
	if (!dpath) {
		SUSFS_LOGE("dpath is NULL\n");
		goto out_free_pathname;
	}

	if (strstr(dpath, MAGIC_MOUNT_WORKDIR)) {
		is_magic_mount_path = true;
	}

	list_for_each_entry_safe(cursor, temp, &LH_TRY_UMOUNT_PATH, list) {
		if (is_magic_mount_path && strstr(dpath, cursor->info.target_pathname)) {
				goto out_free_pathname;
		} else if (unlikely(!strcmp(dpath, cursor->info.target_pathname))) {
			SUSFS_LOGE("target_pathname: '%s' is already created in LH_TRY_UMOUNT_PATH\n",
			           dpath);
			goto out_free_pathname;
		}
	}

	new_list = kmalloc(sizeof(struct st_susfs_try_umount_list), GFP_KERNEL);
	if (!new_list) {
		SUSFS_LOGE("not enough memory\n");
		goto out_free_pathname;
	}

	if (is_magic_mount_path) {
		strncpy(new_list->info.target_pathname, dpath + strlen_debug_ramdisk_workdir, SUSFS_MAX_LEN_PATHNAME-1);
	} else {
		strncpy(new_list->info.target_pathname, dpath, SUSFS_MAX_LEN_PATHNAME-1);
	}

	new_list->info.mnt_mode = TRY_UMOUNT_DETACH;

	INIT_LIST_HEAD(&new_list->list);
	spin_lock(&susfs_spin_lock);
	list_add_tail(&new_list->list, &LH_TRY_UMOUNT_PATH);
	spin_unlock(&susfs_spin_lock);
	SUSFS_LOGI("target_pathname: '%s', mnt_mode: %d, is successfully added to LH_TRY_UMOUNT_PATH\n",
	           new_list->info.target_pathname, new_list->info.mnt_mode);
out_free_pathname:
	kfree(pathname);
}
#endif
#endif

/* set_log */
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
void susfs_set_log(bool enabled) {
	spin_lock(&susfs_spin_lock);
	susfs_is_log_enabled = enabled;
	spin_unlock(&susfs_spin_lock);
	if (susfs_is_log_enabled) {
		pr_info("susfs: enable logging to kernel");
	} else {
		pr_info("susfs: disable logging to kernel");
	}
}
#endif

/* spoof_bootconfig */
#ifdef CONFIG_KSU_SUSFS_SPOOF_BOOTCONFIG
static char *fake_boot_config = NULL;
int susfs_set_bootconfig(char* __user user_fake_boot_config) {
	int res;

	if (!fake_boot_config) {
		fake_boot_config = kmalloc(SUSFS_FAKE_BOOT_CONFIG_SIZE, GFP_KERNEL);
		if (!fake_boot_config) {
			SUSFS_LOGE("not enough memory\n");
			return -ENOMEM;
		}
	}

	spin_lock(&susfs_spin_lock);
	memset(fake_boot_config, 0, SUSFS_FAKE_BOOT_CONFIG_SIZE);
	res = strncpy_from_user(fake_boot_config, user_fake_boot_config, SUSFS_FAKE_BOOT_CONFIG_SIZE-1);
	spin_unlock(&susfs_spin_lock);

	if (res > 0) {
		SUSFS_LOGI("fake_boot_config is set, length of string: %zu\n", strlen(fake_boot_config));
		return 0;
	}
	SUSFS_LOGI("failed setting fake_boot_config\n");
	return res;
}

int susfs_spoof_bootconfig(struct seq_file *m) {
	if (fake_boot_config != NULL) {
		seq_puts(m, fake_boot_config);
		return 0;
	}
	return 1;
}
#endif

/* susfs_init */
void susfs_init(void) {
	spin_lock_init(&susfs_spin_lock);
	SUSFS_LOGI("susfs is initialized! version: " SUSFS_VERSION " \n");
}

/*
 * No exit is needed becuase SUSFS should never be compiled as a module.
 * void __init susfs_exit(void)
 */


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
#include <linux/random.h>
#include <linux/susfs.h>
#include "mount.h"

static spinlock_t susfs_spin_lock;

extern bool susfs_is_current_ksu_domain(void);
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
extern void try_umount(const char *mnt, bool check_mnt, int flags, uid_t uid);
#endif
extern bool susfs_is_avc_log_spoofing_enabled;

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

bool susfs_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str++ != *prefix++)
            return false;
    }
    return true;
}

/* sus_mount */
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
static LIST_HEAD(LH_SUS_MOUNT);
static void susfs_update_sus_mount_inode(char *target_pathname) {
	struct mount *mnt = NULL;
	struct path p;
	struct inode *inode = NULL;
	int err = 0;

	err = kern_path(target_pathname, LOOKUP_FOLLOW, &p);
	if (err) {
		SUSFS_LOGE("Failed opening file '%s'\n", target_pathname);
		return;
	}

	/* Check if the mount has a legitimate peer group id, if so we cannot add it to sus_mount */
	mnt = real_mount(p.mnt);
	if (mnt->mnt_group_id > 0 &&
		mnt->mnt_group_id < DEFAULT_KSU_MNT_GROUP_ID) {
		SUSFS_LOGE("skip setting SUS_MOUNT inode state for path '%s' since its source mount has a legit peer group id\n",
		           target_pathname);
		return;
	}

	inode = d_inode(p.dentry);
	if (!inode) {
		path_put(&p);
		SUSFS_LOGE("inode is NULL\n");
		return;
	}

	if (!(inode->i_mapping->flags & BIT_SUS_MOUNT)) {
		spin_lock(&inode->i_lock);
		set_bit(AS_FLAGS_SUS_MOUNT, &inode->i_mapping->flags);
		spin_unlock(&inode->i_lock);
	}
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

	info.target_dev = old_decode_dev(info.target_dev);

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

#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT
int susfs_auto_add_sus_bind_mount(const char *pathname, struct path *path_target) {
	struct mount *mnt;
	struct inode *inode;

	mnt = real_mount(path_target->mnt);
	if (mnt->mnt_group_id > 0 &&
		mnt->mnt_group_id < DEFAULT_KSU_MNT_GROUP_ID) {
		return 0;
	}
	inode = path_target->dentry->d_inode;
	if (!inode) return 1;
	if (!(inode->i_mapping->flags & BIT_SUS_MOUNT)) {
		spin_lock(&inode->i_lock);
		set_bit(AS_FLAGS_SUS_MOUNT, &inode->i_mapping->flags);
		spin_unlock(&inode->i_lock);
		SUSFS_LOGI("set SUS_MOUNT inode state for source bind mount path '%s'\n", pathname);
	}
	return 0;
}
#endif

#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT
void susfs_auto_add_sus_ksu_default_mount(const char __user *to_pathname) {
	char *pathname = NULL;
	struct path path;
	struct inode *inode;

	pathname = kmalloc(SUSFS_MAX_LEN_PATHNAME, GFP_KERNEL);
	if (!pathname) {
		SUSFS_LOGE("not enough memory\n");
		return;
	}
	if (strncpy_from_user(pathname, to_pathname, SUSFS_MAX_LEN_PATHNAME-1) < 0) {
		SUSFS_LOGE("strncpy_from_user()\n");
		goto out_free_pathname;
		return;
	}
	if ((!strncmp(pathname, "/data/adb/modules", 17) ||
		 !strncmp(pathname, "/debug_ramdisk", 14) ||
		 !strncmp(pathname, "/system_ext", 11) ||
		 !strncmp(pathname, "/system", 7) ||
		 !strncmp(pathname, "/vendor", 7) ||
		 !strncmp(pathname, "/product", 8) ||
		 !strncmp(pathname, "/odm", 4)) &&
		 !kern_path(pathname, LOOKUP_FOLLOW, &path)) {
		goto set_inode_sus_mount;
	}
	goto out_free_pathname;
set_inode_sus_mount:
	inode = path.dentry->d_inode;
	if (!inode) {
		goto out_path_put;
		return;
	}
	if (!(inode->i_mapping->flags & BIT_SUS_MOUNT)) {
		spin_lock(&inode->i_lock);
		set_bit(AS_FLAGS_SUS_MOUNT, &inode->i_mapping->flags);
		spin_unlock(&inode->i_lock);
		SUSFS_LOGI("set SUS_MOUNT inode state for default KSU mount path '%s'\n", pathname);
	}
out_path_put:
	path_put(&path);
out_free_pathname:
	kfree(pathname);
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
	struct st_susfs_try_umount_list *cursor = NULL;

	/* umount in reverse order */
	list_for_each_entry_reverse(cursor, &LH_TRY_UMOUNT_PATH, list) {
		if (cursor->info.mnt_mode == TRY_UMOUNT_DEFAULT) {
			try_umount(cursor->info.target_pathname, false, 0, target_uid);
		} else if (cursor->info.mnt_mode == TRY_UMOUNT_DETACH) {
			try_umount(cursor->info.target_pathname, false, MNT_DETACH, target_uid);
		} else {
			SUSFS_LOGE("failed umounting '%s' for uid: %d, mnt_mode '%d' not supported\n",
			           cursor->info.target_pathname, target_uid, cursor->info.mnt_mode);
		}
	}
}

#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT
void susfs_auto_add_try_umount_for_bind_mount(struct path *path) {
	struct st_susfs_try_umount_list *cursor = NULL, *temp = NULL;
	struct st_susfs_try_umount_list *new_list = NULL;
	char *pathname = NULL, *dpath = NULL;
	size_t new_pathname_len = 0;
	bool is_magic_mount = false;

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

	/* Check for magic mount: only keep directory paths and skip others. 
	 * Strip "/debug_ramdisk/workdir" since it's not prefixed in the zygote mnt ns. */
	if (!strncmp(dpath, "/debug_ramdisk/workdir/", 23)) {
		if (path->dentry->d_inode && S_ISDIR(path->dentry->d_inode->i_mode)) {
			new_pathname_len = strlen(dpath) - 22;
			memmove(dpath, dpath+22, new_pathname_len);
			*(dpath + new_pathname_len) = '\0';
			is_magic_mount = true;
			goto check_duplicated_list_head;
		}
		goto out_free_pathname;
	}

check_duplicated_list_head:
	list_for_each_entry_safe(cursor, temp, &LH_TRY_UMOUNT_PATH, list) {
		if (is_magic_mount) {
			if (!strncmp(dpath, cursor->info.target_pathname, strlen(cursor->info.target_pathname))) {
				SUSFS_LOGE("target_pathname: '%s', ino: %lu, is already created in LH_TRY_UMOUNT_PATH\n",
				           dpath, path->dentry->d_inode->i_ino);
				goto out_free_pathname;
			}
		} else if (unlikely(!strcmp(dpath, cursor->info.target_pathname))) {
			SUSFS_LOGE("target_pathname: '%s', ino: %lu, is already created in LH_TRY_UMOUNT_PATH\n",
			           dpath, path->dentry->d_inode->i_ino);
			goto out_free_pathname;
		}
	}

	new_list = kmalloc(sizeof(struct st_susfs_try_umount_list), GFP_KERNEL);
	if (!new_list) {
		SUSFS_LOGE("not enough memory\n");
		goto out_free_pathname;
	}

	strncpy(new_list->info.target_pathname, dpath, SUSFS_MAX_LEN_PATHNAME-1);

	new_list->info.mnt_mode = TRY_UMOUNT_DETACH;

	INIT_LIST_HEAD(&new_list->list);
	spin_lock(&susfs_spin_lock);
	list_add_tail(&new_list->list, &LH_TRY_UMOUNT_PATH);
	spin_unlock(&susfs_spin_lock);
	SUSFS_LOGI("target_pathname: '%s', ino: %lu, mnt_mode: %d, is successfully added to LH_TRY_UMOUNT_PATH\n",
	           new_list->info.target_pathname, path->dentry->d_inode->i_ino, new_list->info.mnt_mode);
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

/* spoof_cmdline_or_bootconfig */
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
static char *fake_cmdline_or_bootconfig = NULL;
int susfs_set_cmdline_or_bootconfig(char* __user user_fake_cmdline_or_bootconfig) {
	int res;

	if (!fake_cmdline_or_bootconfig) {
		fake_cmdline_or_bootconfig = kmalloc(SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE, GFP_KERNEL);
		if (!fake_cmdline_or_bootconfig) {
			SUSFS_LOGE("not enough memory\n");
			return -ENOMEM;
		}
	}

	spin_lock(&susfs_spin_lock);
	memset(fake_cmdline_or_bootconfig, 0, SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE);
	res = strncpy_from_user(fake_cmdline_or_bootconfig, user_fake_cmdline_or_bootconfig, SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE-1);
	spin_unlock(&susfs_spin_lock);

	if (res > 0) {
		SUSFS_LOGI("fake_cmdline_or_bootconfig is set, length of string: %lu\n",
		           strlen(fake_cmdline_or_bootconfig));
		return 0;
	}
	SUSFS_LOGI("failed setting fake_cmdline_or_bootconfig\n");
	return res;
}

int susfs_spoof_cmdline_or_bootconfig(struct seq_file *m) {
	if (fake_cmdline_or_bootconfig != NULL) {
		seq_puts(m, fake_cmdline_or_bootconfig);
		return 0;
	}
	return 1;
}
#endif

static int copy_config_to_buf(const char *config_string, char *buf_ptr, size_t *copied_size, size_t bufsize) {
	size_t tmp_size = strlen(config_string);

	*copied_size += tmp_size;
	if (*copied_size >= bufsize) {
		SUSFS_LOGE("bufsize is not big enough to hold the string.\n");
		return -EINVAL;
	}
	strncpy(buf_ptr, config_string, tmp_size);
	return 0;
}

int susfs_get_enabled_features(char __user* buf, size_t bufsize) {
	char *kbuf = NULL, *buf_ptr = NULL;
	size_t copied_size = 0;
	int err = 0;

	kbuf = kzalloc(bufsize, GFP_KERNEL);
	if (!kbuf) {
		return -ENOMEM;
	}

	buf_ptr = kbuf;
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
	err = copy_config_to_buf("CONFIG_KSU_SUSFS_SUS_MOUNT\n", buf_ptr, &copied_size, bufsize);
	if (err) goto out_kfree_kbuf;
	buf_ptr = kbuf + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT
	err = copy_config_to_buf("CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT\n", buf_ptr, &copied_size, bufsize);
	if (err) goto out_kfree_kbuf;
	buf_ptr = kbuf + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT
	err = copy_config_to_buf("CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT\n", buf_ptr, &copied_size, bufsize);
	if (err) goto out_kfree_kbuf;
	buf_ptr = kbuf + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
	err = copy_config_to_buf("CONFIG_KSU_SUSFS_TRY_UMOUNT\n", buf_ptr, &copied_size, bufsize);
	if (err) goto out_kfree_kbuf;
	buf_ptr = kbuf + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT
	err = copy_config_to_buf("CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT\n", buf_ptr, &copied_size, bufsize);
	if (err) goto out_kfree_kbuf;
	buf_ptr = kbuf + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
	err = copy_config_to_buf("CONFIG_KSU_SUSFS_ENABLE_LOG\n", buf_ptr, &copied_size, bufsize);
	if (err) goto out_kfree_kbuf;
	buf_ptr = kbuf + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS
	err = copy_config_to_buf("CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS\n", buf_ptr, &copied_size, bufsize);
	if (err) goto out_kfree_kbuf;
	buf_ptr = kbuf + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
	err = copy_config_to_buf("CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG\n", buf_ptr, &copied_size, bufsize);
	if (err) goto out_kfree_kbuf;
	buf_ptr = kbuf + copied_size;
#endif
	err = copy_to_user((void __user*)buf, (void *)kbuf, bufsize);
out_kfree_kbuf:
	kfree(kbuf);
	return err;
}

/* susfs avc log spoofing */
void susfs_set_avc_log_spoofing(bool enabled) {
	spin_lock(&susfs_spin_lock);
	susfs_is_avc_log_spoofing_enabled = enabled;
	spin_unlock(&susfs_spin_lock);
	SUSFS_LOGI("enabled: %d\n", enabled);
}

/* susfs_init */
void susfs_init(void) {
	spin_lock_init(&susfs_spin_lock);
	SUSFS_LOGI("susfs is initialized! version: " SUSFS_VERSION " \n");
}

/*
 * No exit is needed becuase SUSFS should never be compiled as a module
 * void __init susfs_exit(void)
 */


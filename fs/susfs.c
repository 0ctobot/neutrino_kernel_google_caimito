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
extern void ksu_try_umount(const char *mnt, bool check_mnt, int flags, uid_t uid);
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
static LIST_HEAD(LH_SUS_PATH_LOOP);
static LIST_HEAD(LH_SUS_PATH_ANDROID_DATA);
static LIST_HEAD(LH_SUS_PATH_SDCARD);
static struct st_android_data_path android_data_path = {0};
static struct st_sdcard_path sdcard_path = {0};
const struct qstr susfs_fake_qstr_name = QSTR_INIT("..5.u.S", 7);

int susfs_set_i_state_on_external_dir(char __user* user_info, int cmd) {
	struct path path;
	int err = 0;
	struct inode *inode = NULL;
	char *info = kmalloc(SUSFS_MAX_LEN_PATHNAME, GFP_KERNEL);
	char *tmp_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	char *resolved_pathname = NULL;

	if (!info) {
		err = -ENOMEM;
		return err;
	}

	if (!tmp_buf) {
		err = -ENOMEM;
		goto out_kfree_info;
	}

	err = strncpy_from_user(info, user_info, SUSFS_MAX_LEN_PATHNAME-1);
	if (err < 0) {
		SUSFS_LOGE("failed copying from userspace\n");
		goto out_kfree_tmp_buf;
	}

	err = kern_path(info, LOOKUP_FOLLOW, &path);
	if (err) {
		SUSFS_LOGE("failed opening file '%s'\n", info);
		goto out_kfree_tmp_buf;
	}

	resolved_pathname = d_path(&path, tmp_buf, PAGE_SIZE);
	if (!resolved_pathname) {
		err = -ENOMEM;
		goto out_path_put_path;
	}

	inode = d_inode(path.dentry);
	if (!inode) {
		err = -EINVAL;
		goto out_path_put_path;
	}
	
	if (cmd == CMD_SUSFS_SET_ANDROID_DATA_ROOT_PATH) {
		spin_lock(&inode->i_lock);
		set_bit(AS_FLAGS_ANDROID_DATA_ROOT_DIR, &inode->i_mapping->flags);
		spin_unlock(&inode->i_lock);
		strncpy(android_data_path.pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME-1);
		android_data_path.is_inited = true;
		SUSFS_LOGI("set android data root dir: '%s', i_mapping: '0x%p'\n",
		           android_data_path.pathname, inode->i_mapping);
	} else if (cmd == CMD_SUSFS_SET_SDCARD_ROOT_PATH) {
		spin_lock(&inode->i_lock);
		set_bit(AS_FLAGS_SDCARD_ROOT_DIR, &inode->i_mapping->flags);
		spin_unlock(&inode->i_lock);
		strncpy(sdcard_path.pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME-1);
		sdcard_path.is_inited = true;
		SUSFS_LOGI("set sdcard root dir: '%s', i_mapping: '0x%p'\n",
		           sdcard_path.pathname, inode->i_mapping);
	} else {
		err = -EINVAL;
	}

out_path_put_path:
	path_put(&path);
out_kfree_tmp_buf:
	kfree(tmp_buf);
out_kfree_info:
	kfree(info);
	return err;
}

int susfs_add_sus_path(struct st_susfs_sus_path* __user user_info) {
	struct st_susfs_sus_path_list *cursor = NULL, *temp = NULL;
	struct st_susfs_sus_path_list *new_list = NULL;
	struct st_susfs_sus_path info;
	struct path path;
	struct inode *inode = NULL;
	char *resolved_pathname = NULL, *tmp_buf = NULL;
	int err = 0;

	err = copy_from_user(&info, user_info, sizeof(info));
	if (err) {
		SUSFS_LOGE("failed copying from userspace\n");
		return err;
	}

	err = kern_path(info.target_pathname, 0, &path);
	if (err) {
		SUSFS_LOGE("failed opening file '%s'\n", info.target_pathname);
		return err;
	}

	if (!path.dentry->d_inode) {
		err = -EINVAL;
		goto out_path_put_path;
	}
	inode = d_inode(path.dentry);

	tmp_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!tmp_buf) {
		err = -ENOMEM;
		goto out_path_put_path;
	}

	resolved_pathname = d_path(&path, tmp_buf, PAGE_SIZE);
	if (!resolved_pathname) {
		err = -ENOMEM;
		goto out_kfree_tmp_buf;
	}

	if (strstr(resolved_pathname, android_data_path.pathname)) {
		if (!android_data_path.is_inited) {
			err = -EINVAL;
			SUSFS_LOGE("android_data_path not configured, run set_android_data_root_path after unlock\n");
			goto out_kfree_tmp_buf;
		}
		list_for_each_entry_safe(cursor, temp, &LH_SUS_PATH_ANDROID_DATA, list) {
			if (unlikely(!strcmp(cursor->info.target_pathname, path.dentry->d_name.name))) {
				spin_lock(&susfs_spin_lock);
				cursor->info.target_ino = info.target_ino;
				strncpy(cursor->info.target_pathname, path.dentry->d_name.name, SUSFS_MAX_LEN_PATHNAME - 1);
				strncpy(cursor->target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
				cursor->info.i_uid = info.i_uid;
				cursor->path_len = strlen(cursor->info.target_pathname);
				SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s', i_uid: '%u', is successfully updated to LH_SUS_PATH_ANDROID_DATA\n",
				           cursor->info.target_ino, cursor->target_pathname, cursor->info.i_uid);
				spin_unlock(&susfs_spin_lock);
				goto out_kfree_tmp_buf;
			}
		}
		new_list = kmalloc(sizeof(struct st_susfs_sus_path_list), GFP_KERNEL);
		if (!new_list) {
			err = -ENOMEM;
			goto out_kfree_tmp_buf;
		}
		new_list->info.target_ino = info.target_ino;
		strncpy(new_list->info.target_pathname, path.dentry->d_name.name, SUSFS_MAX_LEN_PATHNAME - 1);
		strncpy(new_list->target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
		new_list->info.i_uid = info.i_uid;
		new_list->path_len = strlen(new_list->info.target_pathname);
		INIT_LIST_HEAD(&new_list->list);
		spin_lock(&susfs_spin_lock);
		list_add_tail(&new_list->list, &LH_SUS_PATH_ANDROID_DATA);
		SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s', i_uid: '%u', is successfully added to LH_SUS_PATH_ANDROID_DATA\n",
		           new_list->info.target_ino, new_list->target_pathname, new_list->info.i_uid);
		spin_unlock(&susfs_spin_lock);
		goto out_kfree_tmp_buf;
	} else if (strstr(resolved_pathname, sdcard_path.pathname)) {
		if (!sdcard_path.is_inited) {
			err = -EINVAL;
			SUSFS_LOGE("sdcard_path not configured, run set_sdcard_root_path after unlock\n");
			goto out_kfree_tmp_buf;
		}
		list_for_each_entry_safe(cursor, temp, &LH_SUS_PATH_SDCARD, list) {
			if (unlikely(!strcmp(cursor->info.target_pathname, path.dentry->d_name.name))) {
				spin_lock(&susfs_spin_lock);
				cursor->info.target_ino = info.target_ino;
				strncpy(cursor->info.target_pathname, path.dentry->d_name.name, SUSFS_MAX_LEN_PATHNAME - 1);
				strncpy(cursor->target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
				cursor->info.i_uid = info.i_uid;
				cursor->path_len = strlen(cursor->info.target_pathname);
				SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s', i_uid: '%u', is successfully updated to LH_SUS_PATH_SDCARD\n",
				           cursor->info.target_ino, cursor->target_pathname, cursor->info.i_uid);
				spin_unlock(&susfs_spin_lock);
				goto out_kfree_tmp_buf;
			}
		}
		new_list = kmalloc(sizeof(struct st_susfs_sus_path_list), GFP_KERNEL);
		if (!new_list) {
			err = -ENOMEM;
			goto out_kfree_tmp_buf;
		}
		new_list->info.target_ino = info.target_ino;
		strncpy(new_list->info.target_pathname, path.dentry->d_name.name, SUSFS_MAX_LEN_PATHNAME - 1);
		strncpy(new_list->target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
		new_list->info.i_uid = info.i_uid;
		new_list->path_len = strlen(new_list->info.target_pathname);
		INIT_LIST_HEAD(&new_list->list);
		spin_lock(&susfs_spin_lock);
		list_add_tail(&new_list->list, &LH_SUS_PATH_SDCARD);
		SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s', i_uid: '%u', is successfully added to LH_SUS_PATH_SDCARD\n",
		           new_list->info.target_ino, new_list->target_pathname, new_list->info.i_uid);
		spin_unlock(&susfs_spin_lock);
		goto out_kfree_tmp_buf;
	}

	spin_lock(&inode->i_lock);
	set_bit(AS_FLAGS_SUS_PATH, &inode->i_mapping->flags);
	SUSFS_LOGI("pathname: '%s', ino: '%lu', is flagged as AS_FLAGS_SUS_PATH\n",
	           resolved_pathname, info.target_ino);
	spin_unlock(&inode->i_lock);
out_kfree_tmp_buf:
	kfree(tmp_buf);
out_path_put_path:
	path_put(&path);
	return err;
}

int susfs_add_sus_path_loop(struct st_susfs_sus_path* __user user_info) {
	struct st_susfs_sus_path_list *cursor = NULL, *temp = NULL;
	struct st_susfs_sus_path_list *new_list = NULL;
	struct st_susfs_sus_path info;
	struct path path;
	struct inode *inode = NULL;
	char *resolved_pathname = NULL, *tmp_buf = NULL;
	int err = 0;

	err = copy_from_user(&info, user_info, sizeof(info));
	if (err) {
		SUSFS_LOGE("failed copying from userspace\n");
		return err;
	}

	err = kern_path(info.target_pathname, 0, &path);
	if (err) {
		SUSFS_LOGE("failed opening file '%s'\n",
		           info.target_pathname);
		return err;
	}

	if (!path.dentry->d_inode) {
		err = -EINVAL;
		goto out_path_put_path;
	}
	inode = d_inode(path.dentry);

	tmp_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!tmp_buf) {
		err = -ENOMEM;
		goto out_path_put_path;
	}

	resolved_pathname = d_path(&path, tmp_buf, PAGE_SIZE);
	SUSFS_LOGI("resolved_pathname: %s\n",
	           resolved_pathname);
	if (!resolved_pathname) {
		err = -ENOMEM;
		goto out_kfree_tmp_buf;
	}

	if (susfs_starts_with(resolved_pathname, "/storage/")) {
		err = -EINVAL;
		SUSFS_LOGE("path starting with /storage and /sdcard cannot be added by add_sus_path_loop\n");
		goto out_kfree_tmp_buf;
	}

	list_for_each_entry_safe(cursor, temp, &LH_SUS_PATH_LOOP, list) {
		if (unlikely(!strcmp(cursor->info.target_pathname, resolved_pathname))) {
			spin_lock(&susfs_spin_lock);
			cursor->info.target_ino = info.target_ino;
			strncpy(cursor->info.target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
			strncpy(cursor->target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
			cursor->info.i_uid = info.i_uid;
			cursor->path_len = strlen(cursor->info.target_pathname);
			SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s', i_uid: '%u', is successfully updated to LH_SUS_PATH_LOOP\n",
			           cursor->info.target_ino, cursor->target_pathname, cursor->info.i_uid);
			spin_unlock(&susfs_spin_lock);
			goto out_set_sus_path;
		}
	}
	new_list = kmalloc(sizeof(struct st_susfs_sus_path_list), GFP_KERNEL);
	if (!new_list) {
		err = -ENOMEM;
		goto out_kfree_tmp_buf;
	}
	new_list->info.target_ino = info.target_ino;
	strncpy(new_list->info.target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
	strncpy(new_list->target_pathname, resolved_pathname, SUSFS_MAX_LEN_PATHNAME - 1);
	new_list->info.i_uid = info.i_uid;
	new_list->path_len = strlen(new_list->info.target_pathname);
	INIT_LIST_HEAD(&new_list->list);
	spin_lock(&susfs_spin_lock);
	list_add_tail(&new_list->list, &LH_SUS_PATH_LOOP);
	SUSFS_LOGI("target_ino: '%lu', target_pathname: '%s', i_uid: '%u', is successfully added to LH_SUS_PATH_LOOP\n",
	           new_list->info.target_ino, new_list->target_pathname, new_list->info.i_uid);
	spin_unlock(&susfs_spin_lock);
out_set_sus_path:
	spin_lock(&inode->i_lock);
	set_bit(AS_FLAGS_SUS_PATH, &inode->i_mapping->flags);
	SUSFS_LOGI("pathname: '%s', ino: '%lu', is flagged as AS_FLAGS_SUS_PATH\n",
	           resolved_pathname, info.target_ino);
	spin_unlock(&inode->i_lock);
out_kfree_tmp_buf:
	kfree(tmp_buf);
out_path_put_path:
	path_put(&path);
	return err;
}

void susfs_run_sus_path_loop(uid_t uid) {
	struct st_susfs_sus_path_list *cursor = NULL, *temp = NULL;
	struct path path;
	struct inode *inode;

	list_for_each_entry_safe(cursor, temp, &LH_SUS_PATH_LOOP, list) {
		if (!kern_path(cursor->target_pathname, 0, &path)) {
			inode = path.dentry->d_inode;
			spin_lock(&inode->i_lock);
			set_bit(AS_FLAGS_SUS_PATH, &inode->i_mapping->flags);
			spin_unlock(&inode->i_lock);
			path_put(&path);
			SUSFS_LOGI("re-flag '%s' as SUS_PATH for uid: %u\n",
			           cursor->target_pathname, uid);
		}
	}
}

static inline bool is_i_uid_in_android_data_not_allowed(uid_t i_uid) {
	return (likely(susfs_is_current_non_root_user_app_proc()) &&
		unlikely(current_uid().val != i_uid));
}

static inline bool is_i_uid_in_sdcard_not_allowed(void) {
	return (likely(susfs_is_current_non_root_user_app_proc()));
}

static inline bool is_i_uid_not_allowed(uid_t i_uid) {
	return (likely(susfs_is_current_non_root_user_app_proc()) &&
		unlikely(current_uid().val != i_uid));
}

bool susfs_is_base_dentry_android_data_dir(struct dentry* base) {
	return (base && !IS_ERR(base) && base->d_inode &&
	        (base->d_inode->i_mapping->flags & BIT_ANDROID_DATA_ROOT_DIR));
}

bool susfs_is_base_dentry_sdcard_dir(struct dentry* base) {
	return (base && !IS_ERR(base) && base->d_inode &&
	        (base->d_inode->i_mapping->flags & BIT_ANDROID_SDCARD_ROOT_DIR));
}

bool susfs_is_sus_android_data_d_name_found(const char *d_name) {
	struct st_susfs_sus_path_list *cursor = NULL, *temp = NULL;

	if (d_name[0] == '\0') {
		return false;
	}

	list_for_each_entry_safe(cursor, temp, &LH_SUS_PATH_ANDROID_DATA, list) {
		if (!strncmp(d_name, cursor->info.target_pathname, cursor->path_len) &&
		    (d_name[cursor->path_len] == '\0' || d_name[cursor->path_len] == '/') &&
		     is_i_uid_in_android_data_not_allowed(cursor->info.i_uid))
		{
			SUSFS_LOGI("hiding path '%s'\n", cursor->target_pathname);
			return true;
		}
	}
	return false;
}

bool susfs_is_sus_sdcard_d_name_found(const char *d_name) {
	struct st_susfs_sus_path_list *cursor = NULL, *temp = NULL;

	if (d_name[0] == '\0') {
		return false;
	}
	list_for_each_entry_safe(cursor, temp, &LH_SUS_PATH_SDCARD, list) {
		if (!strncmp(d_name, cursor->info.target_pathname, cursor->path_len) &&
		    (d_name[cursor->path_len] == '\0' || d_name[cursor->path_len] == '/') &&
		     is_i_uid_in_sdcard_not_allowed())
		{
			SUSFS_LOGI("hiding path '%s'\n", cursor->target_pathname);
			return true;
		}
	}
	return false;
}

bool susfs_is_inode_sus_path(struct inode *inode) {
	if (unlikely(inode->i_mapping->flags & BIT_SUS_PATH &&
	    is_i_uid_not_allowed(i_uid_into_mnt(i_user_ns(inode), inode).val)))
	{
		SUSFS_LOGI("hiding path with ino '%lu'\n", inode->i_ino);
		return true;
	}
	return false;
}
#endif

/* sus_mount */
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
static LIST_HEAD(LH_SUS_MOUNT);
static void susfs_update_sus_mount_inode(char *target_pathname) {
	struct mount *mnt = NULL;
	struct path p;
	struct inode *inode = NULL;
	int err = 0;

	err = kern_path(target_pathname, 0, &p);
	if (err) {
		SUSFS_LOGE("failed opening file '%s'\n", target_pathname);
		return;
	}

	/* Check if the mount has a legitimate peer group id, if so we cannot add it to sus_mount */
	mnt = real_mount(p.mnt);
	if (mnt->mnt_group_id > 0 &&
		mnt->mnt_group_id < DEFAULT_SUS_MNT_GROUP_ID) {
		SUSFS_LOGE("skip setting SUS_MOUNT inode state for path '%s'\n", target_pathname);
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
		mnt->mnt_group_id < DEFAULT_SUS_MNT_GROUP_ID) {
		SUSFS_LOGE("skip setting SUS_MOUNT inode state for path '%s'\n", pathname);
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
		 !strncmp(pathname, "/system", 7) ||
		 !strncmp(pathname, "/system_ext", 11) ||
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

	list_for_each_entry_reverse(cursor, &LH_TRY_UMOUNT_PATH, list) {
		if (cursor->info.mnt_mode == TRY_UMOUNT_DEFAULT) {
			ksu_try_umount(cursor->info.target_pathname, false, 0, target_uid);
		} else if (cursor->info.mnt_mode == TRY_UMOUNT_DETACH) {
			ksu_try_umount(cursor->info.target_pathname, false, MNT_DETACH, target_uid);
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
#ifdef CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT
	bool is_magic_mount_path = false;
#endif

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

#ifdef CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT
	if (strstr(dpath, MAGIC_MOUNT_WORKDIR)) {
		is_magic_mount_path = true;
	}
#endif

	list_for_each_entry_safe(cursor, temp, &LH_TRY_UMOUNT_PATH, list) {
#ifdef CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT
		if (is_magic_mount_path && strstr(dpath, cursor->info.target_pathname)) {
			goto out_free_pathname;
		}
#endif
		if (unlikely(!strcmp(dpath, cursor->info.target_pathname))) {
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

#ifdef CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT
	if (is_magic_mount_path) {
		strncpy(new_list->info.target_pathname, dpath + strlen(MAGIC_MOUNT_WORKDIR), SUSFS_MAX_LEN_PATHNAME-1);
		goto out_add_to_list;
	}
#endif
	strncpy(new_list->info.target_pathname, dpath, SUSFS_MAX_LEN_PATHNAME-1);

#ifdef CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT
out_add_to_list:
#endif

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

static int copy_config_to_buf(const char *config_string, char *buf_ptr,
                              size_t *copied_size, size_t bufsize) {
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
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	err = copy_config_to_buf("CONFIG_KSU_SUSFS_SUS_PATH\n", buf_ptr, &copied_size, bufsize);
	if (err) goto out_kfree_kbuf;
	buf_ptr = kbuf + copied_size;
#endif
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
#ifdef CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT
	err = copy_config_to_buf("CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT\n", buf_ptr, &copied_size, bufsize);
	if (err) goto out_kfree_kbuf;
	buf_ptr = kbuf + copied_size;
#endif
	err = copy_to_user((void __user*)buf, (void *)kbuf, bufsize);
out_kfree_kbuf:
	kfree(kbuf);
	return err;
}

/* susfs_init */
void susfs_init(void) {
	spin_lock_init(&susfs_spin_lock);
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
	spin_lock_init(&susfs_uname_spin_lock);
	susfs_my_uname_init();
#endif
	SUSFS_LOGI("susfs is initialized! version: " SUSFS_VERSION " \n");
}

/*
 * No exit is needed becuase SUSFS should never be compiled as a module
 * void __init susfs_exit(void)
 */

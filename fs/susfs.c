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

extern bool susfs_is_current_ksu_domain(void);

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
static DEFINE_SPINLOCK(susfs_spin_lock_sus_mount);
bool susfs_hide_sus_mnts_for_all_procs = true;

void susfs_set_hide_sus_mnts_for_all_procs(void __user **user_info) {
	struct st_susfs_hide_sus_mnts_for_all_procs info = {0};

	if (copy_from_user(&info, (struct st_susfs_hide_sus_mnts_for_all_procs __user*)*user_info,
	                           sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}
	spin_lock(&susfs_spin_lock_sus_mount);
	susfs_hide_sus_mnts_for_all_procs = info.enabled;
	spin_unlock(&susfs_spin_lock_sus_mount);
	SUSFS_LOGI("susfs_hide_sus_mnts_for_all_procs: %d\n", info.enabled);
	info.err = 0;
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_hide_sus_mnts_for_all_procs __user*)*user_info)->err,
	                    &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_HIDE_SUS_MNTS_FOR_ALL_PROCS -> ret: %d\n", info.err);
}
#endif

/* enable_log */
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
static DEFINE_SPINLOCK(susfs_spin_lock_enable_log);

void susfs_enable_log(void __user **user_info) {
	struct st_susfs_log info = {0};

	if (copy_from_user(&info, (struct st_susfs_log __user*)*user_info,
	                           sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	spin_lock(&susfs_spin_lock_enable_log);
	susfs_is_log_enabled = info.enabled;
	spin_unlock(&susfs_spin_lock_enable_log);
	if (susfs_is_log_enabled) {
		pr_info("susfs: enable logging to kernel");
	} else {
		pr_info("susfs: disable logging to kernel");
	}
	info.err = 0;
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_log __user*)*user_info)->err,
	                    &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_ENABLE_LOG -> ret: %d\n", info.err);
}
#endif

/* spoof_cmdline_or_bootconfig */
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
static DEFINE_SPINLOCK(susfs_spin_lock_set_cmdline_or_bootconfig);
static char *fake_cmdline_or_bootconfig = NULL;
static bool susfs_is_fake_cmdline_or_bootconfig_set = false;

void susfs_set_cmdline_or_bootconfig(void __user **user_info) {
	struct st_susfs_spoof_cmdline_or_bootconfig *info =
		(struct st_susfs_spoof_cmdline_or_bootconfig *)
		kzalloc(sizeof(struct st_susfs_spoof_cmdline_or_bootconfig),
		        GFP_KERNEL);

	if (!info) {
		info->err = -ENOMEM;
		goto out_copy_to_user;
	}

	if (copy_from_user(info, (struct st_susfs_spoof_cmdline_or_bootconfig __user*)*user_info,
	                          sizeof(struct st_susfs_spoof_cmdline_or_bootconfig))) {
		info->err = -EFAULT;
		goto out_copy_to_user;
	}

	if (!fake_cmdline_or_bootconfig) {
		fake_cmdline_or_bootconfig = (char *)kzalloc(SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE,
		                                             GFP_KERNEL);
		if (!fake_cmdline_or_bootconfig) {
			info->err = -ENOMEM;
			goto out_copy_to_user;
		}
	}

	spin_lock(&susfs_spin_lock_set_cmdline_or_bootconfig);
	strncpy(fake_cmdline_or_bootconfig,
	        info->fake_cmdline_or_bootconfig,
	        SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE-1);
	spin_unlock(&susfs_spin_lock_set_cmdline_or_bootconfig);
	susfs_is_fake_cmdline_or_bootconfig_set = true;
	SUSFS_LOGI("fake_cmdline_or_bootconfig is set\n");
	info->err = 0;
out_copy_to_user:
	if (info->err) {
		susfs_is_fake_cmdline_or_bootconfig_set = false;
	}
	if (copy_to_user(&((struct st_susfs_spoof_cmdline_or_bootconfig __user*)*user_info)->err,
	                    &info->err, sizeof(info->err))) {
		info->err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG -> ret: %d\n", info->err);
	if (info) {
		kfree(info);
	}
}

int susfs_spoof_cmdline_or_bootconfig(struct seq_file *m) {
	if (susfs_is_fake_cmdline_or_bootconfig_set && fake_cmdline_or_bootconfig) {
		seq_puts(m, fake_cmdline_or_bootconfig);
		return 0;
	}
	return 1;
}
#endif

/* susfs avc log spoofing */
static DEFINE_SPINLOCK(susfs_spin_lock_set_avc_log_spoofing);
extern bool susfs_is_avc_log_spoofing_enabled;

void susfs_set_avc_log_spoofing(void __user **user_info) {
	struct st_susfs_avc_log_spoofing info = {0};

	if (copy_from_user(&info, (struct st_susfs_avc_log_spoofing __user*)*user_info,
	                           sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	spin_lock(&susfs_spin_lock_set_avc_log_spoofing);
	susfs_is_avc_log_spoofing_enabled = info.enabled;
	spin_unlock(&susfs_spin_lock_set_avc_log_spoofing);
	SUSFS_LOGI("susfs_is_avc_log_spoofing_enabled: %d\n", info.enabled);
	info.err = 0;
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_avc_log_spoofing __user*)*user_info)->err,
	                    &info.err, sizeof(info.err))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_ENABLE_AVC_LOG_SPOOFING -> ret: %d\n", info.err);
}

/* get susfs enabled features */
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

void susfs_get_enabled_features(void __user **user_info) {
	struct st_susfs_enabled_features *info =
	        (struct st_susfs_enabled_features *)
	        kzalloc(sizeof(struct st_susfs_enabled_features),
	                GFP_KERNEL);
	char *buf_ptr = NULL;
	size_t copied_size = 0;

	if (!info) {
		info->err = -ENOMEM;
		goto out_copy_to_user;
	}

	if (copy_from_user(info, (struct st_susfs_enabled_features __user*)*user_info,
	                          sizeof(struct st_susfs_enabled_features))) {
		info->err = -EFAULT;
		goto out_copy_to_user;
	}

	buf_ptr = info->enabled_features;

#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_SUS_MOUNT\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_ENABLE_LOG\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
	info->err = copy_config_to_buf("CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG\n", buf_ptr, &copied_size, SUSFS_ENABLED_FEATURES_SIZE);
	if (info->err) goto out_copy_to_user;
	buf_ptr = info->enabled_features + copied_size;
#endif

	info->err = 0;
out_copy_to_user:
	if (copy_to_user((struct st_susfs_enabled_features __user*)*user_info,
	                  info, sizeof(struct st_susfs_enabled_features))) {
		info->err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_SHOW_ENABLED_FEATURES -> ret: %d\n", info->err);
	if (info) {
		kfree(info);
	}
}

/* show_variant */
void susfs_show_variant(void __user **user_info) {
	struct st_susfs_variant info = {0};

	if (copy_from_user(&info, (struct st_susfs_variant __user*)*user_info,
	                           sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	strncpy(info.susfs_variant, SUSFS_VARIANT, SUSFS_MAX_VARIANT_BUFSIZE-1);
	info.err = 0;
out_copy_to_user:
	if (copy_to_user((struct st_susfs_variant __user*)*user_info, &info,
	                  sizeof(info))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_SHOW_VARIANT -> ret: %d\n", info.err);
}

/* show version */
void susfs_show_version(void __user **user_info) {
	struct st_susfs_version info = {0};

	if (copy_from_user(&info, (struct st_susfs_version __user*)*user_info,
	                           sizeof(info))) {
		info.err = -EFAULT;
		goto out_copy_to_user;
	}

	strncpy(info.susfs_version, SUSFS_VERSION, SUSFS_MAX_VERSION_BUFSIZE-1);
	info.err = 0;
out_copy_to_user:
	if (copy_to_user((struct st_susfs_version __user*)*user_info,
	                  &info, sizeof(info))) {
		info.err = -EFAULT;
	}
	SUSFS_LOGI("CMD_SUSFS_SHOW_VERSION -> ret: %d\n", info.err);
}

/* susfs_init */
void susfs_init(void) {
	SUSFS_LOGI("susfs is initialized! version: " SUSFS_VERSION " \n");
}

/*
 * No exit is needed becuase SUSFS should never be compiled as a module
 * void __init susfs_exit(void)
 */

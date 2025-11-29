#ifndef KSU_SUSFS_H
#define KSU_SUSFS_H

#include <linux/version.h>
#include <linux/types.h>
#include <linux/utsname.h>
#include <linux/hashtable.h>
#include <linux/path.h>
#include <linux/susfs_def.h>

#define SUSFS_VERSION "v2.0.0"
#define SUSFS_VARIANT "GKI"

/* MACRO */

#define getname_safe(name) (name == NULL ? ERR_PTR(-EINVAL) : getname(name))
#define putname_safe(name) (IS_ERR(name) ? NULL : putname(name))

/* STRUCT */

/* sus_mount */
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
struct st_susfs_hide_sus_mnts_for_all_procs {
	bool                                    enabled;
	int                                     err;
};
#endif

/* enable_log */
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
struct st_susfs_log {
	bool                                    enabled;
	int                                     err;
};
#endif

/* spoof_cmdline_or_bootconfig */
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
struct st_susfs_spoof_cmdline_or_bootconfig {
	char                                    fake_cmdline_or_bootconfig[SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE];
	int                                     err;
};
#endif

/* avc log spoofing */
struct st_susfs_avc_log_spoofing {
	bool                                    enabled;
	int                                     err;
};

/* get enabled features */
struct st_susfs_enabled_features {
	char                                    enabled_features[SUSFS_ENABLED_FEATURES_SIZE];
	int                                     err;
};

/* show variant */
struct st_susfs_variant {
	char                                    susfs_variant[16];
	int                                     err;
};

/* show version */
struct st_susfs_version {
	char                                    susfs_version[16];
	int                                     err;
};

/* FORWARD DECLARATION */

/* sus_mount */
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
void susfs_set_hide_sus_mnts_for_all_procs(void __user **user_info);
#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT

/* enable_log */
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
void susfs_enable_log(void __user **user_info);
#endif

/* spoof_cmdline_or_bootconfig */
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
void susfs_set_cmdline_or_bootconfig(void __user **user_info);
int susfs_spoof_cmdline_or_bootconfig(struct seq_file *m);
#endif

void susfs_set_avc_log_spoofing(void __user **user_info);

void susfs_get_enabled_features(void __user **user_info);
void susfs_show_variant(void __user **user_info);
void susfs_show_version(void __user **user_info);

/* susfs_init */
void susfs_init(void);

#endif

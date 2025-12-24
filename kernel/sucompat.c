#include <linux/compiler_types.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/uaccess.h>
#include <asm/current.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/sched/task_stack.h>
#include <linux/ptrace.h>

#ifdef CONFIG_KSU_SUSFS
#include <linux/namei.h>
#include <linux/susfs_def.h>
#include "kernel_compat.h"
#include "objsec.h"
#include "selinux/selinux.h"
#endif
#include "allowlist.h"
#include "feature.h"
#include "klog.h" // IWYU pragma: keep
#include "ksud.h"
#include "sucompat.h"
#include "app_profile.h"
#include "util.h"

#ifdef CONFIG_KSU_SUSFS
extern void write_sulog(uint8_t sym);
#endif

#define SU_PATH "/system/bin/su"
#define SH_PATH "/system/bin/sh"

bool ksu_su_compat_enabled __read_mostly = true;

static int su_compat_feature_get(u64 *value)
{
    *value = ksu_su_compat_enabled ? 1 : 0;
    return 0;
}

static int su_compat_feature_set(u64 value)
{
    bool enable = value != 0;

#ifdef CONFIG_KSU_SUSFS
    if (enable == ksu_su_compat_enabled) {
        pr_info("su_compat: no need to change\n");
        return 0;
    }

    if (enable) {
        ksu_sucompat_enable();
    } else {
        ksu_sucompat_disable();
    }
#endif

    ksu_su_compat_enabled = enable;
    pr_info("su_compat: set to %d\n", enable);
    return 0;
}

static const struct ksu_feature_handler su_compat_handler = {
    .feature_id = KSU_FEATURE_SU_COMPAT,
    .name = "su_compat",
    .get_handler = su_compat_feature_get,
    .set_handler = su_compat_feature_set,
};

static void __user *userspace_stack_buffer(const void *d, size_t len)
{
    // To avoid having to mmap a page in userspace, just write below the stack
    // pointer.
    char __user *p = (void __user *)current_user_stack_pointer() - len;

    return copy_to_user(p, d, len) ? NULL : p;
}

static char __user *sh_user_path(void)
{
    static const char sh_path[] = "/system/bin/sh";

    return userspace_stack_buffer(sh_path, sizeof(sh_path));
}

static char __user *ksud_user_path(void)
{
    static const char ksud_path[] = KSUD_PATH;

    return userspace_stack_buffer(ksud_path, sizeof(ksud_path));
}

#ifndef CONFIG_KSU_SUSFS
int ksu_handle_faccessat(int *dfd, const char __user **filename_user, int *mode,
                         int *__unused_flags)
{
    const char su[] = SU_PATH;

    if (!ksu_is_allow_uid_for_current(current_uid().val)) {
        return 0;
    }

    char path[sizeof(su) + 1];
    memset(path, 0, sizeof(path));
    strncpy_from_user_nofault(path, *filename_user, sizeof(path));

    if (unlikely(!memcmp(path, su, sizeof(su)))) {
        pr_info("faccessat su->sh!\n");
        *filename_user = sh_user_path();
    }

    return 0;
}

int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags)
{
    // const char sh[] = SH_PATH;
    const char su[] = SU_PATH;

    if (!ksu_is_allow_uid_for_current(current_uid().val)) {
        return 0;
    }

    if (unlikely(!filename_user)) {
        return 0;
    }

    char path[sizeof(su) + 1];
    memset(path, 0, sizeof(path));
    strncpy_from_user_nofault(path, *filename_user, sizeof(path));

    if (unlikely(!memcmp(path, su, sizeof(su)))) {
        pr_info("newfstatat su->sh!\n");
        *filename_user = sh_user_path();
    }

    return 0;
}

int ksu_handle_execve_sucompat(const char __user **filename_user,
                               void *__never_use_argv, void *__never_use_envp,
                               int *__never_use_flags)
{
    const char su[] = SU_PATH;
    const char __user *fn;
    char path[sizeof(su) + 1];
    long ret;
    unsigned long addr;

    if (unlikely(!filename_user))
        return 0;

    if (!ksu_is_allow_uid_for_current(current_uid().val))
        return 0;

    addr = untagged_addr((unsigned long)*filename_user);
    fn = (const char __user *)addr;
    memset(path, 0, sizeof(path));
    ret = strncpy_from_user_nofault(path, fn, sizeof(path));

    if (ret < 0 && try_set_access_flag(addr)) {
        ret = strncpy_from_user_nofault(path, fn, sizeof(path));
    }

    if (ret < 0 && preempt_count()) {
        /* This is crazy, but we know what we are doing:
         * Temporarily exit atomic context to handle page faults, then restore it */
        pr_info("Access filename failed, try rescue..\n");
        preempt_enable_no_resched_notrace();
        ret = strncpy_from_user(path, fn, sizeof(path));
        preempt_disable_notrace();
    }

    if (ret < 0) {
        pr_warn("Access filename when execve failed: %ld", ret);
        return 0;
    }

    if (likely(memcmp(path, su, sizeof(su))))
        return 0;

    pr_info("sys_execve su found\n");
    *filename_user = ksud_user_path();

    escape_with_root_profile();

    return 0;
}
#else
static bool ksu_sucompat_enabled __read_mostly = true;

static const char sh_path[] = SH_PATH;
static const char su_path[] = SU_PATH;
static const char ksud_path[] = KSUD_PATH;

extern bool ksu_kernel_umount_enabled;

__attribute__((hot, no_stack_protector))
static __always_inline bool is_su_allowed(const void **ptr_to_check)
{
    barrier();
    if (!ksu_sucompat_enabled)
        return false;

    if (likely(!ksu_is_allow_uid_for_current(current_uid().val)))
        return false;

    // first check the pointer-to-pointer
    if (unlikely(!(volatile void *)ptr_to_check))
        return false;

    // now dereference to check actual pointer
    if (unlikely(!(volatile void *)*ptr_to_check))
        return false;

    return true;
}

static int ksu_sucompat_user_common(const char __user **filename_user,
                                    const char *syscall_name,
                                    const bool escalate,
                                    const uint8_t log_symbol)
{
    const char su[] = SU_PATH;
    char path[sizeof(su)];
    
    if (ksu_copy_from_user_retry(path, *filename_user, sizeof(path)))
        return 0;

    path[sizeof(path) - 1] = '\0';

    if (memcmp(path, su, sizeof(su)))
        return 0;

    write_sulog(log_symbol);

    if (escalate) {
        pr_info("%s su found\n", syscall_name);
        *filename_user = ksud_user_path();
        escape_with_root_profile();
    } else {
        pr_info("%s su->sh!\n", syscall_name);
        *filename_user = sh_user_path();
    }

    return 0;
}

static int ksu_handle_init_domain_ksud(const char __user *filename_user)
{
    if (current->pid == 1 || !is_init(get_current_cred()))
        return 0;
    
    char path[sizeof(ksud_path)];
    
    if (ksu_copy_from_user_retry(path, filename_user, sizeof(path)))
        return 0;
    
    path[sizeof(path) - 1] = '\0';
    
    if (unlikely(strcmp(path, ksud_path) == 0)) {
        pr_info("sys_execve: escape to root for init executing ksud: %d\n",
                current->pid);
        escape_to_root_for_init();
        return 1;
    }
    
    return 0;
}

// execve_handler_pre does not pass correct values for the __never_use_* arguments
// these parameters are kept only for consistency with manually patched code
int ksu_handle_execve_sucompat(int *fd, const char __user **filename_user,
                               void *__never_use_argv, void *__never_use_envp,
                               int *__never_use_flags)
{
    ksu_handle_init_domain_ksud(*filename_user);

    if (!is_su_allowed((const void **)filename_user))
        return 0;
    return ksu_sucompat_user_common(filename_user, "sys_execve", true, 'x');
}

int ksu_handle_faccessat(int *dfd, const char __user **filename_user, int *mode,
                         int *__unused_flags)
{
    if (!is_su_allowed((const void **)filename_user))
        return 0;
    return ksu_sucompat_user_common(filename_user, "faccessat", false, 'a');
}

int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags)
{
    if (!is_su_allowed((const void **)filename_user))
        return 0;
    return ksu_sucompat_user_common(filename_user, "newfstatat", false, 's');
}

int ksu_handle_devpts(struct inode *inode)
{
        barrier();
        if (!ksu_sucompat_enabled) {
                return 0;
        }

        if (susfs_is_current_proc_umounted()) {
                return 0;
        }
        
        if (!current->mm) {
                return 0;
        }

        uid_t uid = current_uid().val;
        if (uid % 100000 < 10000) {
                return 0;
        }

        if (!__ksu_is_allow_uid_for_current(uid))
                return 0;

        if (ksu_file_sid) {
                struct inode_security_struct *sec = selinux_inode(inode);
                if (sec) {
                        sec->sid = ksu_file_sid;
                }
        }

        return 0;
}

void ksu_sucompat_enable()
{
	ksu_sucompat_enabled = true;
	pr_info("%s: hooks enabled: exec, faccessat, stat, devpts\n", __func__);
}

void ksu_sucompat_disable()
{
	ksu_sucompat_enabled = false;
	pr_info("%s: hooks disabled: exec, faccessat, stat, devpts\n", __func__);
}
#endif

// sucompat: permitted process can execute 'su' to gain root access.
void ksu_sucompat_init()
{
    if (ksu_register_feature_handler(&su_compat_handler)) {
        pr_err("Failed to register su_compat feature handler\n");
    }
#ifdef CONFIG_KSU_SUSFS
    if (ksu_su_compat_enabled) {
        ksu_sucompat_enable();
    }
#endif
}

void ksu_sucompat_exit()
{
#ifdef CONFIG_KSU_SUSFS
    if (ksu_su_compat_enabled) {
        ksu_sucompat_disable();
    }
#endif
    ksu_unregister_feature_handler(KSU_FEATURE_SU_COMPAT);
}

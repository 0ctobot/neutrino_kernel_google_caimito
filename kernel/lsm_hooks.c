// SPDX-License-Identifier: GPL-2.0
/*
 * KernelSU LSM security hooks
 * Provides LSM hook registration for kernel-level security interception
 */

#include <linux/binfmts.h>
#include <linux/init.h>
#include <linux/lsm_hooks.h>
#include <linux/version.h>

#include "klog.h" // IWYU pragma: keep
#include "ksud.h"

static int ksu_bprm_creds_for_exec(struct linux_binprm *bprm)
{
    if (likely(!ksu_execveat_hook))
        return 0;

    ksu_handle_pre_ksud((char *)bprm->filename);
    return 0;
}

static struct security_hook_list ksu_hooks[] = {
    LSM_HOOK_INIT(bprm_creds_for_exec, ksu_bprm_creds_for_exec),
};

void __init ksu_lsm_hooks_init(void)
{
    security_add_hooks(ksu_hooks, ARRAY_SIZE(ksu_hooks), "ksu");
    pr_info("KSU LSM hooks registered\n");
}

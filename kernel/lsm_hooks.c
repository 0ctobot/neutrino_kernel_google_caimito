// SPDX-License-Identifier: GPL-2.0
/*
 * KernelSU LSM security hooks
 * Provides LSM hook registration for kernel-level security interception
 */

#include <linux/init.h>
#include <linux/lsm_hooks.h>
#include <linux/version.h>

#include "klog.h" // IWYU pragma: keep

static struct security_hook_list ksu_hooks[] = {
};

void __init ksu_lsm_hooks_init(void)
{
    // Only register if we have hooks defined
    if (ARRAY_SIZE(ksu_hooks) == 0) {
        pr_info("KSU LSM hooks: infrastructure ready (no hooks registered yet)\n");
        return;
    }
    security_add_hooks(ksu_hooks, ARRAY_SIZE(ksu_hooks), "ksu");
    pr_info("KSU LSM hooks registered\n");
}

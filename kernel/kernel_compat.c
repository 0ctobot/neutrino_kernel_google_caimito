// SPDX-License-Identifier: GPL-2.0
/*
 * KernelSU kernel compatibility helpers
 * Provides functions for userspace data access
 */

#include <linux/uaccess.h>
#include <linux/version.h>

#include "kernel_compat.h"

long ksu_copy_from_user_nofault(void *dst, const void __user *src, size_t size)
{
    return copy_from_user_nofault(dst, src, size);
}

long ksu_copy_from_user_retry(void *to, const void __user *from, unsigned long count)
{
    // Try nofault copy first (faster, no scheduling)
    long ret = ksu_copy_from_user_nofault(to, from, count);
    if (likely(!ret))
        return ret;

    // Nofault failed, fallback to regular copy which can sleep
    return copy_from_user(to, from, count);
}

#ifndef __KSU_H_KERNEL_COMPAT
#define __KSU_H_KERNEL_COMPAT

#include <linux/types.h>
#include <linux/uaccess.h>

/**
 * ksu_copy_from_user_nofault - Copy data from userspace with fault handling
 *
 * Returns 0 on success, -EFAULT on error
 */
extern long ksu_copy_from_user_nofault(void *dst, const void __user *src, size_t size);

/**
 * ksu_copy_from_user_retry - Copy with nofault first, fallback to regular
 *
 * Attempts nofault copy first for performance, falls back to regular copy_from_user
 * Returns 0 on success, non-zero on error
 */
extern long ksu_copy_from_user_retry(void *to, const void __user *from, unsigned long count);

#endif

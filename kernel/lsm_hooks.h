#ifndef __KSU_H_LSM_HOOKS
#define __KSU_H_LSM_HOOKS

#include <linux/init.h>

/**
 * ksu_lsm_hooks_init - Initialize KernelSU LSM security hooks
 * 
 * Registers LSM hooks with the kernel security framework.
 * Must be called during kernel initialization.
 */
void __init ksu_lsm_hooks_init(void);

#endif

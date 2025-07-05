// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Adam W. Willis <return.of.octobot@gmail.com>.
 */

#include <linux/init.h>
#include <linux/thermal.h>

/* Register cooling device stubs to appease pixel-thermal HAL */
static struct thermal_cooling_device *cdev[3];

static int get_max_state_compat(struct thermal_cooling_device *cdev,
                                unsigned long *state)
{
	*state = 1;
	return 0;
}

static int get_cur_state_compat(struct thermal_cooling_device *cdev,
                                unsigned long *state)
{
	*state = 0;
	return 0;
}

static int set_cur_state_compat(struct thermal_cooling_device *cdev,
                                unsigned long state)
{
	return 0;
}

static const struct thermal_cooling_device_ops compat_ops = {
	.get_max_state = get_max_state_compat,
	.get_cur_state = get_cur_state_compat,
	.set_cur_state = set_cur_state_compat,
};

static int __init uclamp_shim_init(void)
{
	int i;
	char name[32];
	int cpus[] = {0, 4, 7};

	for (i = 0; i < 3; i++) {
		snprintf(name, sizeof(name), "thermal-uclamp-%d", cpus[i]);
		cdev[i] = thermal_cooling_device_register(name, NULL, &compat_ops);
		if (IS_ERR(cdev[i])) {
			pr_err("Failed to register cooling device stub %s: %ld\n",
			       name, PTR_ERR(cdev[i]));
			cdev[i] = NULL;
		} else {
			pr_info("Registered cooling device stub: %s\n", name);
		}
	}

	return 0;
}
late_initcall(uclamp_shim_init);

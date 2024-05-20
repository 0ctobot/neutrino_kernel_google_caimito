/* SPDX-License-Identifier: MIT */

#include "gs_panel/gs_panel_test.h"

/**
 * struct tk4c_panel_test - panel specific test runtime info
 *
 * Only maintains tk4c panel test specific runtime info, any fixed details about panel
 * should most likely go into struct gs_panel_test_desc
 */
struct tk4c_panel_test {
	/** @base: base panel struct */
	struct gs_panel_test base;

	/* add panel specific test data here */
};
#define to_spanel_test(test) container_of(test, struct tk4c_panel_test, base)

static void tk4c_test_debugfs_init(struct gs_panel_test *test, struct dentry *test_root)
{
	/* Add custom debugfs entry */
}

static const struct gs_panel_test_funcs tk4c_test_func = {
	.debugfs_init = tk4c_test_debugfs_init,
};

static const struct gs_panel_test_desc google_tk4c_test = {
	.test_funcs = &tk4c_test_func,
};

static int tk4c_panel_test_probe(struct platform_device *pdev)
{
	struct tk4c_panel_test *stest;

	stest = devm_kzalloc(&pdev->dev, sizeof(*stest), GFP_KERNEL);
	if (!stest)
		return -ENOMEM;

	return gs_panel_test_common_init(pdev, &stest->base);
}

static const struct of_device_id gs_panel_test_of_match[] = {
	{ .compatible = "google,gs-tk4c-test", .data = &google_tk4c_test },
	{ }
};
MODULE_DEVICE_TABLE(of, gs_panel_test_of_match);

static struct platform_driver gs_panel_test_driver = {
	.probe = tk4c_panel_test_probe,
	.remove = gs_panel_test_common_remove,
	.driver = {
		.name = "gs-tk4c-test",
		.of_match_table = gs_panel_test_of_match,
	},
};
module_platform_driver(gs_panel_test_driver);

MODULE_AUTHOR("Safayat Ullah <safayat@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google tk4c panel test driver");
MODULE_LICENSE("Dual MIT/GPL");

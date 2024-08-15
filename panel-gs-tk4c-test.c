/* SPDX-License-Identifier: MIT */

#include "gs_panel/gs_panel_test.h"

static const u8 test_key_enable[] = { 0xF0, 0x5A, 0x5A };
static const u8 test_key_disable[] = { 0xF0, 0xA5, 0xA5 };

const struct gs_dsi_cmd irc_read_cmds[] = {
	GS_DSI_CMD(0xB0, 0x00, 0x03, 0x68),
};
static DEFINE_GS_CMDSET(irc_read);

const struct gs_dsi_cmd fgz_read_cmds[] = {
	GS_DSI_CMD(0xB0, 0x00, 0x61, 0x68),
};
static DEFINE_GS_CMDSET(fgz_read);

const struct gs_dsi_cmd opr_read_cmds[] = {
	GS_DSI_CMD(0xB0, 0x00, 0xD8, 0x63),
};
static DEFINE_GS_CMDSET(opr_read);

const struct gs_dsi_cmd test_key_enable_cmds[] = {
	GS_DSI_CMDLIST(test_key_enable),
};
static DEFINE_GS_CMDSET(test_key_enable);

const struct gs_dsi_cmd test_key_disable_cmds[] = {
	GS_DSI_CMDLIST(test_key_disable),
};
static DEFINE_GS_CMDSET(test_key_disable);

/* sorted by register address */
static const struct gs_panel_register tk4c_registers[] = {
	GS_PANEL_REG("lp_status", 0x54),
	GS_PANEL_REG_LONG_WITH_CMDS("opr", 0x63, 2, &opr_read_cmdset),
	GS_PANEL_REG_WITH_CMDS("irc", 0x68, &irc_read_cmdset),
	GS_PANEL_REG_LONG_WITH_CMDS("fgz", 0x68, 8, &fgz_read_cmdset),
	GS_PANEL_REG_LONG("refresh_rate", 0x84, 2),
};

static const struct gs_panel_registers_desc tk4c_reg_desc = {
	.register_count = ARRAY_SIZE(tk4c_registers),
	.registers = tk4c_registers,

	.global_pre_read_cmdset = &test_key_enable_cmdset,
	.global_post_read_cmdset = &test_key_disable_cmdset,
};

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
	.regs_desc = &tk4c_reg_desc,
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

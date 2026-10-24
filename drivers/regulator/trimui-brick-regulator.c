// SPDX-License-Identifier: GPL-2.0-only
/* Boot-established TrimUI Brick rails; no voltage selector writes. */
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#define ENABLE 0x91
#define CLDO1_VSEL 0x9b
#define CLDO4_VSEL 0x9e

static int voltage(struct regulator_dev *rdev)
{
	unsigned int value;
	int ret = regmap_read(rdev_get_regmap(rdev), rdev->desc->vsel_reg, &value);

	if (ret) return ret;
	return 500000 + (value & 0x1f) * 100000;
}
static int cldo4_enable(struct regulator_dev *rdev)
{
	int ret = voltage(rdev);

	/* Never turn on an unexpectedly configured rail. No selector writes. */
	if (ret < 0) return ret;
	if (ret != 3300000) return -ERANGE;
	return regulator_enable_regmap(rdev);
}
static int retain_shared(struct device_node *node,
	const struct regulator_desc *desc, struct regulator_config *config)
{
	/* Preserve shared boot-enabled rails regardless of consumer refcounts. */
	return of_property_read_bool(node, "regulator-always-on") ? 0 : -EINVAL;
}
static const struct regulator_ops shared_ops = {
	.is_enabled = regulator_is_enabled_regmap, .get_voltage = voltage,
};
static const struct regulator_ops panel_ops = {
	.enable = cldo4_enable, .disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap, .get_voltage = voltage,
};
static const struct regulator_desc supplies[] = {
	{
		.name = "brick-cldo1", .of_match = "cldo1", .regulators_node = "regulators",
		.id = 0, .type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
		.ops = &shared_ops, .of_parse_cb = retain_shared,
		.enable_reg = ENABLE, .enable_mask = BIT(0), .vsel_reg = CLDO1_VSEL,
	}, {
		.name = "brick-cldo4", .of_match = "cldo4", .regulators_node = "regulators",
		.id = 1, .type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
		.ops = &panel_ops,
		.enable_reg = ENABLE, .enable_mask = BIT(3), .vsel_reg = CLDO4_VSEL,
	},
	{
		.name = "brick-aldo3", .of_match = "aldo3",
		.regulators_node = "regulators",
		.id = 2, .type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
		.ops = &shared_ops, .of_parse_cb = retain_shared,
		.enable_reg = 0x90, .enable_mask = BIT(2), .vsel_reg = 0x95,
	},
	{
		.name = "brick-aldo4", .of_match = "aldo4",
		.regulators_node = "regulators",
		.id = 3, .type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
		.ops = &shared_ops, .of_parse_cb = retain_shared,
		.enable_reg = 0x90, .enable_mask = BIT(3), .vsel_reg = 0x96,
	},
	{
		.name = "brick-bldo1", .of_match = "bldo1",
		.regulators_node = "regulators",
		.id = 4, .type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
		.ops = &shared_ops, .of_parse_cb = retain_shared,
		.enable_reg = 0x90, .enable_mask = BIT(4), .vsel_reg = 0x97,
	},
};
static int brick_probe(struct platform_device *pdev)
{
	struct regulator_config config = {
		.dev = pdev->dev.parent,
		.regmap = dev_get_regmap(pdev->dev.parent, NULL),
	};
	struct regulator_dev *rdev;
	int i;

	if (!config.regmap)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(supplies); i++) {
		rdev = devm_regulator_register(&pdev->dev, &supplies[i], &config);
		if (IS_ERR(rdev))
			return PTR_ERR(rdev);
	}
	return 0;
}

static struct platform_driver brick_driver = {
	.probe = brick_probe,
	.driver.name = "trimui-brick-regulator",
};
module_platform_driver(brick_driver);
MODULE_ALIAS("platform:trimui-brick-regulator");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TrimUI Brick boot-established regulators");

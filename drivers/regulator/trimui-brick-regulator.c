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
		.name = "brick-cldo3", .of_match = "cldo3", .regulators_node = "regulators",
		.id = 5, .type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
		.n_voltages = 1, .fixed_uV = 3300000,
		.ops = &shared_ops, .of_parse_cb = retain_shared,
		.enable_reg = ENABLE, .enable_mask = BIT(2), .vsel_reg = 0x9d,
	},
	{
		.name = "brick-cldo1", .of_match = "cldo1", .regulators_node = "regulators",
		.id = 0, .type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
		.n_voltages = 1, .fixed_uV = 1800000,
		.ops = &shared_ops, .of_parse_cb = retain_shared,
		.enable_reg = ENABLE, .enable_mask = BIT(0), .vsel_reg = CLDO1_VSEL,
	}, {
		.name = "brick-cldo4", .of_match = "cldo4", .regulators_node = "regulators",
		.id = 1, .type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
		.n_voltages = 1, .fixed_uV = 3300000,
		.ops = &panel_ops,
		.enable_reg = ENABLE, .enable_mask = BIT(3), .vsel_reg = CLDO4_VSEL,
	},
	{
		.name = "brick-aldo3", .of_match = "aldo3",
		.regulators_node = "regulators",
		.id = 2, .type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
		.n_voltages = 1, .fixed_uV = 3300000,
		.ops = &shared_ops, .of_parse_cb = retain_shared,
		.enable_reg = 0x90, .enable_mask = BIT(2), .vsel_reg = 0x95,
	},
	{
		.name = "brick-aldo4", .of_match = "aldo4",
		.regulators_node = "regulators",
		.id = 3, .type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
		.n_voltages = 1, .fixed_uV = 1800000,
		.ops = &shared_ops, .of_parse_cb = retain_shared,
		.enable_reg = 0x90, .enable_mask = BIT(3), .vsel_reg = 0x96,
	},
	{
		.name = "brick-bldo1", .of_match = "bldo1",
		.regulators_node = "regulators",
		.id = 4, .type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
		.n_voltages = 1, .fixed_uV = 3300000,
		.ops = &shared_ops, .of_parse_cb = retain_shared,
		.enable_reg = 0x90, .enable_mask = BIT(4), .vsel_reg = 0x97,
	},
};
/* Only the nonzero-selector implementation has independent boost/switch bits.
 * Let the regulator core own the parent vote and unwind a failed switch enable.
 */
static int brick_vmid_enable(struct regulator_dev *rdev)
{
	unsigned int value;
	int ret;

	ret = regmap_read(rdev_get_regmap(rdev), 0x00, &value);
	if (ret)
		return ret;
	if (value & BIT(5))
		return -EBUSY; /* Do not source into an already powered connector. */

	return regulator_enable_regmap(rdev);
}

static const struct regulator_ops brick_vmid_ops = {
	.enable = brick_vmid_enable,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

static const struct regulator_ops brick_vbus_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

static const struct regulator_desc brick_vbus_supplies[] = {
	{
		.name = "brick-vmid", .of_match = "vmid",
		.regulators_node = "regulators",
		.id = 6, .type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
		.ops = &brick_vmid_ops,
		.enable_reg = 0x19, .enable_mask = BIT(4),
		.enable_time = 1000,
	}, {
		.name = "brick-drivevbus", .of_match = "drivevbus",
		.regulators_node = "regulators", .supply_name = "vin",
		.id = 7, .type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
		.ops = &brick_vbus_ops,
		.enable_reg = 0x13, .enable_mask = BIT(0),
		.enable_time = 1000,
	},
};

static int brick_register_vbus(struct platform_device *pdev,
			       struct regulator_config *config)
{
	struct device_node *regs, *vmid, *vbus, *parent;
	struct regulator_dev *rdev;
	unsigned int variant, value;
	int ret = 0, i;

	regs = of_get_child_by_name(config->dev->of_node, "regulators");
	vmid = of_get_child_by_name(regs, "vmid");
	vbus = of_get_child_by_name(regs, "drivevbus");
	if (!vmid && !vbus)
		goto put_nodes;
	if (!of_device_is_available(vmid) || !of_device_is_available(vbus)) {
		ret = -EINVAL;
		goto put_nodes;
	}
	parent = of_parse_phandle(vbus, "vin-supply", 0);
	if (parent != vmid)
		ret = -EINVAL;
	of_node_put(parent);
	if (ret)
		goto put_nodes;

	ret = regmap_read(config->regmap, 0x0f, &variant);
	if (ret)
		goto put_nodes;
	if (!variant) {
		ret = dev_err_probe(&pdev->dev, -EOPNOTSUPP,
				    "VBUS parent semantics unavailable for selector zero\n");
		goto put_nodes;
	}
	/* Do not adopt an unowned source inherited from another role owner. */
	ret = regmap_read(config->regmap, 0x13, &value);
	if (ret)
		goto put_nodes;
	if (value & BIT(0)) {
		ret = -EBUSY;
		goto put_nodes;
	}
	ret = regmap_read(config->regmap, 0x19, &value);
	if (ret)
		goto put_nodes;
	if (value & BIT(4)) {
		ret = -EBUSY;
		goto put_nodes;
	}
	for (i = 0; i < ARRAY_SIZE(brick_vbus_supplies); i++) {
		rdev = devm_regulator_register(&pdev->dev,
					       &brick_vbus_supplies[i], config);
		if (IS_ERR(rdev)) {
			ret = PTR_ERR(rdev);
			break;
		}
	}
put_nodes:
	of_node_put(vbus);
	of_node_put(vmid);
	of_node_put(regs);
	return ret;
}

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
	return brick_register_vbus(pdev, &config);
}

static struct platform_driver brick_driver = {
	.probe = brick_probe,
	.driver.name = "trimui-brick-regulator",
};
module_platform_driver(brick_driver);
MODULE_ALIAS("platform:trimui-brick-regulator");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TrimUI Brick boot-established regulators");

// SPDX-License-Identifier: GPL-2.0-only
/* Narrow stock-verified panel supplies; no MFD, charger, IRQ or voltage writes. */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#define ENABLE 0x91
#define CLDO1_VSEL 0x9b
#define CLDO4_VSEL 0x9e

static bool readable(struct device *dev, unsigned int reg)
{
	return reg == ENABLE || reg == CLDO1_VSEL || reg == CLDO4_VSEL;
}
static bool writeable(struct device *dev, unsigned int reg)
{
	return reg == ENABLE;
}
static const struct regmap_config map_config = {
	.reg_bits = 8, .val_bits = 8, .max_register = CLDO4_VSEL,
	.readable_reg = readable, .writeable_reg = writeable,
	.cache_type = REGCACHE_NONE,
};
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
	/* SD/codec ownership is not represented by this panel supply driver.
	 * Preserve the boot-enabled shared rail, regardless of panel refcount.
	 */
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
};
static int brick_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config config = { .dev = dev };
	struct regulator_dev *rdev;
	struct device_node *regs, *child;
	unsigned int value;
	int ret, i;

	if (client->addr != 0x34) return -EINVAL;
	regs = of_get_child_by_name(dev->of_node, "regulators");
	if (!regs) return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(supplies); i++) {
		child = of_get_child_by_name(regs, supplies[i].of_match);
		if (!child || !of_device_is_available(child) ||
		    (i == 0 && !of_property_read_bool(child, "regulator-always-on"))) {
			of_node_put(child); of_node_put(regs); return -EINVAL;
		}
		of_node_put(child);
	}
	of_node_put(regs);
	config.regmap = devm_regmap_init_i2c(client, &map_config);
	if (IS_ERR(config.regmap)) return PTR_ERR(config.regmap);
	/* Validate ALL prerequisites before registering either consumer supply. */
	ret = regmap_read(config.regmap, CLDO1_VSEL, &value);
	if (ret) return ret;
	if ((value & 0x1f) != 13) return -ERANGE;
	ret = regmap_read(config.regmap, CLDO4_VSEL, &value);
	if (ret) return ret;
	if ((value & 0x1f) != 28) return -ERANGE;
	ret = regmap_read(config.regmap, ENABLE, &value);
	if (ret) return ret;
	if (!(value & BIT(0))) return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(supplies); i++) {
		rdev = devm_regulator_register(dev, &supplies[i], &config);
		if (IS_ERR(rdev)) return PTR_ERR(rdev);
	}
	return 0;
}
static const struct of_device_id brick_match[] = {
	{ .compatible = "trimui,brick-panel-supplies" }, { }
};
MODULE_DEVICE_TABLE(of, brick_match);
static struct i2c_driver brick_driver = {
	.probe = brick_probe,
	.driver = { .name = "brick-panel-power", .of_match_table = brick_match },
};
module_i2c_driver(brick_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Stock-verified TrimUI Brick display supplies only");

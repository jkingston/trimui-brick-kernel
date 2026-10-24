// SPDX-License-Identifier: GPL-2.0-only
/* Boot-established AXP2202 interface for TrimUI Brick. */
#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#define AXP2202_STATUS		0x00
#define AXP2202_MODULE_EN	0x19
#define AXP2202_LDO_EN1		0x91
#define AXP2202_CLDO1_VSEL	0x9b
#define AXP2202_CLDO4_VSEL	0x9e
#define AXP2202_GAUGE_CONFIG	0xa2
#define AXP2202_GAUGE_SOC		0xa4
#define AXP2202_VBAT_H		0xc4
#define AXP2202_VBAT_L		0xc5

static bool brick_readable(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case AXP2202_STATUS:
	case AXP2202_MODULE_EN:
	case AXP2202_LDO_EN1:
	case AXP2202_CLDO1_VSEL:
	case AXP2202_CLDO4_VSEL:
	case AXP2202_GAUGE_CONFIG:
	case AXP2202_GAUGE_SOC:
	case AXP2202_VBAT_H:
	case AXP2202_VBAT_L:
		return true;
	default:
		return false;
	}
}

static bool brick_writeable(struct device *dev, unsigned int reg)
{
	return reg == AXP2202_LDO_EN1;
}

static const struct regmap_config brick_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AXP2202_VBAT_L,
	.readable_reg = brick_readable,
	.writeable_reg = brick_writeable,
	.cache_type = REGCACHE_NONE,
};

static const struct mfd_cell brick_cells[] = {
	{ .name = "trimui-brick-panel" },
	{ .name = "axp2202-telemetry" },
};

static int brick_pmic_probe(struct i2c_client *client)
{
	struct regmap *map;
	struct device_node *regs, *child;
	unsigned int value;
	int ret;

	if (client->addr != 0x34)
		return -EINVAL;
	regs = of_get_child_by_name(client->dev.of_node, "regulators");
	if (!regs)
		return -EINVAL;
	child = of_get_child_by_name(regs, "cldo1");
	ret = child && of_device_is_available(child) &&
	      of_property_read_bool(child, "regulator-always-on") ? 0 : -EINVAL;
	of_node_put(child);
	child = of_get_child_by_name(regs, "cldo4");
	if (!child || !of_device_is_available(child))
		ret = -EINVAL;
	of_node_put(child);
	of_node_put(regs);
	if (ret)
		return ret;

	map = devm_regmap_init_i2c(client, &brick_regmap);
	if (IS_ERR(map))
		return PTR_ERR(map);

	/* Preserve the proven display prerequisites before exposing consumers. */
	ret = regmap_read(map, AXP2202_CLDO1_VSEL, &value);
	if (ret)
		return ret;
	if ((value & 0x1f) != 13)
		return -ERANGE;
	ret = regmap_read(map, AXP2202_CLDO4_VSEL, &value);
	if (ret)
		return ret;
	if ((value & 0x1f) != 28)
		return -ERANGE;
	ret = regmap_read(map, AXP2202_LDO_EN1, &value);
	if (ret)
		return ret;
	if (!(value & BIT(0)))
		return -ENODEV;

	return devm_mfd_add_devices(&client->dev, PLATFORM_DEVID_AUTO,
				   brick_cells, ARRAY_SIZE(brick_cells),
				   NULL, 0, NULL);
}

static const struct of_device_id brick_pmic_match[] = {
	{ .compatible = "trimui,brick-pmic" },
	{ }
};
MODULE_DEVICE_TABLE(of, brick_pmic_match);

static struct i2c_driver brick_pmic_driver = {
	.probe = brick_pmic_probe,
	.driver = {
		.name = "trimui-brick-pmic",
		.of_match_table = brick_pmic_match,
	},
};
module_i2c_driver(brick_pmic_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TrimUI Brick boot-established PMIC");

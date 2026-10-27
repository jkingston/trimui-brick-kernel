// SPDX-License-Identifier: GPL-2.0-only
/* Boot-established AXP2202 interface for TrimUI Brick. */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/mfd/trimui-brick-pmic.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/reboot.h>
#include <linux/slab.h>

#define AXP2202_SOFT_PWROFF	0x27

#define AXP2202_STATUS		0x00
#define AXP2202_CHARGE_STATUS	0x01
#define AXP2202_CLOCK_ENABLE	0x0b
#define AXP2202_MODULE_EN	0x19
#define AXP2202_IRQ_ENABLE	0x40
#define AXP2202_IRQ_STATUS	0x48
#define AXP2202_KEY_PRESS	13
#define AXP2202_KEY_RELEASE	14
#define AXP2202_LDO_EN1		0x91
#define AXP2202_CLDO1_VSEL	0x9b
#define AXP2202_CLDO3_VSEL	0x9d
#define AXP2202_CLDO4_VSEL	0x9e
#define AXP2202_GAUGE_CONFIG	0xa2
#define AXP2202_GAUGE_SOC		0xa4
#define AXP2202_VBAT_H		0xc4
#define AXP2202_VBAT_L		0xc5

static bool brick_readable(struct device *dev, unsigned int reg)
{
	if ((reg >= AXP2202_IRQ_ENABLE && reg < AXP2202_IRQ_ENABLE + 5) ||
	    (reg >= AXP2202_IRQ_STATUS && reg < AXP2202_IRQ_STATUS + 5))
		return true;
	switch (reg) {
	case AXP2202_SOFT_PWROFF:
	case AXP2202_CLOCK_ENABLE:
	case 0x0f: /* VBUS implementation selector */
	case 0x13: /* VBUS output switch */
	case 0x17: /* USB input current limit */
	case 0x62: /* Constant charge current */
	case AXP2202_STATUS:
	case AXP2202_CHARGE_STATUS:
	case AXP2202_MODULE_EN:
	case 0x90:
	case 0x95:
	case 0x96:
	case 0x97:
	case AXP2202_LDO_EN1:
	case AXP2202_CLDO1_VSEL:
	case AXP2202_CLDO3_VSEL:
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
	return reg == AXP2202_SOFT_PWROFF ||
	       reg == AXP2202_CLOCK_ENABLE ||
	       reg == 0x17 || reg == 0x62 || reg == 0x13 || reg == AXP2202_MODULE_EN ||
	       reg == AXP2202_LDO_EN1 ||
	       (reg >= AXP2202_IRQ_ENABLE && reg < AXP2202_IRQ_ENABLE + 5) ||
	       (reg >= AXP2202_IRQ_STATUS && reg < AXP2202_IRQ_STATUS + 5);
}

static const struct regmap_config brick_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AXP2202_VBAT_L,
	.readable_reg = brick_readable,
	.writeable_reg = brick_writeable,
	.cache_type = REGCACHE_NONE,
};

/* All documented sources are masked until a child requests its IRQ. */
static const struct regmap_irq brick_irqs[] = {
	{ .reg_offset = 0, .mask = BIT(7) },
	{ .reg_offset = 0, .mask = BIT(6) },
	{ .reg_offset = 0, .mask = BIT(5) },
	{ .reg_offset = 0, .mask = BIT(4) },
	{ .reg_offset = 0, .mask = BIT(2) },
	{ .reg_offset = 0, .mask = BIT(1) },
	{ .reg_offset = 0, .mask = BIT(0) },
	{ .reg_offset = 1, .mask = BIT(7) },
	{ .reg_offset = 1, .mask = BIT(6) },
	{ .reg_offset = 1, .mask = BIT(5) },
	{ .reg_offset = 1, .mask = BIT(4) },
	{ .reg_offset = 1, .mask = BIT(3) },
	{ .reg_offset = 1, .mask = BIT(2) },
	{ .reg_offset = 1, .mask = BIT(1) },
	{ .reg_offset = 1, .mask = BIT(0) },
	{ .reg_offset = 2, .mask = BIT(7) },
	{ .reg_offset = 2, .mask = BIT(6) },
	{ .reg_offset = 2, .mask = BIT(5) },
	{ .reg_offset = 2, .mask = BIT(4) },
	{ .reg_offset = 2, .mask = BIT(3) },
	{ .reg_offset = 2, .mask = BIT(2) },
	{ .reg_offset = 2, .mask = BIT(1) },
	{ .reg_offset = 2, .mask = BIT(0) },
	{ .reg_offset = 3, .mask = BIT(7) },
	{ .reg_offset = 3, .mask = BIT(6) },
	{ .reg_offset = 3, .mask = BIT(5) },
	{ .reg_offset = 3, .mask = BIT(4) },
	{ .reg_offset = 3, .mask = BIT(3) },
	{ .reg_offset = 3, .mask = BIT(2) },
	{ .reg_offset = 3, .mask = BIT(1) },
	{ .reg_offset = 3, .mask = BIT(0) },
	{ .reg_offset = 4, .mask = BIT(6) },
	{ .reg_offset = 4, .mask = BIT(5) },
	{ .reg_offset = 4, .mask = BIT(4) },
	{ .reg_offset = 4, .mask = BIT(3) },
	{ .reg_offset = 4, .mask = BIT(2) },
	{ .reg_offset = 4, .mask = BIT(1) },
	{ .reg_offset = 4, .mask = BIT(0) },
};

static const struct regmap_irq_chip brick_irq_chip = {
	.name = "axp2202",
	.status_base = AXP2202_IRQ_STATUS,
	.ack_base = AXP2202_IRQ_STATUS,
	.unmask_base = AXP2202_IRQ_ENABLE,
	.init_ack_masked = true,
	.irqs = brick_irqs,
	.num_irqs = ARRAY_SIZE(brick_irqs),
	.num_regs = 5,
};

static const struct resource brick_key_resources[] = {
	DEFINE_RES_IRQ_NAMED(AXP2202_KEY_PRESS, "press"),
	DEFINE_RES_IRQ_NAMED(AXP2202_KEY_RELEASE, "release"),
};

enum brick_cell_index {
	BRICK_CELL_REGULATOR,
	BRICK_CELL_TELEMETRY,
	BRICK_CELL_PEK,
};

static const struct mfd_cell brick_cells[] = {
	[BRICK_CELL_REGULATOR] = { .name = "trimui-brick-regulator" },
	[BRICK_CELL_TELEMETRY] = { .name = "axp2202-telemetry" },
	[BRICK_CELL_PEK] = {
		.name = "axp2202-pek",
		.resources = brick_key_resources,
		.num_resources = ARRAY_SIZE(brick_key_resources),
	},
};

static const struct {
	const char *name;
	unsigned int vsel, selector, enable, mask;
} brick_shared_rails[] = {
	{ "cldo1", AXP2202_CLDO1_VSEL, 13, AXP2202_LDO_EN1, BIT(0) },
	{ "cldo3", AXP2202_CLDO3_VSEL, 28, AXP2202_LDO_EN1, BIT(2) },
	{ "aldo3", 0x95, 28, 0x90, BIT(2) },
	{ "aldo4", 0x96, 13, 0x90, BIT(3) },
	{ "bldo1", 0x97, 28, 0x90, BIT(4) },
};

/* I2C may sleep: run after device shutdown but before syscore shutdown. */
static int brick_pmic_power_off_prepare(struct sys_off_data *data)
{
	struct regmap *map = data->cb_data;
	int ret;

	/* Allwinner AXP2202 PMU code preserves the other OFF_CTL bits. */
	ret = regmap_write_bits(map, AXP2202_SOFT_PWROFF, BIT(0), BIT(0));
	if (ret) {
		dev_err(data->dev, "power-off request failed: %d\n", ret);
		return NOTIFY_DONE;
	}

	/* Allow the request to take effect; this is not a completion test. */
	msleep(1000);
	dev_err(data->dev, "power-off request returned; trying firmware fallback\n");
	return NOTIFY_DONE;
}

static int brick_pmic_probe(struct i2c_client *client)
{
	struct trimui_brick_pmic_pek_pdata *pek_pdata;
	struct regmap_irq_chip_data *irq_data;
	struct mfd_cell *cells;
	struct regmap *map;
	struct device_node *regs, *child;
	unsigned int value;
	int ret;

	if (client->addr != 0x34)
		return -EINVAL;
	regs = of_get_child_by_name(client->dev.of_node, "regulators");
	if (!regs)
		return -EINVAL;
	ret = 0;
	for (int i = 0; i < ARRAY_SIZE(brick_shared_rails); i++) {
		child = of_get_child_by_name(regs, brick_shared_rails[i].name);
		if (!child || !of_device_is_available(child) ||
		    !of_property_read_bool(child, "regulator-always-on"))
			ret = -EINVAL;
		of_node_put(child);
	}
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

	/* Validate shared rail voltage and enable state before any child probes. */
	for (int i = 0; i < ARRAY_SIZE(brick_shared_rails); i++) {
		ret = regmap_read(map, brick_shared_rails[i].vsel, &value);
		if (ret)
			return ret;
		if ((value & 0x1f) != brick_shared_rails[i].selector)
			return -ERANGE;
		ret = regmap_read(map, brick_shared_rails[i].enable, &value);
		if (ret)
			return ret;
		if (!(value & brick_shared_rails[i].mask))
			return -ENODEV;
	}
	ret = regmap_read(map, AXP2202_CLDO4_VSEL, &value);
	if (ret)
		return ret;
	if ((value & 0x1f) != 28)
		return -ERANGE;

	if (client->irq <= 0)
		return -EINVAL;
	cells = devm_kmemdup(&client->dev, brick_cells, sizeof(brick_cells),
			     GFP_KERNEL);
	if (!cells)
		return -ENOMEM;
	pek_pdata = devm_kzalloc(&client->dev, sizeof(*pek_pdata), GFP_KERNEL);
	if (!pek_pdata)
		return -ENOMEM;
	pek_pdata->parent_irq = client->irq;
	cells[BRICK_CELL_PEK].platform_data = pek_pdata;
	cells[BRICK_CELL_PEK].pdata_size = sizeof(*pek_pdata);
	ret = devm_regmap_add_irq_chip(&client->dev, map, client->irq,
				       IRQF_ONESHOT, 0, &brick_irq_chip, &irq_data);
	if (ret)
		return dev_err_probe(&client->dev, ret, "Cannot register PMIC IRQs\n");

	if (of_property_read_bool(client->dev.of_node, "system-power-controller")) {
		/* The existing PSCI final handler also makes POWER_OFF available. */
		if (!kernel_can_power_off())
			return dev_err_probe(&client->dev, -ENODEV,
					     "requires firmware power-off fallback\n");
		ret = devm_register_sys_off_handler(&client->dev,
						    SYS_OFF_MODE_POWER_OFF_PREPARE,
						    SYS_OFF_PRIO_LOW,
						   brick_pmic_power_off_prepare, map);
		if (ret)
			return ret;
	}

	return devm_mfd_add_devices(&client->dev, PLATFORM_DEVID_AUTO,
				   cells, ARRAY_SIZE(brick_cells),
				   NULL, 0, regmap_irq_get_domain(irq_data));
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

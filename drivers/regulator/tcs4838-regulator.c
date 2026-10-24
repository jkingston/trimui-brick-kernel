// SPDX-License-Identifier: GPL-2.0-only
/* TCS4838 single buck, with the board holding SEL low during normal operation. */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

#define TCS4838_VSEL0		0x00
#define TCS4838_CTRL		0x02
#define TCS4838_ID1		0x03
#define TCS4838_ID2		0x04
#define TCS4838_PGOOD		0x05
#define TCS4838_SELECTOR	GENMASK(5, 0)
#define TCS4838_ENABLE		BIT(7)
#define TCS4838_SLEW		GENMASK(6, 4)

struct tcs4838 {
	struct regmap *regmap;
	bool registered;
	bool faulted;
};

static bool tcs4838_writeable(struct device *dev, unsigned int reg)
{
	return reg == TCS4838_VSEL0;
}

static const struct regmap_config tcs4838_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = TCS4838_PGOOD,
	.cache_type = REGCACHE_NONE,
	.writeable_reg = tcs4838_writeable,
};

static int tcs4838_set_voltage_sel(struct regulator_dev *rdev,
				 unsigned int selector)
{
	struct tcs4838 *tcs = rdev_get_drvdata(rdev);
	unsigned int before, after, ctrl, old_sel, slew, delay_us;
	int ret;

	if (selector >= 64)
		return -EINVAL;
	if (tcs->faulted)
		return -EIO;
	ret = regmap_read(tcs->regmap, TCS4838_VSEL0, &before);
	if (ret)
		return ret;
	if (!(before & TCS4838_ENABLE))
		return -EPERM;
	old_sel = FIELD_GET(TCS4838_SELECTOR, before);
	if (selector == old_sel)
		return 0;
	/* Registration must never normalize the inherited boot voltage. */
	if (!tcs->registered)
		return -EPERM;
	/*
	 * The slew setting applies only to increases. CPU consumers must lower
	 * their clock before lowering voltage, as the OPP framework does.
	 * No chip-specified falling delay is available; board settling-time
	 * constraints, if needed, are applied by the regulator core.
	 */
	delay_us = 0;
	if (selector > old_sel) {
		ret = regmap_read(tcs->regmap, TCS4838_CTRL, &ctrl);
		if (ret)
			return ret;
		slew = FIELD_GET(TCS4838_SLEW, ctrl);
		/* 12.5 mV/step, 10 mV per (150 ns << slew), rounded up to us. */
		delay_us = DIV_ROUND_UP(((selector - old_sel) * 1875U) << slew,
				      10000U);
	}
	ret = regmap_update_bits(tcs->regmap, TCS4838_VSEL0,
				TCS4838_SELECTOR, selector);
	if (ret)
		goto fault;
	fsleep(delay_us);
	ret = regmap_read(tcs->regmap, TCS4838_VSEL0, &after);
	if (ret)
		goto fault;
	if (after != ((before & ~TCS4838_SELECTOR) | selector)) {
		ret = -EIO;
		goto fault;
	}
	/* PGOOD validates enable/soft-start status, not analog settling. */
	ret = regmap_read(tcs->regmap, TCS4838_PGOOD, &after);
	if (ret)
		goto fault;
	if (!(after & BIT(7))) {
		ret = -EIO;
		goto fault;
	}
	return 0;

fault:
	/* An uncertain write must not be followed by another voltage change. */
	tcs->faulted = true;
	return ret;
}

static int tcs4838_get_voltage_sel(struct regulator_dev *rdev)
{
	struct tcs4838 *tcs = rdev_get_drvdata(rdev);

	if (tcs->faulted)
		return -EIO;
	return regulator_get_voltage_sel_regmap(rdev);
}

static const struct regulator_ops tcs4838_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = tcs4838_get_voltage_sel,
	.set_voltage_sel = tcs4838_set_voltage_sel,
	.is_enabled = regulator_is_enabled_regmap,
};

static const struct regulator_desc tcs4838_desc = {
	.name = "tcs4838",
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &tcs4838_ops,
	.n_voltages = 64,
	.min_uV = 712500,
	.uV_step = 12500,
	.vsel_reg = TCS4838_VSEL0,
	.vsel_mask = TCS4838_SELECTOR,
	.enable_reg = TCS4838_VSEL0,
	.enable_mask = TCS4838_ENABLE,
};

static int tcs4838_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config config = { .dev = dev, .of_node = dev->of_node };
	struct regulator_dev *rdev;
	struct tcs4838 *tcs;
	unsigned int id, rev, value;
	u32 bank;
	int ret;

	ret = device_property_read_u32(dev, "tcs,runtime-voltage-selector", &bank);
	if (ret || bank != 0 ||
	    !device_property_read_bool(dev, "regulator-always-on"))
		return dev_err_probe(dev, -EINVAL, "requires always-on runtime bank 0\n");
	tcs = devm_kzalloc(dev, sizeof(*tcs), GFP_KERNEL);
	if (!tcs)
		return -ENOMEM;
	tcs->regmap = devm_regmap_init_i2c(client, &tcs4838_regmap_config);
	if (IS_ERR(tcs->regmap))
		return PTR_ERR(tcs->regmap);
	ret = regmap_read(tcs->regmap, TCS4838_ID1, &id);
	if (ret)
		return ret;
	ret = regmap_read(tcs->regmap, TCS4838_ID2, &rev);
	if (ret)
		return ret;
	if (id != 0x88 || rev != 0x01)
		return dev_err_probe(dev, -ENODEV, "unsupported identity %02x:%02x\n", id, rev);
	ret = regmap_read(tcs->regmap, TCS4838_VSEL0, &value);
	if (ret)
		return ret;
	if (!(value & TCS4838_ENABLE))
		return dev_err_probe(dev, -EINVAL, "boot supply is disabled\n");
	ret = regmap_read(tcs->regmap, TCS4838_PGOOD, &value);
	if (ret)
		return ret;
	if (!(value & BIT(7)))
		return dev_err_probe(dev, -EINVAL, "boot supply is not power-good\n");
	config.regmap = tcs->regmap;
	config.driver_data = tcs;
	rdev = devm_regulator_register(dev, &tcs4838_desc, &config);
	if (IS_ERR(rdev))
		return PTR_ERR(rdev);
	tcs->registered = true;
	return 0;
}

static const struct of_device_id tcs4838_of_match[] = {
	{ .compatible = "tcs,tcs4838" },
	{ }
};
MODULE_DEVICE_TABLE(of, tcs4838_of_match);

static int tcs4838_suspend_prepare(struct device *dev)
{
	return -EOPNOTSUPP;
}

static const struct dev_pm_ops tcs4838_pm_ops = {
	.prepare = tcs4838_suspend_prepare,
};

static struct i2c_driver tcs4838_driver = {
	.driver = {
		.name = "tcs4838-regulator",
		.of_match_table = tcs4838_of_match,
		.pm = &tcs4838_pm_ops,
	},
	.probe = tcs4838_probe,
};
module_i2c_driver(tcs4838_driver);

MODULE_DESCRIPTION("TCS4838 regulator with fixed runtime bank zero");
MODULE_LICENSE("GPL");

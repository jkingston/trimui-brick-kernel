// SPDX-License-Identifier: GPL-2.0-only
/* Read-only telemetry; boot firmware owns charger and gauge configuration. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

#define AXP2202_STATUS		0x00
#define AXP2202_MODULE_EN	0x19
#define AXP2202_GAUGE_CONFIG	0xa2
#define AXP2202_GAUGE_SOC		0xa4
#define AXP2202_VBAT		0xc4

static int axp2202_voltage(struct regmap *map, int *voltage)
{
	u8 data[2];
	int values[3], ret, i;

	/* The vendor driver takes the median to reject ADC outliers. */
	for (i = 0; i < ARRAY_SIZE(values); i++) {
		ret = regmap_bulk_read(map, AXP2202_VBAT, data, sizeof(data));
		if (ret)
			return ret;
		values[i] = ((data[0] & 0x3f) << 8) | data[1];
	}
	if (values[0] > values[1])
		swap(values[0], values[1]);
	if (values[1] > values[2])
		swap(values[1], values[2]);
	if (values[0] > values[1])
		swap(values[0], values[1]);
	if (values[1] < 2000 || values[1] > 8000)
		return -ENODATA;
	*voltage = values[1] * 1000;
	return 0;
}

static int axp2202_battery_get(struct power_supply *psy,
			       enum power_supply_property property,
			       union power_supply_propval *val)
{
	struct regmap *map = power_supply_get_drvdata(psy);
	unsigned int value;
	int voltage, ret;

	switch (property) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return axp2202_voltage(map, &val->intval);
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = axp2202_voltage(map, &voltage);
		if (ret)
			return ret;
		ret = regmap_read(map, AXP2202_MODULE_EN, &value);
		if (ret)
			return ret;
		if (!(value & BIT(2)))
			return -ENODATA;
		ret = regmap_read(map, AXP2202_GAUGE_CONFIG, &value);
		if (ret)
			return ret;
		/* A programmed model is required; reject an open BROM window. */
		if (!(value & BIT(4)) || (value & BIT(0)))
			return -ENODATA;
		ret = regmap_read(map, AXP2202_GAUGE_SOC, &value);
		if (ret)
			return ret;
		if (value > 100)
			return -ENODATA;
		val->intval = value;
		return 0;
	default:
		return -EINVAL;
	}
}

static int axp2202_usb_get(struct power_supply *psy,
			   enum power_supply_property property,
			   union power_supply_propval *val)
{
	struct regmap *map = power_supply_get_drvdata(psy);
	unsigned int value;
	int ret;

	if (property != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;
	ret = regmap_read(map, AXP2202_STATUS, &value);
	if (ret)
		return ret;
	val->intval = !!(value & BIT(5));
	return 0;
}

static const enum power_supply_property battery_properties[] = {
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
};

static const enum power_supply_property usb_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc battery_desc = {
	.name = "axp2202-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = battery_properties,
	.num_properties = ARRAY_SIZE(battery_properties),
	.get_property = axp2202_battery_get,
};

static const struct power_supply_desc usb_desc = {
	.name = "axp2202-usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = usb_properties,
	.num_properties = ARRAY_SIZE(usb_properties),
	.get_property = axp2202_usb_get,
};

static int axp2202_telemetry_probe(struct platform_device *pdev)
{
	struct power_supply_config config = {
		.drv_data = dev_get_regmap(pdev->dev.parent, NULL),
	};
	struct power_supply *psy;

	if (!config.drv_data)
		return -ENODEV;
	psy = devm_power_supply_register(&pdev->dev, &battery_desc, &config);
	if (IS_ERR(psy))
		return PTR_ERR(psy);
	psy = devm_power_supply_register(&pdev->dev, &usb_desc, &config);
	return PTR_ERR_OR_ZERO(psy);
}

static struct platform_driver axp2202_telemetry_driver = {
	.probe = axp2202_telemetry_probe,
	.driver.name = "axp2202-telemetry",
};
module_platform_driver(axp2202_telemetry_driver);
MODULE_ALIAS("platform:axp2202-telemetry");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only AXP2202 battery and USB telemetry");

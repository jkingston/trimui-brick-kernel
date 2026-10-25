// SPDX-License-Identifier: GPL-2.0-only
/* Read-only telemetry; boot firmware owns charger and gauge configuration. */
#include <linux/devm-helpers.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

#define AXP2202_STATUS		0x00
#define AXP2202_CHARGE_STATUS	0x01
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

static int axp2202_battery_status(struct regmap *map, int *status)
{
	unsigned int value;
	int voltage, ret;

	ret = axp2202_voltage(map, &voltage);
	if (ret)
		return ret;
	ret = regmap_read(map, AXP2202_CHARGE_STATUS, &value);
	if (ret)
		return ret;

	/* Vendor charger-state encoding; unknown is preferable to false full. */
	switch (value & GENMASK(2, 0)) {
	case 1:
	case 2:
	case 3:
		*status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	default:
		/* Idle/current direction and spurious charge-done are ambiguous. */
		*status = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}
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
	case POWER_SUPPLY_PROP_STATUS:
		return axp2202_battery_status(map, &val->intval);
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
	POWER_SUPPLY_PROP_STATUS,
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

#define AXP2202_POLL_INTERVAL (30 * HZ)

struct axp2202_sample {
	bool valid;
	int error;
	int value;
};

struct axp2202_monitor {
	struct power_supply *battery;
	struct power_supply *usb;
	struct delayed_work work;
	struct axp2202_sample status;
	struct axp2202_sample capacity;
	struct axp2202_sample online;
};

static bool axp2202_sample_changed(struct power_supply *psy,
				   enum power_supply_property property,
				   struct axp2202_sample *sample)
{
	union power_supply_propval val = { 0 };
	bool changed;
	int ret;

	ret = power_supply_get_property(psy, property, &val);
	changed = sample->valid && (sample->error != ret ||
		  (!ret && sample->value != val.intval));
	sample->valid = true;
	sample->error = ret;
	if (!ret)
		sample->value = val.intval;

	return changed;
}

static void axp2202_poll(struct work_struct *work)
{
	struct axp2202_monitor *monitor = container_of(to_delayed_work(work),
						     struct axp2202_monitor, work);
	bool battery_changed, usb_changed;

	battery_changed = axp2202_sample_changed(monitor->battery,
						 POWER_SUPPLY_PROP_STATUS, &monitor->status);
	battery_changed |= axp2202_sample_changed(monitor->battery,
						  POWER_SUPPLY_PROP_CAPACITY, &monitor->capacity);
	usb_changed = axp2202_sample_changed(monitor->usb,
					     POWER_SUPPLY_PROP_ONLINE, &monitor->online);
	if (battery_changed || usb_changed)
		power_supply_changed(monitor->battery);
	if (usb_changed)
		power_supply_changed(monitor->usb);

	schedule_delayed_work(&monitor->work, AXP2202_POLL_INTERVAL);
}

static int axp2202_telemetry_suspend(struct device *dev)
{
	struct axp2202_monitor *monitor = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&monitor->work);
	return 0;
}

static int axp2202_telemetry_resume(struct device *dev)
{
	struct axp2202_monitor *monitor = dev_get_drvdata(dev);

	schedule_delayed_work(&monitor->work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(axp2202_telemetry_pm_ops,
			       axp2202_telemetry_suspend,
			       axp2202_telemetry_resume);

static int axp2202_telemetry_probe(struct platform_device *pdev)
{
	struct power_supply_config config = {
		.drv_data = dev_get_regmap(pdev->dev.parent, NULL),
	};
	struct axp2202_monitor *monitor;
	int ret;

	if (!config.drv_data)
		return -ENODEV;
	monitor = devm_kzalloc(&pdev->dev, sizeof(*monitor), GFP_KERNEL);
	if (!monitor)
		return -ENOMEM;
	platform_set_drvdata(pdev, monitor);
	monitor->battery = devm_power_supply_register(&pdev->dev, &battery_desc,
						      &config);
	if (IS_ERR(monitor->battery))
		return PTR_ERR(monitor->battery);
	monitor->usb = devm_power_supply_register(&pdev->dev, &usb_desc, &config);
	if (IS_ERR(monitor->usb))
		return PTR_ERR(monitor->usb);

	/* Cancel polling before either managed power supply is released. */
	ret = devm_delayed_work_autocancel(&pdev->dev, &monitor->work,
					   axp2202_poll);
	if (ret)
		return ret;
	schedule_delayed_work(&monitor->work, 0);
	return 0;
}

static void axp2202_telemetry_shutdown(struct platform_device *pdev)
{
	struct axp2202_monitor *monitor = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&monitor->work);
}

static struct platform_driver axp2202_telemetry_driver = {
	.probe = axp2202_telemetry_probe,
	.shutdown = axp2202_telemetry_shutdown,
	.driver = {
		.name = "axp2202-telemetry",
		.pm = pm_sleep_ptr(&axp2202_telemetry_pm_ops),
	},
};
module_platform_driver(axp2202_telemetry_driver);
MODULE_ALIAS("platform:axp2202-telemetry");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only AXP2202 battery and USB telemetry");

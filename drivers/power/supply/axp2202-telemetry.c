// SPDX-License-Identifier: GPL-2.0-only
/* Battery telemetry and explicit, board-bounded charger current controls. */
#include <linux/devm-helpers.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

#define AXP2202_INPUT_CURRENT	0x17
#define AXP2202_CHARGE_CURRENT	0x62
#define AXP2202_CURRENT_MASK	GENMASK(5, 0)
#define AXP2202_CHARGE_STEP_UA	64000
#define AXP2202_INPUT_STEP_UA	50000
#define AXP2202_INPUT_MIN_UA	100000

/*
 * Brick stock policy, not battery design data or the IC's physical maximum.
 * Stock's 1950 mA suspend limit is encoded as 1920 mA; adapter input is 2500 mA.
 * Boot/runtime/suspend settings are never changed automatically by this driver.
 */
#define BRICK_CHARGE_MAX_UA	1920000
#define BRICK_INPUT_MAX_UA	2500000

struct axp2202_charger {
	struct regmap *map;
	struct mutex lock;
};

#define AXP2202_STATUS		0x00
#define AXP2202_CHARGE_STATUS	0x01
#define AXP2202_MODULE_EN	0x19
#define AXP2202_GAUGE_CONFIG	0xa2
#define AXP2202_GAUGE_SOC		0xa4
#define AXP2202_VBAT		0xc4

static int axp2202_get_current(struct regmap *map, unsigned int reg,
			       int *current_ua)
{
	unsigned int value;
	int ret;

	ret = regmap_read(map, reg, &value);
	if (ret)
		return ret;
	value &= AXP2202_CURRENT_MASK;
	if (reg == AXP2202_CHARGE_CURRENT) {
		/* The verified stock setter supports codes 0 through 48 only. */
		if (value > 48)
			return -ENODATA;
		*current_ua = value * AXP2202_CHARGE_STEP_UA;
	} else {
		*current_ua = AXP2202_INPUT_MIN_UA + value * AXP2202_INPUT_STEP_UA;
	}
	return 0;
}

static int axp2202_set_current(struct power_supply *psy, unsigned int reg,
			       int current_ua, int minimum, int step, int maximum)
{
	struct axp2202_charger *charger = power_supply_get_drvdata(psy);
	unsigned int value, selector;
	int ret;

	if (current_ua < minimum || current_ua > maximum)
		return -EINVAL;
	/* Round down: never exceed the requested current budget. */
	selector = (current_ua - minimum) / step;
	mutex_lock(&charger->lock);
	ret = regmap_update_bits(charger->map, reg, AXP2202_CURRENT_MASK, selector);
	if (ret)
		goto unlock;
	ret = regmap_read(charger->map, reg, &value);
	if (!ret && (value & AXP2202_CURRENT_MASK) != selector)
		ret = -EIO;
unlock:
	mutex_unlock(&charger->lock);
	if (!ret)
		power_supply_changed(psy);
	return ret;
}

static int axp2202_battery_set(struct power_supply *psy,
			       enum power_supply_property property,
			       const union power_supply_propval *val)
{
	if (property != POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT)
		return -EINVAL;
	return axp2202_set_current(psy, AXP2202_CHARGE_CURRENT, val->intval,
				  0, AXP2202_CHARGE_STEP_UA, BRICK_CHARGE_MAX_UA);
}

static int axp2202_usb_set(struct power_supply *psy,
			   enum power_supply_property property,
			   const union power_supply_propval *val)
{
	if (property != POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT)
		return -EINVAL;
	return axp2202_set_current(psy, AXP2202_INPUT_CURRENT, val->intval,
				  AXP2202_INPUT_MIN_UA, AXP2202_INPUT_STEP_UA,
				  BRICK_INPUT_MAX_UA);
}

static int axp2202_battery_writeable(struct power_supply *psy,
				     enum power_supply_property property)
{
	return property == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT;
}

static int axp2202_usb_writeable(struct power_supply *psy,
				 enum power_supply_property property)
{
	return property == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT;
}

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
	struct axp2202_charger *charger = power_supply_get_drvdata(psy);
	struct regmap *map = charger->map;
	unsigned int value;
	int voltage, ret;

	switch (property) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		return axp2202_get_current(map, AXP2202_CHARGE_CURRENT, &val->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = BRICK_CHARGE_MAX_UA;
		return 0;
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
	struct axp2202_charger *charger = power_supply_get_drvdata(psy);
	struct regmap *map = charger->map;
	unsigned int value;
	int ret;

	if (property == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT)
		return axp2202_get_current(map, AXP2202_INPUT_CURRENT, &val->intval);
	if (property != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;
	ret = regmap_read(map, AXP2202_STATUS, &value);
	if (ret)
		return ret;
	val->intval = !!(value & BIT(5));
	return 0;
}

static const enum power_supply_property battery_properties[] = {
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
};

static const enum power_supply_property usb_properties[] = {
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc battery_desc = {
	.name = "axp2202-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = battery_properties,
	.num_properties = ARRAY_SIZE(battery_properties),
	.get_property = axp2202_battery_get,
	.set_property = axp2202_battery_set,
	.property_is_writeable = axp2202_battery_writeable,
};

static const struct power_supply_desc usb_desc = {
	.name = "axp2202-usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = usb_properties,
	.num_properties = ARRAY_SIZE(usb_properties),
	.get_property = axp2202_usb_get,
	.set_property = axp2202_usb_set,
	.property_is_writeable = axp2202_usb_writeable,
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
	struct power_supply_config config = { };
	struct regmap *map = dev_get_regmap(pdev->dev.parent, NULL);
	struct axp2202_charger *charger;
	struct axp2202_monitor *monitor;
	int ret;

	if (!map)
		return -ENODEV;
	charger = devm_kzalloc(&pdev->dev, sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;
	charger->map = map;
	mutex_init(&charger->lock);
	config.drv_data = charger;
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
MODULE_DESCRIPTION("AXP2202 battery telemetry and Brick charger current controls");

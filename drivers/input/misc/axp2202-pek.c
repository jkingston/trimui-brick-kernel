// SPDX-License-Identifier: GPL-2.0-only
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>

struct axp2202_key {
	struct input_dev *input;
	int press_irq;
};

static irqreturn_t axp2202_key_irq(int irq, void *data)
{
	struct axp2202_key *key = data;

	input_report_key(key->input, KEY_POWER, irq == key->press_irq);
	input_sync(key->input);
	return IRQ_HANDLED;
}

static int axp2202_key_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct axp2202_key *key;
	int release_irq, ret;

	key = devm_kzalloc(dev, sizeof(*key), GFP_KERNEL);
	if (!key)
		return -ENOMEM;
	key->press_irq = platform_get_irq_byname(pdev, "press");
	if (key->press_irq < 0)
		return key->press_irq;
	release_irq = platform_get_irq_byname(pdev, "release");
	if (release_irq < 0)
		return release_irq;
	key->input = devm_input_allocate_device(dev);
	if (!key->input)
		return -ENOMEM;
	key->input->name = "AXP2202 Power Key";
	key->input->phys = "axp2202-pek/input0";
	key->input->id.bustype = BUS_I2C;
	input_set_capability(key->input, EV_KEY, KEY_POWER);
	ret = input_register_device(key->input);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(dev, key->press_irq, NULL,
					axp2202_key_irq, IRQF_ONESHOT,
					"axp2202-key-press", key);
	if (ret)
		return ret;
	return devm_request_threaded_irq(dev, release_irq, NULL,
					axp2202_key_irq, IRQF_ONESHOT,
					"axp2202-key-release", key);
}

static struct platform_driver axp2202_key_driver = {
	.probe = axp2202_key_probe,
	.driver.name = "axp2202-pek",
};
module_platform_driver(axp2202_key_driver);
MODULE_ALIAS("platform:axp2202-pek");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AXP2202 power key without timing reconfiguration");

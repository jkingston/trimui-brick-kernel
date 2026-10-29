// SPDX-License-Identifier: GPL-2.0-only
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/mfd/trimui-brick-pmic.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/suspend.h>
#include <linux/sun50i-a133-ccu-sleep.h>

struct axp2202_key {
	struct input_dev *input;
	struct device *dev;
	bool wake_armed;
	int parent_irq;
	int press_irq;
};

static irqreturn_t axp2202_key_irq(int irq, void *data)
{
	struct axp2202_key *key = data;

	if (irq == key->press_irq)
		pm_wakeup_event(key->dev, 0);
	input_report_key(key->input, KEY_POWER, irq == key->press_irq);
	input_sync(key->input);
	return IRQ_HANDLED;
}

static void axp2202_key_wakeup_cleanup(void *data)
{
	struct axp2202_key *key = data;

	if (key->wake_armed)
		disable_irq_wake(key->parent_irq);
	device_init_wakeup(key->dev, false);
}

static int axp2202_key_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct trimui_brick_pmic_pek_pdata *pdata = dev_get_platdata(dev);
	struct axp2202_key *key;
	int release_irq, ret;

	key = devm_kzalloc(dev, sizeof(*key), GFP_KERNEL);
	if (!key)
		return -ENOMEM;
	key->dev = dev;
	if (!pdata || pdata->parent_irq <= 0)
		return -EINVAL;
	key->parent_irq = pdata->parent_irq;
	platform_set_drvdata(pdev, key);
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
	ret = devm_request_threaded_irq(dev, release_irq, NULL,
					axp2202_key_irq, IRQF_ONESHOT,
					"axp2202-key-release", key);
	if (ret)
		return ret;
	ret = device_init_wakeup(dev, true);
	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, axp2202_key_wakeup_cleanup, key);
}

static int axp2202_key_prepare(struct device *dev)
{
	int ret;

	if (pm_suspend_target_state == PM_SUSPEND_TO_IDLE)
		return 0;
	if (pm_suspend_target_state != PM_SUSPEND_MEM)
		return -EOPNOTSUPP;

	ret = sun50i_a133_ccu_deep_ready();
	if (ret)
		return ret;
	return 0;
}

static int axp2202_key_suspend(struct device *dev)
{
	struct axp2202_key *key = dev_get_drvdata(dev);
	int ret;

	if (key->wake_armed)
		return -EBUSY;
	if (!device_may_wakeup(dev))
		return 0;
	/*
	 * The nested regmap IRQ API cannot report a failed parent wake vote.
	 * Arm the physical PMIC NMI so R_INTC and a deep parent can fail this
	 * suspend transaction synchronously.
	 */
	ret = enable_irq_wake(key->parent_irq);
	if (!ret)
		key->wake_armed = true;
	return ret;
}

static int axp2202_key_resume(struct device *dev)
{
	struct axp2202_key *key = dev_get_drvdata(dev);
	int ret;

	if (!key->wake_armed)
		return 0;
	ret = disable_irq_wake(key->parent_irq);
	if (!ret)
		key->wake_armed = false;
	return ret;
}

static const struct dev_pm_ops axp2202_key_pm_ops = {
	.prepare = axp2202_key_prepare,
	.suspend = axp2202_key_suspend,
	.resume = axp2202_key_resume,
};

static struct platform_driver axp2202_key_driver = {
	.probe = axp2202_key_probe,
	.driver = {
		.name = "axp2202-pek",
		.pm = &axp2202_key_pm_ops,
	},
};
module_platform_driver(axp2202_key_driver);
MODULE_ALIAS("platform:axp2202-pek");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AXP2202 power key without timing reconfiguration");

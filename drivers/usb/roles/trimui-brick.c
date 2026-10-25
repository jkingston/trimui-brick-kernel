// SPDX-License-Identifier: GPL-2.0-only
/* The Brick routes its bottom PHY between MUSB device and EHCI/OHCI host. */
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/phy/phy-sun4i-usb.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/usb/role.h>
#include <linux/workqueue.h>

struct brick_usb {
	struct device *dev;
	struct phy *phy;
	struct gpio_desc *id;
	struct power_supply *vbus;
	struct notifier_block power_nb;
	struct usb_role_switch *sw;
	struct mutex lock;
	struct delayed_work work;
	struct device_node *nodes[3]; /* MUSB, EHCI, OHCI */
	struct platform_device *children[3];
	enum usb_role role;
	int irq;
	unsigned int retries;
	bool stopping, suspended, fault;
};

static void brick_usb_children_stop(struct brick_usb *usb)
{
	int i;

	for (i = ARRAY_SIZE(usb->children) - 1; i >= 0; i--) {
		if (!usb->children[i])
			continue;
		platform_device_unregister(usb->children[i]);
		usb->children[i] = NULL;
		of_node_clear_flag(usb->nodes[i], OF_POPULATED);
	}
}

static int brick_usb_musb_match(struct device *dev, const void *data)
{
	return dev->bus == &platform_bus_type &&
		!strcmp(to_platform_device(dev)->name, "musb-hdrc");
}

static int brick_usb_child_start(struct brick_usb *usb, unsigned int i)
{
	struct platform_device *child;
	struct device *core;
	int ret;

	/* Disabled nodes are deliberately instantiated only by this owner. */
	if (of_node_test_and_set_flag(usb->nodes[i], OF_POPULATED))
		return -EBUSY;
	child = of_device_alloc(usb->nodes[i], NULL, usb->dev);
	if (!child) {
		ret = -ENOMEM;
		goto clear_node;
	}
	ret = of_device_add(child);
	if (ret) {
		platform_device_put(child);
		goto clear_node;
	}
	usb->children[i] = child;
	/* Unlike automatic device-add probing, device_attach disallows async
	 * deferral and serializes against a probe already running for this child.
	 */
	ret = device_attach(&child->dev);
	if (ret != 1)
		return ret < 0 ? ret : -EPROBE_DEFER;
	if (i)
		return 0;
	/* MUSB glue binding alone does not prove its nested core/UDC is ready. */
	core = device_find_child(&child->dev, NULL, brick_usb_musb_match);
	if (!core)
		return -ENODEV;
	ret = device_attach(core);
	put_device(core);
	return ret == 1 ? 0 : ret < 0 ? ret : -EPROBE_DEFER;
clear_node:
	of_node_clear_flag(usb->nodes[i], OF_POPULATED);
	return ret;
}

static int brick_usb_host_allowed(struct brick_usb *usb)
{
	union power_supply_propval value;
	int ret;

	ret = gpiod_get_value_cansleep(usb->id);
	if (ret)
		return ret < 0 ? ret : -EPERM;
	ret = power_supply_get_property(usb->vbus, POWER_SUPPLY_PROP_ONLINE, &value);
	if (ret)
		return ret;
	return value.intval ? -EBUSY : 0;
}

static int brick_usb_transition(struct brick_usb *usb, enum usb_role role)
{
	int ret;

	if (usb->stopping || usb->suspended)
		return -EBUSY;
	if (usb->fault)
		return -EIO;
	if (role == usb->role)
		return 0;
	brick_usb_children_stop(usb);
	usb->role = USB_ROLE_NONE;
	ret = sun4i_usb_phy_set_role(usb->phy, USB_ROLE_NONE);
	if (ret) {
		usb->fault = true;
		return ret;
	}
	if (role == USB_ROLE_NONE)
		return 0;
	if (role == USB_ROLE_HOST) {
		ret = brick_usb_host_allowed(usb);
		if (ret)
			return ret;
	}
	ret = sun4i_usb_phy_set_role(usb->phy, role);
	if (ret)
		return ret;
	if (role == USB_ROLE_DEVICE) {
		ret = brick_usb_child_start(usb, 0);
	} else {
		ret = brick_usb_child_start(usb, 1);
		if (!ret)
			ret = brick_usb_child_start(usb, 2);
	}
	if (ret) {
		brick_usb_children_stop(usb);
		if (sun4i_usb_phy_set_role(usb->phy, USB_ROLE_NONE))
			usb->fault = true;
		return ret;
	}
	usb->role = role;
	return 0;
}

static int brick_usb_set(struct usb_role_switch *sw, enum usb_role role)
{
	struct brick_usb *usb = usb_role_switch_get_drvdata(sw);
	int ret;

	if (role != USB_ROLE_NONE && role != USB_ROLE_DEVICE && role != USB_ROLE_HOST)
		return -EINVAL;
	mutex_lock(&usb->lock);
	ret = brick_usb_transition(usb, role);
	mutex_unlock(&usb->lock);
	return ret;
}

static enum usb_role brick_usb_get(struct usb_role_switch *sw)
{
	struct brick_usb *usb = usb_role_switch_get_drvdata(sw);

	return READ_ONCE(usb->role);
}

static void brick_usb_scan(struct work_struct *work)
{
	struct brick_usb *usb = container_of(to_delayed_work(work), struct brick_usb, work);
	enum usb_role role;
	int id, ret;

	if (READ_ONCE(usb->stopping) || READ_ONCE(usb->suspended))
		return;
	id = gpiod_get_value_cansleep(usb->id);
	role = id < 0 ? USB_ROLE_NONE : id ? USB_ROLE_DEVICE : USB_ROLE_HOST;
	ret = usb_role_switch_set_role(usb->sw, role);
	if (ret) {
		dev_err_ratelimited(usb->dev, "USB role transition failed: %d\n", ret);
		if (ret == -EPROBE_DEFER && usb->retries++ < 3 &&
		    !READ_ONCE(usb->stopping))
			mod_delayed_work(system_wq, &usb->work, msecs_to_jiffies(1000));
	} else {
		usb->retries = 0;
	}
}

static irqreturn_t brick_usb_irq(int irq, void *data)
{
	struct brick_usb *usb = data;

	if (!READ_ONCE(usb->stopping))
		mod_delayed_work(system_wq, &usb->work, msecs_to_jiffies(50));
	return IRQ_HANDLED;
}

static int brick_usb_power_event(struct notifier_block *nb, unsigned long event,
				void *data)
{
	struct brick_usb *usb = container_of(nb, struct brick_usb, power_nb);

	if (event == PSY_EVENT_PROP_CHANGED && data == usb->vbus &&
	    !READ_ONCE(usb->stopping))
		mod_delayed_work(system_wq, &usb->work, msecs_to_jiffies(50));
	return NOTIFY_OK;
}

static void brick_usb_stop(struct brick_usb *usb)
{
	mutex_lock(&usb->lock);
	if (usb->stopping) {
		mutex_unlock(&usb->lock);
		return;
	}
	usb->stopping = true;
	mutex_unlock(&usb->lock);
	disable_irq(usb->irq);
	power_supply_unreg_notifier(&usb->power_nb);
	cancel_delayed_work_sync(&usb->work);
	mutex_lock(&usb->lock);
	brick_usb_children_stop(usb);
	usb->role = USB_ROLE_NONE;
	if (sun4i_usb_phy_set_role(usb->phy, USB_ROLE_NONE))
		dev_err(usb->dev, "USB resources remain owned after stop\n");
	mutex_unlock(&usb->lock);
	usb_role_switch_unregister(usb->sw);
}

static void brick_usb_nodes_put(void *data)
{
	struct brick_usb *usb = data;
	int i;

	for (i = 0; i < ARRAY_SIZE(usb->nodes); i++)
		of_node_put(usb->nodes[i]);
}

static void brick_usb_supply_put(void *data)
{
	power_supply_put(data);
}

static int brick_usb_get_supply(struct brick_usb *usb)
{
	struct device_node *pmic;
	struct device *parent;
	int ret = -EINVAL;

	pmic = of_parse_phandle(usb->dev->of_node, "vbus-power-supply", 0);
	if (!pmic)
		return -EINVAL;
	/* The existing PMIC telemetry child registers two supplies. Core phandle
	 * lookup matches their common parent, so identify USB by its unique name
	 * and then verify that its ancestor is the referenced PMIC.
	 */
	usb->vbus = power_supply_get_by_name("axp2202-usb");
	if (!usb->vbus) {
		ret = -EPROBE_DEFER;
		goto put_node;
	}
	for (parent = usb->vbus->dev.parent; parent; parent = parent->parent)
		if (parent->of_node == pmic) {
			ret = 0;
			break;
		}
	if (ret)
		power_supply_put(usb->vbus);
	else
		ret = devm_add_action_or_reset(usb->dev, brick_usb_supply_put, usb->vbus);
put_node:
	of_node_put(pmic);
	return ret;
}

static int brick_usb_probe(struct platform_device *pdev)
{
	static const char * const names[] = { "peripheral", "ehci", "ohci" };
	struct usb_role_switch_desc desc = { };
	struct device *dev = &pdev->dev;
	struct brick_usb *usb;
	int i, ret;

	usb = devm_kzalloc(dev, sizeof(*usb), GFP_KERNEL);
	if (!usb)
		return -ENOMEM;
	usb->dev = dev;
	usb->stopping = true;
	mutex_init(&usb->lock);
	INIT_DELAYED_WORK(&usb->work, brick_usb_scan);
	platform_set_drvdata(pdev, usb);
	ret = devm_add_action_or_reset(dev, brick_usb_nodes_put, usb);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		usb->nodes[i] = of_parse_phandle(dev->of_node, names[i], 0);
		if (!usb->nodes[i] || of_device_is_available(usb->nodes[i]))
			return -EINVAL;
	}
	usb->phy = devm_phy_get(dev, "usb");
	if (IS_ERR(usb->phy))
		return dev_err_probe(dev, PTR_ERR(usb->phy), "PHY unavailable\n");
	ret = sun4i_usb_phy_set_role(usb->phy, USB_ROLE_NONE);
	if (ret)
		return dev_err_probe(dev, ret, "PHY handoff unavailable\n");
	usb->id = devm_gpiod_get(dev, "id", GPIOD_IN);
	if (IS_ERR(usb->id))
		return dev_err_probe(dev, PTR_ERR(usb->id), "ID unavailable\n");
	ret = brick_usb_get_supply(usb);
	if (ret)
		return dev_err_probe(dev, ret, "USB input supply unavailable\n");
	usb->irq = gpiod_to_irq(usb->id);
	if (usb->irq < 0)
		return usb->irq;
	desc.fwnode = dev_fwnode(dev);
	desc.set = brick_usb_set;
	desc.get = brick_usb_get;
	desc.driver_data = usb;
	usb->sw = usb_role_switch_register(dev, &desc);
	if (IS_ERR(usb->sw))
		return PTR_ERR(usb->sw);
	usb->power_nb.notifier_call = brick_usb_power_event;
	ret = power_supply_reg_notifier(&usb->power_nb);
	if (ret)
		goto unregister_switch;
	ret = devm_request_irq(dev, usb->irq, brick_usb_irq,
		IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, dev_name(dev), usb);
	if (ret)
		goto unregister_notifier;
	WRITE_ONCE(usb->stopping, false);
	mod_delayed_work(system_wq, &usb->work, 0);
	return 0;
unregister_notifier:
	power_supply_unreg_notifier(&usb->power_nb);
	cancel_delayed_work_sync(&usb->work);
unregister_switch:
	usb_role_switch_unregister(usb->sw);
	return ret;
}

static void brick_usb_remove(struct platform_device *pdev)
{
	brick_usb_stop(platform_get_drvdata(pdev));
}

static int brick_usb_prepare(struct device *dev)
{
	struct brick_usb *usb = dev_get_drvdata(dev);

	mutex_lock(&usb->lock);
	usb->suspended = true;
	mutex_unlock(&usb->lock);
	cancel_delayed_work_sync(&usb->work);
	return 0;
}

static void brick_usb_complete(struct device *dev)
{
	struct brick_usb *usb = dev_get_drvdata(dev);

	mutex_lock(&usb->lock);
	usb->suspended = false;
	mutex_unlock(&usb->lock);
	mod_delayed_work(system_wq, &usb->work, 0);
}

static ssize_t role_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct brick_usb *usb = dev_get_drvdata(dev);
	enum usb_role role = READ_ONCE(usb->role);

	return sysfs_emit(buf, "%s\n", role == USB_ROLE_HOST ? "host" :
			  role == USB_ROLE_DEVICE ? "device" : "none");
}
static DEVICE_ATTR_RO(role);
static struct attribute *brick_usb_attrs[] = { &dev_attr_role.attr, NULL };
ATTRIBUTE_GROUPS(brick_usb);

static const struct dev_pm_ops brick_usb_pm = {
	.prepare = brick_usb_prepare,
	.complete = brick_usb_complete,
};
static const struct of_device_id brick_usb_match[] = {
	{ .compatible = "trimui,brick-usb-role-switch" }, { }
};
MODULE_DEVICE_TABLE(of, brick_usb_match);
static struct platform_driver brick_usb_driver = {
	.probe = brick_usb_probe,
	.remove = brick_usb_remove,
	.shutdown = brick_usb_remove,
	.driver = {
		.name = "trimui-brick-usb-role", .of_match_table = brick_usb_match,
		.pm = &brick_usb_pm,
		.dev_groups = brick_usb_groups,
	},
};
/* All three built-in controller drivers must be registered before role scan. */
static int __init brick_usb_init(void)
{
	return platform_driver_register(&brick_usb_driver);
}
late_initcall(brick_usb_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TrimUI Brick bottom USB controller role coordinator");

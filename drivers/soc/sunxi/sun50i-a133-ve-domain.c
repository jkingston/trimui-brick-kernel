// SPDX-License-Identifier: GPL-2.0-only
/* Allwinner A133 shared video-engine clock and reset domain */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/device/bus.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/panic.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/sun50i-a133-ve-domain.h>
#include <linux/wait.h>

#define A133_VE_RATE 432000000

struct sun50i_a133_ve_domain {
	struct device *dev;
	struct clk_bulk_data clocks[3];
	struct reset_control *reset;
	struct semaphore available;
	spinlock_t state_lock;
	wait_queue_head_t idle;
	unsigned int users;
	bool stopping;
	bool needs_reset;
	struct device *owner;
};

static int sun50i_a133_ve_domain_user_get(struct sun50i_a133_ve_domain *domain)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&domain->state_lock, flags);
	if (domain->stopping)
		ret = -ENODEV;
	else
		domain->users++;
	spin_unlock_irqrestore(&domain->state_lock, flags);

	return ret;
}

static void sun50i_a133_ve_domain_user_put(struct sun50i_a133_ve_domain *domain)
{
	unsigned long flags;
	bool idle;

	spin_lock_irqsave(&domain->state_lock, flags);
	idle = !--domain->users;
	spin_unlock_irqrestore(&domain->state_lock, flags);

	if (idle)
		wake_up_all(&domain->idle);
}

static bool sun50i_a133_ve_domain_is_idle(struct sun50i_a133_ve_domain *domain)
{
	unsigned long flags;
	bool idle;

	spin_lock_irqsave(&domain->state_lock, flags);
	idle = !domain->users;
	spin_unlock_irqrestore(&domain->state_lock, flags);

	return idle;
}

static void sun50i_a133_ve_domain_put(void *data)
{
	put_device(data);
}

struct sun50i_a133_ve_domain *
devm_sun50i_a133_ve_domain_get(struct device *dev)
{
	struct sun50i_a133_ve_domain *domain;
	struct platform_device *pdev;
	struct device_link *link;
	struct device_node *np;
	int ret;

	np = of_parse_phandle(dev->of_node, "allwinner,video-engine-domain", 0);
	if (!np)
		return ERR_PTR(-ENODEV);

	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);

	domain = platform_get_drvdata(pdev);
	if (!domain) {
		platform_device_put(pdev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	link = device_link_add(dev, &pdev->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		platform_device_put(pdev);
		return ERR_PTR(-EINVAL);
	}

	ret = devm_add_action_or_reset(dev, sun50i_a133_ve_domain_put,
				       &pdev->dev);
	if (ret)
		return ERR_PTR(ret);

	return domain;
}
EXPORT_SYMBOL_GPL(devm_sun50i_a133_ve_domain_get);

int sun50i_a133_ve_domain_acquire(struct sun50i_a133_ve_domain *domain,
				  struct device *owner)
{
	int ret;

	ret = sun50i_a133_ve_domain_user_get(domain);
	if (ret)
		return ret;

	ret = down_interruptible(&domain->available);
	if (ret)
		goto err_user;

	if (READ_ONCE(domain->stopping)) {
		ret = -ENODEV;
		goto err_release;
	}

	if (domain->needs_reset) {
		ret = reset_control_reset(domain->reset);
		if (ret)
			goto err_release;
		domain->needs_reset = false;
	}

	ret = clk_bulk_enable(ARRAY_SIZE(domain->clocks), domain->clocks);
	if (ret)
		goto err_assert;

	WRITE_ONCE(domain->owner, owner);
	return 0;

err_assert:
	reset_control_assert(domain->reset);
	domain->needs_reset = true;
err_release:
	up(&domain->available);
err_user:
	sun50i_a133_ve_domain_user_put(domain);
	return ret;
}
EXPORT_SYMBOL_GPL(sun50i_a133_ve_domain_acquire);

void sun50i_a133_ve_domain_release(struct sun50i_a133_ve_domain *domain,
				   struct device *owner)
{
	int ret;

	if (WARN_ON(READ_ONCE(domain->owner) != owner))
		return;

	/*
	 * Do not freeze an in-flight VE transaction behind a disabled clock.
	 * The encoder completion interrupt can precede the final MBUS drain; if
	 * the next owner enables the clocks after those DMA mappings have been
	 * released, that stale transaction resumes against unmapped memory.
	 *
	 * Holding reset across the idle interval matches the standalone Cedrus
	 * power lifecycle and guarantees that every next owner starts clean.
	 */
	ret = reset_control_assert(domain->reset);
	if (ret)
		panic("%s: failed to contain video-engine DMA: %d\n",
		      dev_name(domain->dev), ret);
	domain->needs_reset = true;
	WRITE_ONCE(domain->owner, NULL);
	clk_bulk_disable(ARRAY_SIZE(domain->clocks), domain->clocks);
	up(&domain->available);
	sun50i_a133_ve_domain_user_put(domain);
}
EXPORT_SYMBOL_GPL(sun50i_a133_ve_domain_release);

int sun50i_a133_ve_domain_reset(struct sun50i_a133_ve_domain *domain,
				struct device *owner)
{
	int ret;

	if (WARN_ON(READ_ONCE(domain->owner) != owner))
		return -EPERM;

	ret = reset_control_reset(domain->reset);
	domain->needs_reset = ret != 0;

	return ret;
}
EXPORT_SYMBOL_GPL(sun50i_a133_ve_domain_reset);

int sun50i_a133_ve_domain_assert(struct sun50i_a133_ve_domain *domain,
				 struct device *owner)
{
	int ret;

	if (WARN_ON(READ_ONCE(domain->owner) != owner))
		return -EPERM;

	ret = reset_control_assert(domain->reset);
	if (!ret)
		domain->needs_reset = true;

	return ret;
}
EXPORT_SYMBOL_GPL(sun50i_a133_ve_domain_assert);

static int sun50i_a133_ve_domain_probe(struct platform_device *pdev)
{
	struct sun50i_a133_ve_domain *domain;
	int ret;

	domain = devm_kzalloc(&pdev->dev, sizeof(*domain), GFP_KERNEL);
	if (!domain)
		return -ENOMEM;

	domain->dev = &pdev->dev;
	domain->clocks[0].id = "ahb";
	domain->clocks[1].id = "mod";
	domain->clocks[2].id = "ram";
	ret = devm_clk_bulk_get(&pdev->dev, ARRAY_SIZE(domain->clocks),
				domain->clocks);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to get clocks\n");

	domain->reset = devm_reset_control_get_exclusive(&pdev->dev, NULL);
	if (IS_ERR(domain->reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(domain->reset),
				     "failed to get reset\n");

	ret = clk_set_rate(domain->clocks[1].clk, A133_VE_RATE);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to set module clock rate\n");

	ret = clk_bulk_prepare(ARRAY_SIZE(domain->clocks), domain->clocks);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to prepare clocks\n");

	sema_init(&domain->available, 1);
	spin_lock_init(&domain->state_lock);
	init_waitqueue_head(&domain->idle);
	domain->needs_reset = true;
	platform_set_drvdata(pdev, domain);

	return 0;
}

static void sun50i_a133_ve_domain_remove(struct platform_device *pdev)
{
	struct sun50i_a133_ve_domain *domain = platform_get_drvdata(pdev);
	unsigned long flags;

	spin_lock_irqsave(&domain->state_lock, flags);
	domain->stopping = true;
	spin_unlock_irqrestore(&domain->state_lock, flags);
	wait_event(domain->idle, sun50i_a133_ve_domain_is_idle(domain));

	reset_control_assert(domain->reset);
	clk_bulk_unprepare(ARRAY_SIZE(domain->clocks), domain->clocks);
}

static int sun50i_a133_ve_domain_suspend(struct device *dev)
{
	struct sun50i_a133_ve_domain *domain = dev_get_drvdata(dev);
	int ret;

	ret = down_timeout(&domain->available, msecs_to_jiffies(2500));
	if (ret)
		return ret;
	ret = reset_control_assert(domain->reset);
	if (!ret)
		domain->needs_reset = true;
	up(&domain->available);

	return ret;
}

static const struct dev_pm_ops sun50i_a133_ve_domain_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(sun50i_a133_ve_domain_suspend, NULL)
};

static const struct of_device_id sun50i_a133_ve_domain_of_match[] = {
	{ .compatible = "allwinner,sun50i-a133-video-engine-domain" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun50i_a133_ve_domain_of_match);

static struct platform_driver sun50i_a133_ve_domain_driver = {
	.probe = sun50i_a133_ve_domain_probe,
	.remove = sun50i_a133_ve_domain_remove,
	.driver = {
		.name = "sun50i-a133-ve-domain",
		.of_match_table = sun50i_a133_ve_domain_of_match,
		.pm = &sun50i_a133_ve_domain_pm_ops,
	},
};
module_platform_driver(sun50i_a133_ve_domain_driver);

MODULE_DESCRIPTION("Allwinner A133 shared video-engine domain");
MODULE_LICENSE("GPL");

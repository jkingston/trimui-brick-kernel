// SPDX-License-Identifier: GPL-2.0-only
/* A133 PWM channel 0 for the Brick backlight. */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/reset.h>
#include <linux/suspend.h>
#include "pwm-sun50i-a133-arith.h"
#include "pwm-sun50i-a133-lifecycle.h"

#define PCCR 0x20
#define PCGR 0x40
#define PDZCR 0x60
#define PER 0x80
#define PGR0 0x90
#define PGR1 0x94
#define CER 0xc0
#define PCR 0x100
#define PPR 0x104
/* Period-update status (read-only), observed asserted after a PPR write. */
#define PERIOD_BUSY BIT(11)
struct brick_pwm {
	void __iomem *base;
	struct clk *bus, *osc;
	struct reset_control *rst;
	struct mutex lock;
	bool suspended, retention_fault;
	u32 sleep_pair, sleep_ctl, sleep_period;
};
static u32 rd(struct brick_pwm *p, unsigned int reg)
{
	return readl(p->base + reg);
}
static void wr(struct brick_pwm *p, unsigned int reg, u32 value)
{
	writel(value, p->base + reg);
}
/* Exclusive MMIO reservation owns the whole block. Reject inherited users,
 * including sibling1, before changing its shared source or divider.
 * Upper PCGR bits are unqualified (RFC calls them bypass); never change them.
 */
static int brick_guard(struct brick_pwm *p)
{
	if ((rd(p, PER) & ~BIT(0)) || (rd(p, PCGR) & ~BIT(0)) ||
	    rd(p, CER) || (rd(p, PDZCR) & BIT(0)) ||
	    rd(p, PGR0) || rd(p, PGR1) || rd(p, 0) || rd(p, 0x10))
		return -EBUSY;
	if ((rd(p, PCR) & ~(GENMASK(8, 0) | PERIOD_BUSY)) ||
	    (rd(p, PCCR) & ~(GENMASK(8, 7) | GENMASK(3, 0))))
		return -EOPNOTSUPP;
	return 0;
}
static int brick_apply(struct pwm_chip *chip, struct pwm_device *pwm,
		       const struct pwm_state *s)
{
	struct brick_pwm *p = pwmchip_get_drvdata(chip);
	unsigned int duty;
	u32 pair, ctl, status;
	int ret;

	if (s->usage_power || s->polarity != PWM_POLARITY_NORMAL ||
	    brick_pwm_ticks(s->period, s->duty_cycle, &duty))
		return -EINVAL;
	mutex_lock(&p->lock);
	if (p->retention_fault || p->suspended) {
		ret = p->retention_fault ? -EIO : -EBUSY;
		goto out;
	}
	ret = brick_guard(p);
	if (ret) goto out;
	if (s->enabled && (rd(p, PCR) & PERIOD_BUSY)) { ret = -EBUSY; goto out; }
	/* Stop only channel0 before altering the pair clock or cached waveform. */
	wr(p, PER, rd(p, PER) & ~BIT(0));
	wr(p, PCGR, rd(p, PCGR) & ~BIT(0));
	if (!s->enabled) goto out;
	pair = rd(p, PCCR) & ~(GENMASK(8, 7) | GENMASK(3, 0));
	wr(p, PCCR, pair); /* source0=OSC24M, DIV_M0=/1 */
	ctl = (rd(p, PCR) & ~(GENMASK(8, 0) | PERIOD_BUSY)) | BIT(8);
	wr(p, PCR, ctl); /* prescaler0=/1, active high, continuous */
	wr(p, PPR, (479U << 16) | duty);
	/* Detect mismatched programming before exposing the output. */
	if (rd(p, PCCR) != pair || (rd(p, PCR) & ~PERIOD_BUSY) != ctl ||
	    rd(p, PPR) != ((479U << 16) | duty)) {
		ret = -EIO;
		goto out;
	}
	wr(p, PCGR, rd(p, PCGR) | BIT(0));
	wr(p, PER, rd(p, PER) | BIT(0));
	ret = readl_poll_timeout(p->base + PCR, status, !(status & PERIOD_BUSY), 10, 1000);
	if (ret) {
		wr(p, PER, rd(p, PER) & ~BIT(0));
		wr(p, PCGR, rd(p, PCGR) & ~BIT(0));
	}
out:
	mutex_unlock(&p->lock);
	return ret;
}
static int brick_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
			   struct pwm_state *s)
{
	struct brick_pwm *p = pwmchip_get_drvdata(chip);
	unsigned long long period, duty;
	u32 ctl;
	int ret;

	mutex_lock(&p->lock);
	if (p->retention_fault || p->suspended) {
		ret = p->retention_fault ? -EIO : -EBUSY;
		goto out;
	}
	ret = brick_guard(p);
	if (ret) goto out;
	ctl = rd(p, PCR);
	if (brick_pwm_decode(rd(p, PCCR), ctl, rd(p, PPR), &period, &duty)) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	s->period = period;
	s->duty_cycle = duty;
	s->polarity = ctl & BIT(8) ? PWM_POLARITY_NORMAL : PWM_POLARITY_INVERSED;
	s->enabled = !!(rd(p, PER) & BIT(0)) && !!(rd(p, PCGR) & BIT(0));
	s->usage_power = false;
out:
	mutex_unlock(&p->lock);
	return ret;
}
static const struct pwm_ops brick_ops = {
	.apply = brick_apply, .get_state = brick_get_state,
};
static int brick_reset_status(void *ctx)
{
	struct brick_pwm *p = ctx;

	return reset_control_status(p->rst);
}
static int brick_bus_is_on(void *ctx)
{
	struct brick_pwm *p = ctx;

	return __clk_is_enabled(p->bus);
}
static int brick_bus_enable(void *ctx)
{
	struct brick_pwm *p = ctx;

	return clk_prepare_enable(p->bus);
}
static void brick_bus_disable(void *ctx)
{
	struct brick_pwm *p = ctx;

	clk_disable_unprepare(p->bus);
}
static int brick_reset_deassert(void *ctx)
{
	struct brick_pwm *p = ctx;
	int ret = reset_control_deassert(p->rst);

	if (ret) return ret;
	ret = reset_control_status(p->rst);
	return ret > 0 ? -EIO : ret;
}
static int brick_check_idle(void *ctx)
{
	struct brick_pwm *p = ctx;
	int ret = brick_guard(p);

	if (ret) return ret;
	return (rd(p, PER) | rd(p, PCGR)) & BIT(0) ? -EBUSY : 0;
}
static const struct brick_pwm_start_ops brick_start_ops = {
	.reset_status = brick_reset_status,
	.bus_is_on = brick_bus_is_on,
	.bus_enable = brick_bus_enable,
	.bus_disable = brick_bus_disable,
	.reset_deassert = brick_reset_deassert,
	.check_idle = brick_check_idle,
};
static int brick_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_chip *chip;
	struct brick_pwm *p;
	struct resource *res;
	int ret;

	chip = devm_pwmchip_alloc(dev, 1, sizeof(*p));
	if (IS_ERR(chip)) return PTR_ERR(chip);
	p = pwmchip_get_drvdata(chip);
	mutex_init(&p->lock);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) < PPR + 4) return -EINVAL;
	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base)) return PTR_ERR(p->base);
	p->bus = devm_clk_get(dev, "bus");
	if (IS_ERR(p->bus)) return PTR_ERR(p->bus);
	p->osc = devm_clk_get(dev, "osc24M");
	if (IS_ERR(p->osc)) return PTR_ERR(p->osc);
	p->rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(p->rst)) return PTR_ERR(p->rst);
	if (clk_get_rate(p->osc) != 24000000) return -ERANGE;
	ret = devm_clk_rate_exclusive_get(dev, p->osc);
	if (ret) return ret;
	if (clk_get_rate(p->osc) != 24000000) return -ERANGE;
	ret = brick_pwm_start(p, &brick_start_ops);
	if (ret) return dev_err_probe(dev, ret, "controller startup rejected\n");
	ret = devm_add_action_or_reset(dev, brick_bus_disable, p);
	if (ret) return ret;
	chip->ops = &brick_ops;
	platform_set_drvdata(pdev, chip);
	return pwmchip_add(chip);
}
static void brick_shutdown(struct platform_device *pdev)
{
	struct pwm_chip *chip = platform_get_drvdata(pdev);
	struct brick_pwm *p = pwmchip_get_drvdata(chip);

	mutex_lock(&p->lock);
	/* No pair/controller reset, and no writes to unrelated bits. */
	wr(p, PER, rd(p, PER) & ~BIT(0));
	wr(p, PCGR, rd(p, PCGR) & ~BIT(0));
	mutex_unlock(&p->lock);
}
static void brick_remove(struct platform_device *pdev)
{
	struct pwm_chip *chip = platform_get_drvdata(pdev);

	/* Serialize against consumer apply before shutdown and devm clock release. */
	pwmchip_remove(chip);
	brick_shutdown(pdev);
}
static const struct of_device_id brick_match[] = {
	{ .compatible = "allwinner,sun50i-a133-pwm" }, { }
};
MODULE_DEVICE_TABLE(of, brick_match);
/* s2idle retains this driver's bus clock, oscillator and reset ownership.
 * Deep suspend and hibernation need a separate restore contract.
 */
static int brick_pm_prepare(struct device *dev)
{
	return pm_suspend_target_state == PM_SUSPEND_TO_IDLE ? 0 : -EBUSY;
}
static int brick_pm_suspend(struct device *dev)
{
	struct pwm_chip *chip = dev_get_drvdata(dev);
	struct brick_pwm *p = pwmchip_get_drvdata(chip);
	int ret;

	mutex_lock(&p->lock);
	if (p->retention_fault || p->suspended) {
		ret = p->retention_fault ? -EIO : -EBUSY;
		goto out;
	}
	ret = brick_guard(p);
	if (ret)
		goto out;
	/* PWM device links put the backlight consumer first. Never blank an
	 * active consumer behind its back or restore light before the panel.
	 */
	if ((rd(p, PER) | rd(p, PCGR)) & BIT(0) ||
	    (rd(p, PCR) & PERIOD_BUSY)) {
		ret = -EBUSY;
		goto out;
	}
	p->sleep_pair = rd(p, PCCR);
	p->sleep_ctl = rd(p, PCR);
	p->sleep_period = rd(p, PPR);
	p->suspended = true;
out:
	mutex_unlock(&p->lock);
	return ret;
}
static int brick_pm_resume(struct device *dev)
{
	struct pwm_chip *chip = dev_get_drvdata(dev);
	struct brick_pwm *p = pwmchip_get_drvdata(chip);
	int ret = 0;

	mutex_lock(&p->lock);
	if (!p->suspended)
		goto out;
	/* Check accessibility before MMIO; do not guess a lost-clock/reset
	 * recovery sequence. A fault blocks future applies until reprobe.
	 */
	if (reset_control_status(p->rst) != 0 || !__clk_is_enabled(p->bus) ||
	    clk_get_rate(p->osc) != 24000000) {
		ret = -EIO;
		goto fault;
	}
	ret = brick_guard(p);
	if (ret || rd(p, PCCR) != p->sleep_pair ||
	    rd(p, PCR) != p->sleep_ctl || rd(p, PPR) != p->sleep_period ||
	    ((rd(p, PER) | rd(p, PCGR)) & BIT(0))) {
		ret = -EIO;
		goto fault;
	}
	p->suspended = false;
	goto out;
fault:
	p->retention_fault = true;
	dev_err(dev, "PWM s2idle retention lost; output remains unavailable\n");
out:
	mutex_unlock(&p->lock);
	return ret;
}
static const struct dev_pm_ops brick_pm_ops = {
	.prepare = brick_pm_prepare,
	.suspend = brick_pm_suspend,
	.resume = brick_pm_resume,
};
static struct platform_driver brick_driver = {
	.probe = brick_probe,
	.remove = brick_remove,
	.shutdown = brick_shutdown,
	.driver = { .name = "sun50i-a133-pwm", .of_match_table = brick_match,
		.pm = &brick_pm_ops },
};
module_platform_driver(brick_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Allwinner A133 channel 0 backlight PWM");

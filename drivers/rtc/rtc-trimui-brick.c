// SPDX-License-Identifier: GPL-2.0-only
/* A133 RTC preserving the Brick firmware's clock configuration. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/rtc.h>
#include <linux/timekeeping.h>
#include <linux/spinlock.h>

#include "brick_radio_clocks.h"

#define BRICK_RTC_CTRL	0x00
#define BRICK_RTC_DAY_BUSY	BIT(7)
#define BRICK_RTC_HMS_BUSY	BIT(8)
#define BRICK_RTC_ALARM_BUSY	BIT(9)
#define BRICK_RTC_ALARM_DAY	0x20
#define BRICK_RTC_ALARM_HMS	0x24
#define BRICK_RTC_ALARM_PENDING	0x30
#define BRICK_RTC_ALARM_MAX_GAP	(255LL * 86400)
#define BRICK_RTC_ALARM_ENABLE	0x28
#define BRICK_RTC_ALARM_IRQ_ENABLE	0x2c
#define BRICK_RTC_MAX_SECONDS	5662223999LL

#define BRICK_RTC_DAY	0x10
#define BRICK_RTC_HMS	0x14

struct brick_rtc {
	struct brick_radio_clocks *radio_clocks;
	void __iomem *base;
	struct mutex lock;
	spinlock_t alarm_lock;
	struct rtc_device *rtc;
	int irq;
	bool irq_masked;
	bool stopping;
	bool alarm_fault;
	bool alarm_owned;
	bool time_uncertain;
};

static int brick_rtc_decode(u32 day, u32 hms, time64_t *seconds)
{
	u32 sec = hms & 0x3f;
	u32 min = (hms >> 8) & 0x3f;
	u32 hour = (hms >> 16) & 0x1f;

	if (!day || day > 0xffff || (hms & ~0x1f3f3f) ||
	    sec > 59 || min > 59 || hour > 23)
		return -EINVAL;

	/* Stock Brick firmware stores one-based days since 1970-01-01. */
	*seconds = (time64_t)(day - 1) * 86400 + hour * 3600 + min * 60 + sec;
	return 0;
}

static int brick_rtc_snapshot(struct brick_rtc *chip, time64_t *seconds)
{
	u32 day, hms;
	int attempt;

	for (attempt = 0; attempt < 10; attempt++) {
		day = readl(chip->base + BRICK_RTC_DAY);
		hms = readl(chip->base + BRICK_RTC_HMS);
		if (day == readl(chip->base + BRICK_RTC_DAY) &&
		    hms == readl(chip->base + BRICK_RTC_HMS))
			return brick_rtc_decode(day, hms, seconds);
	}
	return -EIO;
}

static int brick_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct brick_rtc *chip = dev_get_drvdata(dev);
	time64_t seconds;
	int ret;

	mutex_lock(&chip->lock);
	ret = chip->stopping ? -ENODEV : chip->time_uncertain ? -EIO :
		brick_rtc_snapshot(chip, &seconds);
	mutex_unlock(&chip->lock);
	if (!ret)
		rtc_time64_to_tm(seconds, tm);
	return ret;
}

static int brick_rtc_encode(struct rtc_time *tm, time64_t *seconds,
			    u32 *day, u32 *hms)
{
	int ret = rtc_valid_tm(tm);

	if (ret)
		return ret;
	*seconds = rtc_tm_to_time64(tm);
	if (*seconds < 0 || *seconds > BRICK_RTC_MAX_SECONDS)
		return -ERANGE;
	*day = div_u64(*seconds, 86400) + 1;
	*hms = tm->tm_sec | (tm->tm_min << 8) | (tm->tm_hour << 16);
	return 0;
}

static int brick_rtc_wait(struct brick_rtc *chip, u32 mask)
{
	u32 value;

	return readl_poll_timeout(chip->base + BRICK_RTC_CTRL, value,
				  !(value & mask), 100, 50000);
}

static int brick_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct brick_rtc *chip = dev_get_drvdata(dev);
	time64_t requested, actual;
	u64 started, allowance;
	u32 day, hms;
	int ret;

	ret = brick_rtc_encode(tm, &requested, &day, &hms);
	if (ret)
		return ret;
	mutex_lock(&chip->lock);
	if (chip->stopping) {
		ret = -ENODEV;
		goto unlock;
	}
	/* RTC core holds ops_lock: include timers awaiting deferred IRQ work. */
	if (timerqueue_getnext(&chip->rtc->timerqueue)) {
		ret = -EBUSY;
		goto unlock;
	}
	/* Userspace must explicitly disable/service an alarm before time jumps. */
	if ((readl(chip->base + BRICK_RTC_ALARM_ENABLE) |
	     readl(chip->base + BRICK_RTC_ALARM_IRQ_ENABLE) |
	     readl(chip->base + BRICK_RTC_ALARM_PENDING)) & BIT(0)) {
		ret = -EBUSY;
		goto unlock;
	}
	ret = brick_rtc_wait(chip, BRICK_RTC_DAY_BUSY | BRICK_RTC_HMS_BUSY);
	if (ret)
		goto unlock;
	usleep_range(2000, 3000);
	started = ktime_get_ns();
	/* Separate writes can partially succeed. Do not expose uncertain time. */
	chip->time_uncertain = true;
	writel(hms, chip->base + BRICK_RTC_HMS);
	usleep_range(2000, 3000);
	ret = brick_rtc_wait(chip, BRICK_RTC_HMS_BUSY | BRICK_RTC_DAY_BUSY);
	if (ret)
		goto unlock;
	writel(day, chip->base + BRICK_RTC_DAY);
	usleep_range(2000, 3000);
	ret = brick_rtc_wait(chip, BRICK_RTC_DAY_BUSY | BRICK_RTC_HMS_BUSY);
	if (ret)
		goto unlock;
	ret = brick_rtc_snapshot(chip, &actual);
	if (ret)
		goto unlock;
	allowance = DIV_ROUND_UP_ULL(ktime_get_ns() - started, NSEC_PER_SEC);
	if (actual < requested || actual - requested > allowance) {
		ret = -EIO;
		goto unlock;
	}
	chip->time_uncertain = false;
unlock:
	mutex_unlock(&chip->lock);
	return ret;
}

/* Gate and pending accesses are serialized against the IRQ handler. */
static void brick_rtc_mask_irq(struct brick_rtc *chip)
{
	if (!chip->irq_masked) {
		chip->irq_masked = true;
		disable_irq_nosync(chip->irq);
	}
}

static int brick_rtc_disarm(struct brick_rtc *chip)
{
	u32 value;

	value = readl(chip->base + BRICK_RTC_ALARM_IRQ_ENABLE);
	writel(value & ~BIT(0), chip->base + BRICK_RTC_ALARM_IRQ_ENABLE);
	value = readl(chip->base + BRICK_RTC_ALARM_ENABLE);
	writel(value & ~BIT(0), chip->base + BRICK_RTC_ALARM_ENABLE);
	if ((readl(chip->base + BRICK_RTC_ALARM_ENABLE) |
	     readl(chip->base + BRICK_RTC_ALARM_IRQ_ENABLE)) & BIT(0)) {
		chip->alarm_fault = true;
		brick_rtc_mask_irq(chip);
		return -EIO;
	}
	return 0;
}

/* Only after disarming: no new match can race the W1C readback. */
static int brick_rtc_ack_alarm(struct brick_rtc *chip)
{
	writel(BIT(0), chip->base + BRICK_RTC_ALARM_PENDING);
	if (readl(chip->base + BRICK_RTC_ALARM_PENDING) & BIT(0)) {
		chip->alarm_fault = true;
		brick_rtc_mask_irq(chip);
		return -EIO;
	}
	return 0;
}

static int brick_rtc_alarm_snapshot(struct brick_rtc *chip, time64_t *seconds)
{
	u32 day, hms;
	int attempt;

	for (attempt = 0; attempt < 10; attempt++) {
		day = readl(chip->base + BRICK_RTC_ALARM_DAY);
		hms = readl(chip->base + BRICK_RTC_ALARM_HMS);
		if (day == readl(chip->base + BRICK_RTC_ALARM_DAY) &&
		    hms == readl(chip->base + BRICK_RTC_ALARM_HMS))
			return brick_rtc_decode(day, hms, seconds);
	}
	return -EIO;
}

static int brick_rtc_alarm_future(struct brick_rtc *chip, time64_t target)
{
	time64_t now;
	int ret = brick_rtc_snapshot(chip, &now);

	if (ret)
		return ret;
	if (target <= now)
		return -ETIME;
	/* Stock's qualified scheduling horizon, not a register-width limit. */
	if (target - now > BRICK_RTC_ALARM_MAX_GAP)
		return -ERANGE;
	return 0;
}

static int brick_rtc_arm(struct brick_rtc *chip, time64_t target)
{
	time64_t now;
	u32 value;
	int ret;

	ret = brick_rtc_alarm_future(chip, target);
	if (ret)
		return ret;
	ret = brick_rtc_ack_alarm(chip);
	if (ret)
		return ret;
	value = readl(chip->base + BRICK_RTC_ALARM_ENABLE);
	writel(value | BIT(0), chip->base + BRICK_RTC_ALARM_ENABLE);
	value = readl(chip->base + BRICK_RTC_ALARM_IRQ_ENABLE);
	writel(value | BIT(0), chip->base + BRICK_RTC_ALARM_IRQ_ENABLE);
	if (!(readl(chip->base + BRICK_RTC_ALARM_ENABLE) & BIT(0)) ||
	    !(readl(chip->base + BRICK_RTC_ALARM_IRQ_ENABLE) & BIT(0)))
		return -EIO;
	/* A second can tick between the future check and enabling the match. */
	ret = brick_rtc_snapshot(chip, &now);
	if (ret)
		return ret;
	if (now >= target &&
	    !(readl(chip->base + BRICK_RTC_ALARM_PENDING) & BIT(0)))
		return -ETIME;
	return 0;
}

static int brick_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct brick_rtc *chip = dev_get_drvdata(dev);
	unsigned long flags;
	time64_t seconds;
	int ret;

	mutex_lock(&chip->lock);
	spin_lock_irqsave(&chip->alarm_lock, flags);
	ret = chip->stopping ? -ENODEV : brick_rtc_alarm_snapshot(chip, &seconds);
	alarm->enabled = !!(readl(chip->base + BRICK_RTC_ALARM_ENABLE) & BIT(0)) &&
		!!(readl(chip->base + BRICK_RTC_ALARM_IRQ_ENABLE) & BIT(0));
	alarm->pending = !!(readl(chip->base + BRICK_RTC_ALARM_PENDING) & BIT(0));
	spin_unlock_irqrestore(&chip->alarm_lock, flags);
	mutex_unlock(&chip->lock);
	if (!ret)
		rtc_time64_to_tm(seconds, &alarm->time);
	return ret;
}

static int brick_rtc_alarm_wait(struct brick_rtc *chip)
{
	int ret = brick_rtc_wait(chip, BRICK_RTC_ALARM_BUSY);

	if (ret)
		return ret;
	usleep_range(2000, 3000);
	return brick_rtc_wait(chip, BRICK_RTC_ALARM_BUSY);
}

static int brick_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct brick_rtc *chip = dev_get_drvdata(dev);
	time64_t target, actual;
	unsigned long flags;
	u32 day, hms;
	int ret, cleanup;

	ret = brick_rtc_encode(&alarm->time, &target, &day, &hms);
	if (ret)
		return ret;
	mutex_lock(&chip->lock);
	if (chip->stopping || chip->time_uncertain) {
		ret = chip->stopping ? -ENODEV : -EIO;
		goto unlock;
	}
	if (alarm->enabled) {
		ret = brick_rtc_alarm_future(chip, target);
		if (ret)
			goto unlock;
	}
	spin_lock_irqsave(&chip->alarm_lock, flags);
	if (chip->alarm_fault) {
		ret = -EIO;
		goto unlock_gate;
	}
	chip->alarm_owned = true;
	ret = brick_rtc_disarm(chip);
	if (ret)
		goto unlock_gate;
	ret = brick_rtc_ack_alarm(chip);
	if (ret)
		goto unlock_gate;
	spin_unlock_irqrestore(&chip->alarm_lock, flags);

	ret = brick_rtc_alarm_wait(chip);
	if (ret)
		goto fail;
	writel(0, chip->base + BRICK_RTC_ALARM_DAY);
	ret = brick_rtc_alarm_wait(chip);
	if (ret)
		goto fail;
	writel(0, chip->base + BRICK_RTC_ALARM_HMS);
	ret = brick_rtc_alarm_wait(chip);
	if (ret)
		goto fail;
	writel(day, chip->base + BRICK_RTC_ALARM_DAY);
	ret = brick_rtc_alarm_wait(chip);
	if (ret)
		goto fail;
	writel(hms, chip->base + BRICK_RTC_ALARM_HMS);
	ret = brick_rtc_alarm_wait(chip);
	if (ret)
		goto fail;

	spin_lock_irqsave(&chip->alarm_lock, flags);
	ret = brick_rtc_alarm_snapshot(chip, &actual);
	if (ret || actual != target || chip->alarm_fault) {
		ret = -EIO;
		goto fail_gate;
	}
	if (alarm->enabled) {
		ret = brick_rtc_arm(chip, target);
		if (ret)
			goto fail_gate;
	}
	goto unlock_gate;
fail:
	spin_lock_irqsave(&chip->alarm_lock, flags);
fail_gate:
	if (ret != -ETIME && ret != -ERANGE) {
		chip->alarm_fault = true;
		brick_rtc_mask_irq(chip);
	}
	cleanup = brick_rtc_disarm(chip);
	if (cleanup)
		ret = cleanup;
unlock_gate:
	spin_unlock_irqrestore(&chip->alarm_lock, flags);
unlock:
	mutex_unlock(&chip->lock);
	return ret;
}

static int brick_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct brick_rtc *chip = dev_get_drvdata(dev);
	unsigned long flags;
	time64_t target;
	int ret, cleanup;

	mutex_lock(&chip->lock);
	spin_lock_irqsave(&chip->alarm_lock, flags);
	if (chip->stopping) {
		ret = -ENODEV;
		goto unlock;
	}
	if (!enabled) {
		chip->alarm_owned = true;
		ret = brick_rtc_disarm(chip);
		if (!ret)
			ret = brick_rtc_ack_alarm(chip);
		goto unlock;
	}
	if (chip->alarm_fault || chip->time_uncertain) {
		ret = -EIO;
		goto unlock;
	}
	ret = brick_rtc_alarm_snapshot(chip, &target);
	if (ret)
		goto unlock;
	ret = brick_rtc_alarm_future(chip, target);
	if (ret)
		goto unlock;
	chip->alarm_owned = true;
	ret = brick_rtc_disarm(chip);
	if (!ret)
		ret = brick_rtc_arm(chip, target);
	if (ret) {
		if (ret != -ETIME && ret != -ERANGE) {
			chip->alarm_fault = true;
			brick_rtc_mask_irq(chip);
		}
		cleanup = brick_rtc_disarm(chip);
		if (cleanup)
			ret = cleanup;
	}
unlock:
	spin_unlock_irqrestore(&chip->alarm_lock, flags);
	mutex_unlock(&chip->lock);
	return ret;
}

static irqreturn_t brick_rtc_irq(int irq, void *data)
{
	struct brick_rtc *chip = data;
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&chip->alarm_lock, flags);
	pending = readl(chip->base + BRICK_RTC_ALARM_PENDING) & BIT(0);
	if (pending) {
		if (!brick_rtc_disarm(chip))
			brick_rtc_ack_alarm(chip);
		else
			writel(BIT(0), chip->base + BRICK_RTC_ALARM_PENDING);
	}
	spin_unlock_irqrestore(&chip->alarm_lock, flags);
	if (!pending)
		return IRQ_NONE;
	rtc_update_irq(chip->rtc, 1, RTC_IRQF | RTC_AF);
	return IRQ_HANDLED;
}

static void brick_rtc_quiesce(void *data)
{
	struct brick_rtc *chip = data;
	unsigned long flags;

	mutex_lock(&chip->lock);
	spin_lock_irqsave(&chip->alarm_lock, flags);
	chip->stopping = true;
	brick_rtc_mask_irq(chip);
	/* Preserve an inherited alarm that userspace never took over. */
	if (chip->alarm_owned && !brick_rtc_disarm(chip))
		brick_rtc_ack_alarm(chip);
	spin_unlock_irqrestore(&chip->alarm_lock, flags);
	mutex_unlock(&chip->lock);
	synchronize_irq(chip->irq);
}

/* Inspect inherited state without taking ownership or clearing pending. */
static int brick_rtc_check_alarm(struct brick_rtc *chip)
{
	bool enabled, irq_enabled, pending;
	time64_t target, now;
	int ret;

	enabled = readl(chip->base + BRICK_RTC_ALARM_ENABLE) & BIT(0);
	irq_enabled = readl(chip->base + BRICK_RTC_ALARM_IRQ_ENABLE) & BIT(0);
	pending = readl(chip->base + BRICK_RTC_ALARM_PENDING) & BIT(0);
	if (enabled != irq_enabled || (pending && !enabled))
		return -EBUSY;
	if (!enabled && !pending)
		return 0;
	ret = brick_rtc_alarm_snapshot(chip, &target);
	if (ret)
		return ret;
	ret = brick_rtc_snapshot(chip, &now);
	if (ret)
		return ret;
	return enabled && target <= now && !pending ? -ETIME : 0;
}

static const struct rtc_class_ops brick_rtc_ops = {
	.read_time = brick_rtc_read_time,
	.set_time = brick_rtc_set_time,
	.read_alarm = brick_rtc_read_alarm,
	.set_alarm = brick_rtc_set_alarm,
	.alarm_irq_enable = brick_rtc_alarm_irq_enable,
};

static int brick_rtc_register_time(struct platform_device *pdev, struct brick_rtc *chip,
				   bool *invalid_state)
{
	struct device *dev = &pdev->dev;
	struct rtc_device *rtc;
	time64_t first, last;
	u64 first_before, first_after, last_before, last_after;
	u64 minimum, maximum;
	unsigned long flags;
	bool enable;
	int ret;

	*invalid_state = true;
	/* Bus enable can expose stale APB shadows. Settle and verify progress. */
	msleep(500);
	first_before = ktime_get_ns();
	ret = brick_rtc_snapshot(chip, &first);
	first_after = ktime_get_ns();
	if (ret)
		return dev_err_probe(dev, ret, "invalid initial time\n");
	msleep(2000);
	last_before = ktime_get_ns();
	ret = brick_rtc_snapshot(chip, &last);
	last_after = ktime_get_ns();
	if (ret)
		return dev_err_probe(dev, ret, "invalid subsequent time\n");
	/* Bound sample separation, including scheduling delays around MMIO. */
	minimum = div_u64(last_before - first_after, NSEC_PER_SEC);
	maximum = DIV_ROUND_UP_ULL(last_after - first_before, NSEC_PER_SEC);
	if (last <= first || last - first < minimum || last - first > maximum)
		return dev_err_probe(dev, -EIO, "RTC counter did not progress correctly\n");

	ret = brick_rtc_check_alarm(chip);
	if (ret)
		return dev_err_probe(dev, ret, "unsupported inherited alarm state\n");
	*invalid_state = false;
	chip->irq = platform_get_irq(pdev, 0);
	if (chip->irq < 0)
		return chip->irq;
	rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(rtc))
		return PTR_ERR(rtc);
	chip->rtc = rtc;
	ret = devm_request_irq(dev, chip->irq, brick_rtc_irq, IRQF_NO_AUTOEN,
			       dev_name(dev), chip);
	if (ret)
		return dev_err_probe(dev, ret, "cannot request alarm IRQ\n");
	rtc->ops = &brick_rtc_ops;
	rtc->alarm_offset_max = BRICK_RTC_ALARM_MAX_GAP;
	rtc->range_min = 0;
	rtc->range_max = BRICK_RTC_MAX_SECONDS; /* Day 65535: 2149-06-05 23:59:59. */

	clear_bit(RTC_FEATURE_UPDATE_INTERRUPT, rtc->features);
	ret = devm_rtc_register_device(rtc);
	if (ret) {
		brick_rtc_quiesce(chip);
		return ret;
	}
	ret = devm_add_action_or_reset(dev, brick_rtc_quiesce, chip);
	if (ret)
		return ret;
	/* The cdev is already visible; a failed user operation may have faulted. */
	spin_lock_irqsave(&chip->alarm_lock, flags);
	enable = !chip->alarm_fault;
	if (enable)
		chip->irq_masked = false;
	spin_unlock_irqrestore(&chip->alarm_lock, flags);
	if (enable)
		enable_irq(chip->irq);
	return 0;
}

static int brick_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *reset;
	struct brick_rtc *chip;
	struct clk *bus;
	void *time_group;
	bool invalid_state = false;
	int ret;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	mutex_init(&chip->lock);
	spin_lock_init(&chip->alarm_lock);
	chip->irq_masked = true;
	platform_set_drvdata(pdev, chip);
	reset = devm_reset_control_get_exclusive(dev, "bus");
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "cannot acquire reset\n");
	ret = reset_control_status(reset);
	if (ret)
		return dev_err_probe(dev, ret < 0 ? ret : -EBUSY,
				     "RTC must be out of reset\n");
	bus = devm_clk_get_enabled(dev, "bus");
	if (IS_ERR(bus))
		return dev_err_probe(dev, PTR_ERR(bus), "cannot enable bus clock\n");
	chip->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(chip->base))
		return PTR_ERR(chip->base);

	/* Publish no provider until transient RTC dependency failures are resolved. */
	time_group = devres_open_group(dev, NULL, GFP_KERNEL);
	if (!time_group)
		return -ENOMEM;
	ret = brick_rtc_register_time(pdev, chip, &invalid_state);
	if (ret) {
		devres_release_group(dev, time_group);
		chip->rtc = NULL;
		if (!invalid_state)
			return ret; /* Includes EPROBE_DEFER and IRQ/allocation failures. */
		dev_warn(dev, "retained RTC state invalid (%d); enabling radio clocks only\n", ret);
	} else {
		devres_remove_group(dev, time_group);
	}
	return brick_radio_clocks_register(dev, chip->base, &chip->radio_clocks);
}

static void brick_rtc_sync_state(struct device *dev)
{
	struct brick_rtc *chip = dev_get_drvdata(dev);

	brick_radio_clocks_sync(chip->radio_clocks);
}

static int brick_rtc_suspend(struct device *dev)
{
	/* Bus-off shadow synchronization and wakeup are not implemented. */
	return -EBUSY;
}

static DEFINE_SIMPLE_DEV_PM_OPS(brick_rtc_pm_ops, brick_rtc_suspend, NULL);

static const struct of_device_id brick_rtc_match[] = {
	{ .compatible = "trimui,brick-rtc" },
	{ }
};
MODULE_DEVICE_TABLE(of, brick_rtc_match);

static struct platform_driver brick_rtc_driver = {
	.probe = brick_rtc_probe,
	.driver = {
		.name = "trimui-brick-rtc",
		.suppress_bind_attrs = true,
		.sync_state = brick_rtc_sync_state,
		.of_match_table = brick_rtc_match,
		.pm = pm_sleep_ptr(&brick_rtc_pm_ops),
	},
};
module_platform_driver(brick_rtc_driver);

MODULE_DESCRIPTION("TrimUI Brick RTC");
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0-only
/* A133 RTC time reader preserving the Brick firmware's clock configuration. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/rtc.h>
#include <linux/timekeeping.h>

#define BRICK_RTC_DAY	0x10
#define BRICK_RTC_HMS	0x14

struct brick_rtc {
	void __iomem *base;
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

	ret = brick_rtc_snapshot(chip, &seconds);
	if (!ret)
		rtc_time64_to_tm(seconds, tm);
	return ret;
}

static const struct rtc_class_ops brick_rtc_ops = {
	.read_time = brick_rtc_read_time,
};

static int brick_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *reset;
	struct rtc_device *rtc;
	struct brick_rtc *chip;
	struct clk *bus;
	time64_t first, last;
	u64 first_before, first_after, last_before, last_after;
	u64 minimum, maximum;
	int ret;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
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

	rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(rtc))
		return PTR_ERR(rtc);
	rtc->ops = &brick_rtc_ops;
	rtc->range_min = 0;
	rtc->range_max = 5662223999LL; /* Day 65535: 2149-06-05 23:59:59. */
	clear_bit(RTC_FEATURE_ALARM, rtc->features);
	clear_bit(RTC_FEATURE_UPDATE_INTERRUPT, rtc->features);
	return devm_rtc_register_device(rtc);
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
		.of_match_table = brick_rtc_match,
		.pm = pm_sleep_ptr(&brick_rtc_pm_ops),
	},
};
module_platform_driver(brick_rtc_driver);

MODULE_DESCRIPTION("TrimUI Brick read-only RTC");
MODULE_LICENSE("GPL");

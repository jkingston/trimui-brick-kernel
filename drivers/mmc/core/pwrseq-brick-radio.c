// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include "pwrseq.h"

struct brick_pwrseq {
	struct mmc_pwrseq seq;
	struct clk_bulk_data clocks[2];
	struct gpio_desc *reset;
	bool enabled;
};
static struct brick_pwrseq *to_brick(struct mmc_host *host)
{
	return container_of(host->pwrseq, struct brick_pwrseq, seq);
}
static void brick_pre(struct mmc_host *host)
{
	struct brick_pwrseq *p = to_brick(host);
	int ret;

	gpiod_set_value_cansleep(p->reset, 1);
	if (p->enabled)
		return;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(p->clocks), p->clocks);
	if (ret) {
		dev_err(p->seq.dev, "radio clocks unavailable: %d; keeping reset asserted\n", ret);
		return;
	}
	p->enabled = true;
}
static void brick_post(struct mmc_host *host)
{
	struct brick_pwrseq *p = to_brick(host);

	if (!p->enabled)
		return;
	gpiod_set_value_cansleep(p->reset, 0);
	msleep(100);
}
static void brick_off(struct mmc_host *host)
{
	struct brick_pwrseq *p = to_brick(host);

	gpiod_set_value_cansleep(p->reset, 1);
	if (p->enabled) {
		usleep_range(1000, 2000);
		clk_bulk_disable_unprepare(ARRAY_SIZE(p->clocks), p->clocks);
		p->enabled = false;
	}
}
static const struct mmc_pwrseq_ops brick_ops = {
	.pre_power_on = brick_pre, .post_power_on = brick_post,
	.power_off = brick_off,
};
static int brick_probe(struct platform_device *pdev)
{
	struct brick_pwrseq *p;
	int ret;

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->clocks[0].id = "lpo";
	p->clocks[1].id = "ref";
	ret = devm_clk_bulk_get(&pdev->dev, ARRAY_SIZE(p->clocks), p->clocks);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "radio clocks\n");
	p->reset = devm_gpiod_get(&pdev->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(p->reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(p->reset), "WLAN reset\n");
	p->seq.dev = &pdev->dev;
	p->seq.owner = THIS_MODULE;
	p->seq.ops = &brick_ops;
	platform_set_drvdata(pdev, p);
	return mmc_pwrseq_register(&p->seq);
}
static void brick_remove(struct platform_device *pdev)
{
	struct brick_pwrseq *p = platform_get_drvdata(pdev);

	mmc_pwrseq_unregister(&p->seq);
	gpiod_set_value_cansleep(p->reset, 1);
	if (p->enabled)
		clk_bulk_disable_unprepare(ARRAY_SIZE(p->clocks), p->clocks);
}
static const struct of_device_id brick_match[] = {
	{ .compatible = "trimui,brick-radio-pwrseq" }, {}
};
MODULE_DEVICE_TABLE(of, brick_match);
static struct platform_driver brick_driver = {
	.probe = brick_probe, .remove = brick_remove,
	.driver = { .name = "brick-radio-pwrseq", .of_match_table = brick_match,
		.suppress_bind_attrs = true },
};
module_platform_driver(brick_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TrimUI Brick shared-clock WLAN power sequence");

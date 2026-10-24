// SPDX-License-Identifier: GPL-2.0-only
/* A133 boot-source-preserving radio output gates, owned by the RTC device. */
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include "brick_radio_clocks.h"

#define LPO_REG 0x60
#define LPO_SOURCE (GENMASK(2, 1) | BIT(16))
#define DCXO_REG 0x160
struct brick_radio_clocks {
	void __iomem *base;
	spinlock_t lock;
	u32 source;
	struct clk_bulk_data boot[2];
	struct mutex boot_lock;
	bool boot_held, published, handoff_done;
};
struct radio_output {
	struct clk_hw hw;
	struct brick_radio_clocks *owner;
	unsigned int index;
};
static struct radio_output *to_output(struct clk_hw *hw)
{
	return container_of(hw, struct radio_output, hw);
}
static int radio_enable(struct clk_hw *hw)
{
	struct radio_output *out = to_output(hw);
	struct brick_radio_clocks *clocks = out->owner;
	unsigned long flags;
	u32 old, val, check, mask = out->index ? BIT(31) : BIT(0);
	u32 reg = out->index ? DCXO_REG : LPO_REG;
	int ret = 0;

	spin_lock_irqsave(&clocks->lock, flags);
	/* Refuse changed source instead of programming mux/divider/calibration. */
	if ((readl(clocks->base + LPO_REG) & LPO_SOURCE) != clocks->source) {
		ret = -EIO;
		goto unlock;
	}
	old = readl(clocks->base + reg);
	val = out->index ? old & ~mask : old | mask;
	writel(val, clocks->base + reg);
	check = readl(clocks->base + reg);
	if ((check & mask) != (val & mask)) {
		/* Restore only this gate bit, retaining any unrelated field changes. */
		writel((check & ~mask) | (old & mask), clocks->base + reg);
		ret = -EIO;
	}
unlock:
	spin_unlock_irqrestore(&clocks->lock, flags);
	return ret;
}
static void radio_disable(struct clk_hw *hw)
{
	struct radio_output *out = to_output(hw);
	struct brick_radio_clocks *clocks = out->owner;
	unsigned long flags;
	u32 reg = out->index ? DCXO_REG : LPO_REG, val;

	spin_lock_irqsave(&clocks->lock, flags);
	if (!clocks->handoff_done) {
		spin_unlock_irqrestore(&clocks->lock, flags);
		return;
	}
	val = readl(clocks->base + reg);
	val = out->index ? val | BIT(31) : val & ~BIT(0);
	writel(val, clocks->base + reg);
	readl(clocks->base + reg);
	spin_unlock_irqrestore(&clocks->lock, flags);
}
static int radio_is_enabled(struct clk_hw *hw)
{
	struct radio_output *out = to_output(hw);

	if (out->index)
		return !(readl(out->owner->base + DCXO_REG) & BIT(31));
	return !!(readl(out->owner->base + LPO_REG) & BIT(0));
}
static unsigned long radio_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	/* No physical rate measurement or programmable divisor established. */
	return 0;
}
static const struct clk_ops radio_ops = {
	.enable = radio_enable, .disable = radio_disable,
	.is_enabled = radio_is_enabled, .recalc_rate = radio_rate,
};
void brick_radio_clocks_sync(struct brick_radio_clocks *clocks)
{
	unsigned long flags;

	mutex_lock(&clocks->boot_lock);
	/* Before successful handoff even error unwind must preserve boot outputs. */
	spin_lock_irqsave(&clocks->lock, flags);
	clocks->handoff_done = clocks->published;
	spin_unlock_irqrestore(&clocks->lock, flags);
	if (clocks->boot_held) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(clocks->boot), clocks->boot);
		clocks->boot_held = false;
	}
	mutex_unlock(&clocks->boot_lock);
}
static void radio_boot_release(void *data)
{
	struct brick_radio_clocks *clocks = data;
	unsigned int i;
	unsigned long flags;
	u32 val;

	brick_radio_clocks_sync(clocks);
	/* Failed publication must restore inherited outputs, not gate boot radios. */
	if (!clocks->published) {
		spin_lock_irqsave(&clocks->lock, flags);
		val = readl(clocks->base + LPO_REG);
		writel(val | BIT(0), clocks->base + LPO_REG);
		val = readl(clocks->base + DCXO_REG);
		writel(val & ~BIT(31), clocks->base + DCXO_REG);
		readl(clocks->base + DCXO_REG);
		spin_unlock_irqrestore(&clocks->lock, flags);
	}
	for (i = 0; i < ARRAY_SIZE(clocks->boot); i++)
		clk_put(clocks->boot[i].clk);
}
int brick_radio_clocks_register(struct device *dev, void __iomem *base,
			       struct brick_radio_clocks **result)
{
	static const char * const names[] = { "radio-lpo", "radio-ref" };
	struct clk_hw_onecell_data *data;
	struct radio_output *outputs;
	struct brick_radio_clocks *clocks;
	u32 lpo = readl(base + LPO_REG), parent = (lpo >> 1) & 3;
	unsigned int i;
	int ret;

	/* Only hand off working boot sources; source2 requires its divider on. */
	if (parent == 3 || (parent == 2 && !(lpo & BIT(16))) ||
	    !(lpo & BIT(0)) || (readl(base + DCXO_REG) & BIT(31)))
		return dev_err_probe(dev, -EIO, "unsupported inherited radio clocks\n");
	clocks = devm_kzalloc(dev, sizeof(*clocks), GFP_KERNEL);
	data = devm_kzalloc(dev, struct_size(data, hws, 2), GFP_KERNEL);
	outputs = devm_kcalloc(dev, 2, sizeof(*outputs), GFP_KERNEL);
	if (!clocks || !data || !outputs)
		return -ENOMEM;
	clocks->base = base;
	clocks->source = lpo & LPO_SOURCE;
	spin_lock_init(&clocks->lock);
	mutex_init(&clocks->boot_lock);
	data->num = 2;
	for (i = 0; i < 2; i++) {
		struct clk_init_data init = {
			.ops = &radio_ops,
			/* No unused sweep before both radio consumers have probed. */
			.flags = CLK_IGNORE_UNUSED | CLK_GET_RATE_NOCACHE,
		};

		init.name = devm_kasprintf(dev, GFP_KERNEL, "%s-%s", dev_name(dev), names[i]);
		if (!init.name)
			return -ENOMEM;
		outputs[i].owner = clocks;
		outputs[i].index = i;
		outputs[i].hw.init = &init;
		ret = devm_clk_hw_register(dev, &outputs[i].hw);
		if (ret)
			return ret;
		data->hws[i] = &outputs[i].hw;
	}
	/* Hold boot state until DT-linked consumers bind and assert their resets. */
	for (i = 0; i < 2; i++) {
		clocks->boot[i].clk = clk_hw_get_clk(data->hws[i], NULL);
		if (IS_ERR(clocks->boot[i].clk)) {
			ret = PTR_ERR(clocks->boot[i].clk);
			while (i)
				clk_put(clocks->boot[--i].clk);
			return ret;
		}
	}
	ret = devm_add_action_or_reset(dev, radio_boot_release, clocks);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(clocks->boot), clocks->boot);
	if (ret)
		return ret;
	clocks->boot_held = true;
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
	if (!ret) {
		clocks->published = true;
		*result = clocks;
	}
	return ret;
}

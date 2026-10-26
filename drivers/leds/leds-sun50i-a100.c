// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021-2023 Samuel Holland <samuel@sholland.org>
 *
 * Partly based on drivers/leds/leds-turris-omnia.c, which is:
 *     Copyright (c) 2020 by Marek Behún <kabel@kernel.org>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#define LEDC_CTRL_REG			0x0000
#define LEDC_CTRL_REG_DATA_LENGTH		GENMASK(28, 16)
#define LEDC_CTRL_REG_RGB_MODE			GENMASK(8, 6)
#define LEDC_CTRL_REG_LEDC_EN			BIT(0)
#define LEDC_T01_TIMING_CTRL_REG	0x0004
#define LEDC_T01_TIMING_CTRL_REG_T1H		GENMASK(26, 21)
#define LEDC_T01_TIMING_CTRL_REG_T1L		GENMASK(20, 16)
#define LEDC_T01_TIMING_CTRL_REG_T0H		GENMASK(10, 6)
#define LEDC_T01_TIMING_CTRL_REG_T0L		GENMASK(5, 0)
#define LEDC_RESET_TIMING_CTRL_REG	0x000c
#define LEDC_RESET_TIMING_CTRL_REG_TR		GENMASK(28, 16)
#define LEDC_RESET_TIMING_CTRL_REG_LED_NUM	GENMASK(9, 0)
#define LEDC_DATA_REG			0x0014
#define LEDC_DMA_CTRL_REG		0x0018
#define LEDC_DMA_CTRL_REG_DMA_EN		BIT(5)
#define LEDC_DMA_CTRL_REG_FIFO_TRIG_LEVEL	GENMASK(4, 0)
#define LEDC_INT_CTRL_REG		0x001c
#define LEDC_INT_CTRL_REG_GLOBAL_INT_EN		BIT(5)
#define LEDC_INT_CTRL_REG_FIFO_CPUREQ_INT_EN	BIT(1)
#define LEDC_INT_CTRL_REG_TRANS_FINISH_INT_EN	BIT(0)
#define LEDC_INT_STS_REG		0x0020
#define LEDC_INT_STS_REG_FIFO_WLW		GENMASK(15, 10)
#define LEDC_INT_STS_REG_FIFO_CPUREQ_INT	BIT(1)
#define LEDC_INT_STS_REG_TRANS_FINISH_INT	BIT(0)

#define LEDC_FIFO_DEPTH			32U
#define LEDC_MAX_LEDS			1024
#define LEDC_CHANNELS_PER_LED		3 /* RGB */

#define LEDS_TO_BYTES(n)		((n) * sizeof(u32))

struct sun50i_a100_ledc_led {
	struct led_classdev_mc mc_cdev;
	struct mc_subled subled_info[LEDC_CHANNELS_PER_LED];
	u32 addr;
};

#define to_ledc_led(mc) container_of(mc, struct sun50i_a100_ledc_led, mc_cdev)

struct sun50i_a100_ledc_timing {
	u32 t0h_ns;
	u32 t0l_ns;
	u32 t1h_ns;
	u32 t1l_ns;
	u32 treset_ns;
};

struct sun50i_a100_ledc {
	struct device *dev;
	void __iomem *base;
	struct clk *bus_clk;
	struct clk *mod_clk;
	struct reset_control *reset;

	u32 *buffer;
	struct dma_chan *dma_chan;
	dma_addr_t dma_handle;
	unsigned int pio_length;
	unsigned int pio_offset;

	spinlock_t lock;
	unsigned int next_length;
	bool xfer_active;
	bool suspended;
	bool powered;
	bool irq_disabled;
	int irq;

	u32 format;
	struct sun50i_a100_ledc_timing timing;

	u32 max_addr;
	u32 num_leds;
	struct sun50i_a100_ledc_led leds[] __counted_by(num_leds);
};

static int sun50i_a100_ledc_dma_xfer(struct sun50i_a100_ledc *priv, unsigned int length)
{
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;

	desc = dmaengine_prep_slave_single(priv->dma_chan, priv->dma_handle,
					   LEDS_TO_BYTES(length), DMA_MEM_TO_DEV, 0);
	if (!desc)
		return -ENOMEM;

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie))
		return -EIO;

	dma_async_issue_pending(priv->dma_chan);

	return 0;
}

static void sun50i_a100_ledc_pio_xfer(struct sun50i_a100_ledc *priv, unsigned int fifo_used)
{
	unsigned int burst, length, offset;
	u32 control;

	length = priv->pio_length;
	offset = priv->pio_offset;
	burst  = min(length, LEDC_FIFO_DEPTH - fifo_used);

	iowrite32_rep(priv->base + LEDC_DATA_REG, priv->buffer + offset, burst);

	if (burst < length) {
		priv->pio_length = length - burst;
		priv->pio_offset = offset + burst;

		if (!offset) {
			control = readl(priv->base + LEDC_INT_CTRL_REG);
			control |= LEDC_INT_CTRL_REG_FIFO_CPUREQ_INT_EN;
			writel(control, priv->base + LEDC_INT_CTRL_REG);
		}
	} else {
		/* Disable the request IRQ once all data is written. */
		control = readl(priv->base + LEDC_INT_CTRL_REG);
		control &= ~LEDC_INT_CTRL_REG_FIFO_CPUREQ_INT_EN;
		writel(control, priv->base + LEDC_INT_CTRL_REG);
	}
}

static void sun50i_a100_ledc_start_xfer(struct sun50i_a100_ledc *priv, unsigned int length)
{
	bool use_dma = false;
	u32 control;

	if (priv->dma_chan && length > LEDC_FIFO_DEPTH) {
		int ret;

		ret = sun50i_a100_ledc_dma_xfer(priv, length);
		if (ret)
			dev_warn(priv->dev, "Failed to set up DMA (%d), using PIO\n", ret);
		else
			use_dma = true;
	}

	/* The DMA trigger level must be at least the burst length. */
	control = FIELD_PREP(LEDC_DMA_CTRL_REG_DMA_EN, use_dma) |
		  FIELD_PREP_CONST(LEDC_DMA_CTRL_REG_FIFO_TRIG_LEVEL, LEDC_FIFO_DEPTH / 2);
	writel(control, priv->base + LEDC_DMA_CTRL_REG);

	control = readl(priv->base + LEDC_CTRL_REG);
	control &= ~LEDC_CTRL_REG_DATA_LENGTH;
	control |= FIELD_PREP(LEDC_CTRL_REG_DATA_LENGTH, length) | LEDC_CTRL_REG_LEDC_EN;
	writel(control, priv->base + LEDC_CTRL_REG);

	if (!use_dma) {
		/* The FIFO is empty when starting a new transfer. */
		unsigned int fifo_used = 0;

		priv->pio_length = length;
		priv->pio_offset = 0;

		sun50i_a100_ledc_pio_xfer(priv, fifo_used);
	}
}

static irqreturn_t sun50i_a100_ledc_irq(int irq, void *data)
{
	struct sun50i_a100_ledc *priv = data;
	unsigned int next_length = 0;
	u32 status;

	spin_lock(&priv->lock);
	status = readl(priv->base + LEDC_INT_STS_REG);
	/* Acknowledge this transfer before a subsequent transfer can start. */
	writel(status, priv->base + LEDC_INT_STS_REG);
	if (status & LEDC_INT_STS_REG_TRANS_FINISH_INT) {
		next_length = priv->next_length;
		priv->next_length = 0;
		if (next_length)
			sun50i_a100_ledc_start_xfer(priv, next_length);
		else
			priv->xfer_active = false;
	} else if (status & LEDC_INT_STS_REG_FIFO_CPUREQ_INT) {
		sun50i_a100_ledc_pio_xfer(priv,
					  FIELD_GET(LEDC_INT_STS_REG_FIFO_WLW, status));
	}
	spin_unlock(&priv->lock);
	return IRQ_HANDLED;
}

static void sun50i_a100_ledc_brightness_set(struct led_classdev *cdev,
					    enum led_brightness brightness)
{
	struct sun50i_a100_ledc *priv = dev_get_drvdata(cdev->dev->parent);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(cdev);
	struct sun50i_a100_ledc_led *led = to_ledc_led(mc_cdev);
	unsigned int next_length;
	unsigned long flags;

	led_mc_calc_color_components(mc_cdev, brightness);
	spin_lock_irqsave(&priv->lock, flags);
	priv->buffer[led->addr] = led->subled_info[0].brightness << 16 |
				  led->subled_info[1].brightness << 8 |
				  led->subled_info[2].brightness;
	if (priv->suspended)
		goto out;
	next_length = max(priv->next_length, led->addr + 1);
	if (priv->xfer_active) {
		priv->next_length = next_length;
	} else {
		priv->xfer_active = true;
		sun50i_a100_ledc_start_xfer(priv, next_length);
	}
out:
	spin_unlock_irqrestore(&priv->lock, flags);
}

static const char *const sun50i_a100_ledc_formats[] = {
	"rgb", "rbg", "grb", "gbr", "brg", "bgr",
};

static int sun50i_a100_ledc_parse_format(struct device *dev,
					 struct sun50i_a100_ledc *priv)
{
	const char *format = "grb";
	int i;

	device_property_read_string(dev, "allwinner,pixel-format", &format);

	i = match_string(sun50i_a100_ledc_formats, ARRAY_SIZE(sun50i_a100_ledc_formats), format);
	if (i < 0)
		return dev_err_probe(dev, i, "Bad pixel format '%s'\n", format);

	priv->format = i;
	return 0;
}

static void sun50i_a100_ledc_set_format(struct sun50i_a100_ledc *priv)
{
	u32 control;

	control = readl(priv->base + LEDC_CTRL_REG);
	control &= ~LEDC_CTRL_REG_RGB_MODE;
	control |= FIELD_PREP(LEDC_CTRL_REG_RGB_MODE, priv->format);
	writel(control, priv->base + LEDC_CTRL_REG);
}

static const struct sun50i_a100_ledc_timing sun50i_a100_ledc_default_timing = {
	.t0h_ns = 336,
	.t0l_ns = 840,
	.t1h_ns = 882,
	.t1l_ns = 294,
	.treset_ns = 300000,
};

static int sun50i_a100_ledc_parse_timing(struct device *dev,
					 struct sun50i_a100_ledc *priv)
{
	struct sun50i_a100_ledc_timing *timing = &priv->timing;

	*timing = sun50i_a100_ledc_default_timing;

	device_property_read_u32(dev, "allwinner,t0h-ns", &timing->t0h_ns);
	device_property_read_u32(dev, "allwinner,t0l-ns", &timing->t0l_ns);
	device_property_read_u32(dev, "allwinner,t1h-ns", &timing->t1h_ns);
	device_property_read_u32(dev, "allwinner,t1l-ns", &timing->t1l_ns);
	device_property_read_u32(dev, "allwinner,treset-ns", &timing->treset_ns);

	return 0;
}

static u32 sun50i_a100_ledc_ns_to_cycles(u32 timing_ns,
					 unsigned long mod_freq, u32 mask,
					 bool round_up)
{
	u64 cycles = (u64)timing_ns * mod_freq;

	if (round_up)
		cycles = DIV_ROUND_UP_ULL(cycles, NSEC_PER_SEC);
	else
		cycles = div_u64(cycles, NSEC_PER_SEC);

	/* Zero has a special meaning; timing fields store cycles minus one. */
	cycles = clamp_t(u64, cycles, 2, (u64)FIELD_MAX(mask) + 1);

	return cycles - 1;
}

static int sun50i_a100_ledc_set_timing(struct sun50i_a100_ledc *priv)
{
	const struct sun50i_a100_ledc_timing *timing = &priv->timing;
	unsigned long mod_freq = clk_get_rate(priv->mod_clk);
	u64 reset_cycles;
	u32 t0h, t0l, t1h, t1l, treset;
	u32 control;

	if (!mod_freq)
		return -EINVAL;

	reset_cycles = DIV_ROUND_UP_ULL((u64)timing->treset_ns * mod_freq,
					NSEC_PER_SEC);
	if (reset_cycles > FIELD_MAX(LEDC_RESET_TIMING_CTRL_REG_TR) + 1)
		return dev_err_probe(priv->dev, -ERANGE,
				     "reset timing %u ns is not representable at %lu Hz\n",
				     timing->treset_ns, mod_freq);

	t1h = sun50i_a100_ledc_ns_to_cycles(timing->t1h_ns, mod_freq,
					    LEDC_T01_TIMING_CTRL_REG_T1H, false);
	t1l = sun50i_a100_ledc_ns_to_cycles(timing->t1l_ns, mod_freq,
					    LEDC_T01_TIMING_CTRL_REG_T1L, false);
	t0h = sun50i_a100_ledc_ns_to_cycles(timing->t0h_ns, mod_freq,
					    LEDC_T01_TIMING_CTRL_REG_T0H, false);
	t0l = sun50i_a100_ledc_ns_to_cycles(timing->t0l_ns, mod_freq,
					    LEDC_T01_TIMING_CTRL_REG_T0L, false);
	treset = sun50i_a100_ledc_ns_to_cycles(timing->treset_ns, mod_freq,
					       LEDC_RESET_TIMING_CTRL_REG_TR, true);

	control = FIELD_PREP(LEDC_T01_TIMING_CTRL_REG_T1H, t1h) |
		  FIELD_PREP(LEDC_T01_TIMING_CTRL_REG_T1L, t1l) |
		  FIELD_PREP(LEDC_T01_TIMING_CTRL_REG_T0H, t0h) |
		  FIELD_PREP(LEDC_T01_TIMING_CTRL_REG_T0L, t0l);
	writel(control, priv->base + LEDC_T01_TIMING_CTRL_REG);

	control = FIELD_PREP(LEDC_RESET_TIMING_CTRL_REG_TR, treset) |
		  FIELD_PREP(LEDC_RESET_TIMING_CTRL_REG_LED_NUM, priv->max_addr);
	writel(control, priv->base + LEDC_RESET_TIMING_CTRL_REG);

	return 0;
}

static int sun50i_a100_ledc_resume(struct device *dev)
{
	struct sun50i_a100_ledc *priv = dev_get_drvdata(dev);
	unsigned long flags;
	int ret;

	if (priv->powered)
		return priv->suspended ? -EBUSY : 0;

	ret = reset_control_deassert(priv->reset);
	if (ret)
		return ret;

	ret = clk_prepare_enable(priv->bus_clk);
	if (ret)
		goto err_assert_reset;

	ret = clk_prepare_enable(priv->mod_clk);
	if (ret)
		goto err_disable_bus_clk;

	ret = sun50i_a100_ledc_set_timing(priv);
	if (ret)
		goto err_disable_mod_clk;

	sun50i_a100_ledc_set_format(priv);
	priv->powered = true;

	/* Clear stale status before replay, while the IRQ is still disabled. */
	writel(readl(priv->base + LEDC_INT_STS_REG),
	       priv->base + LEDC_INT_STS_REG);
	writel(LEDC_INT_CTRL_REG_GLOBAL_INT_EN | LEDC_INT_CTRL_REG_TRANS_FINISH_INT_EN,
	       priv->base + LEDC_INT_CTRL_REG);

	spin_lock_irqsave(&priv->lock, flags);
	priv->suspended = false;
	priv->next_length = 0;
	priv->xfer_active = true;
	sun50i_a100_ledc_start_xfer(priv, priv->max_addr + 1);
	spin_unlock_irqrestore(&priv->lock, flags);
	if (priv->irq_disabled) {
		priv->irq_disabled = false;
		enable_irq(priv->irq);
	}
	return 0;

err_disable_mod_clk:
	clk_disable_unprepare(priv->mod_clk);
err_disable_bus_clk:
	clk_disable_unprepare(priv->bus_clk);
err_assert_reset:
	reset_control_assert(priv->reset);

	return ret;
}

static bool sun50i_a100_ledc_busy(struct sun50i_a100_ledc *priv)
{
	unsigned long flags;
	bool busy;

	spin_lock_irqsave(&priv->lock, flags);
	busy = priv->xfer_active;
	spin_unlock_irqrestore(&priv->lock, flags);
	return busy;
}

static int sun50i_a100_ledc_suspend(struct device *dev)
{
	struct sun50i_a100_ledc *priv = dev_get_drvdata(dev);
	unsigned long flags;
	bool busy;
	int ret;

	if (!priv->powered)
		return 0;

	spin_lock_irqsave(&priv->lock, flags);
	priv->suspended = true;
	spin_unlock_irqrestore(&priv->lock, flags);
	ret = read_poll_timeout(sun50i_a100_ledc_busy, busy, !busy,
				1000, 1000000, false, priv);
	if (ret) {
		/* Failed suspend leaves the controller powered and owned. */
		spin_lock_irqsave(&priv->lock, flags);
		priv->suspended = false;
		/* Include all updates buffered while submissions were gated. */
		if (priv->xfer_active) {
			priv->next_length = priv->max_addr + 1;
		} else {
			priv->next_length = 0;
			priv->xfer_active = true;
			sun50i_a100_ledc_start_xfer(priv, priv->max_addr + 1);
		}
		spin_unlock_irqrestore(&priv->lock, flags);
		return ret;
	}
	disable_irq(priv->irq);
	priv->irq_disabled = true;
	writel(0, priv->base + LEDC_INT_CTRL_REG);
	readl(priv->base + LEDC_INT_CTRL_REG);
	clk_disable_unprepare(priv->mod_clk);
	clk_disable_unprepare(priv->bus_clk);
	reset_control_assert(priv->reset);
	priv->powered = false;
	return 0;
}

/* Caller has excluded DMA; quiesce PIO even if completion was lost. */
static void sun50i_a100_ledc_force_pio_stop(struct sun50i_a100_ledc *priv)
{
	unsigned long flags;
	u32 control;
	int ret;

	spin_lock_irqsave(&priv->lock, flags);
	priv->suspended = true;
	spin_unlock_irqrestore(&priv->lock, flags);
	if (!priv->powered)
		return;

	if (!priv->irq_disabled) {
		disable_irq(priv->irq);
		priv->irq_disabled = true;
	}
	writel(0, priv->base + LEDC_INT_CTRL_REG);
	readl(priv->base + LEDC_INT_CTRL_REG);
	control = readl(priv->base + LEDC_CTRL_REG);
	writel(control & ~LEDC_CTRL_REG_LEDC_EN, priv->base + LEDC_CTRL_REG);
	readl(priv->base + LEDC_CTRL_REG);
	ret = reset_control_assert(priv->reset);
	if (ret)
		dev_warn(priv->dev, "PIO stopped without reset: %d\n", ret);
	/* PIO has no bus master; masked/synchronized IRQs cannot touch memory. */
	clk_disable_unprepare(priv->mod_clk);
	clk_disable_unprepare(priv->bus_clk);
	priv->powered = false;
	spin_lock_irqsave(&priv->lock, flags);
	priv->xfer_active = false;
	priv->next_length = 0;
	spin_unlock_irqrestore(&priv->lock, flags);
}

static void sun50i_a100_ledc_stop(struct device *dev)
{
	struct sun50i_a100_ledc *priv = dev_get_drvdata(dev);

	while (sun50i_a100_ledc_suspend(dev)) {
		if (!priv->dma_chan) {
			/* No DMA or callbacks remain once the PIO IRQ is quiesced. */
			sun50i_a100_ledc_force_pio_stop(priv);
			return;
		}
		/* Preserve the original DMA drain policy until its stop is proven. */
		usleep_range(1000, 1100);
	}
}

static void sun50i_a100_ledc_dma_cleanup(void *data)
{
	struct sun50i_a100_ledc *priv = data;

	dma_release_channel(priv->dma_chan);
}

static int sun50i_a100_ledc_probe(struct platform_device *pdev)
{
	struct dma_slave_config dma_cfg = {};
	struct led_init_data init_data = {};
	struct sun50i_a100_ledc_led *led;
	struct device *dev = &pdev->dev;
	struct sun50i_a100_ledc *priv;
	struct resource *mem;
	u32 max_addr = 0;
	u32 num_leds = 0;
	int irq, ret;

	/*
	 * The maximum LED address must be known in sun50i_a100_ledc_resume() before
	 * class device registration, so parse and validate the subnodes up front.
	 */
	device_for_each_child_node_scoped(dev, child) {
		u32 addr, color;

		ret = fwnode_property_read_u32(child, "reg", &addr);
		if (ret || addr >= LEDC_MAX_LEDS)
			return dev_err_probe(dev, -EINVAL, "'reg' must be between 0 and %d\n",
					     LEDC_MAX_LEDS - 1);

		ret = fwnode_property_read_u32(child, "color", &color);
		if (ret || color != LED_COLOR_ID_RGB)
			return dev_err_probe(dev, -EINVAL, "'color' must be LED_COLOR_ID_RGB\n");

		max_addr = max(max_addr, addr);
		num_leds++;
	}

	if (!num_leds)
		return -ENODEV;

	priv = devm_kzalloc(dev, struct_size(priv, leds, num_leds), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->suspended = true;
	priv->irq_disabled = true;
	priv->max_addr = max_addr;
	priv->num_leds = num_leds;
	spin_lock_init(&priv->lock);
	dev_set_drvdata(dev, priv);

	ret = sun50i_a100_ledc_parse_format(dev, priv);
	if (ret)
		return ret;

	ret = sun50i_a100_ledc_parse_timing(dev, priv);
	if (ret)
		return ret;

	priv->base = devm_platform_get_and_ioremap_resource(pdev, 0, &mem);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->bus_clk = devm_clk_get(dev, "bus");
	if (IS_ERR(priv->bus_clk))
		return PTR_ERR(priv->bus_clk);

	priv->mod_clk = devm_clk_get(dev, "mod");
	if (IS_ERR(priv->mod_clk))
		return PTR_ERR(priv->mod_clk);

	priv->reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(priv->reset))
		return PTR_ERR(priv->reset);

	priv->dma_chan = dma_request_chan(dev, "tx");
	if (IS_ERR(priv->dma_chan)) {
		if (PTR_ERR(priv->dma_chan) != -ENODEV)
			return PTR_ERR(priv->dma_chan);

		priv->dma_chan = NULL;

		priv->buffer = devm_kzalloc(dev, LEDS_TO_BYTES(LEDC_MAX_LEDS), GFP_KERNEL);
		if (!priv->buffer)
			return -ENOMEM;
	} else {
		ret = devm_add_action_or_reset(dev, sun50i_a100_ledc_dma_cleanup, priv);
		if (ret)
			return ret;

		dma_cfg.dst_addr	= mem->start + LEDC_DATA_REG;
		dma_cfg.dst_addr_width	= DMA_SLAVE_BUSWIDTH_4_BYTES;
		dma_cfg.dst_maxburst	= LEDC_FIFO_DEPTH / 2;

		ret = dmaengine_slave_config(priv->dma_chan, &dma_cfg);
		if (ret)
			return ret;

		priv->buffer = dmam_alloc_attrs(dmaengine_get_dma_device(priv->dma_chan),
						LEDS_TO_BYTES(LEDC_MAX_LEDS), &priv->dma_handle,
						GFP_KERNEL, DMA_ATTR_WRITE_COMBINE);
		if (!priv->buffer)
			return -ENOMEM;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	priv->irq = irq;
	ret = devm_request_irq(dev, irq, sun50i_a100_ledc_irq, IRQF_NO_AUTOEN, dev_name(dev), priv);
	if (ret)
		return ret;

	ret = sun50i_a100_ledc_resume(dev);
	if (ret)
		return ret;

	led = priv->leds;
	device_for_each_child_node_scoped(dev, child) {
		struct led_classdev *cdev;

		/* The node was already validated above. */
		fwnode_property_read_u32(child, "reg", &led->addr);

		led->subled_info[0].color_index = LED_COLOR_ID_RED;
		led->subled_info[0].channel = 0;
		led->subled_info[1].color_index = LED_COLOR_ID_GREEN;
		led->subled_info[1].channel = 1;
		led->subled_info[2].color_index = LED_COLOR_ID_BLUE;
		led->subled_info[2].channel = 2;

		led->mc_cdev.num_colors = ARRAY_SIZE(led->subled_info);
		led->mc_cdev.subled_info = led->subled_info;

		cdev = &led->mc_cdev.led_cdev;
		cdev->max_brightness = U8_MAX;
		cdev->brightness_set = sun50i_a100_ledc_brightness_set;

		init_data.fwnode = child;

		ret = led_classdev_multicolor_register_ext(dev, &led->mc_cdev, &init_data);
		if (ret) {
			dev_err_probe(dev, ret, "Failed to register multicolor LED %u", led->addr);
			while (led-- > priv->leds)
				led_classdev_multicolor_unregister(&led->mc_cdev);
			sun50i_a100_ledc_stop(&pdev->dev);

			return ret;
		}

		led++;
	}

	dev_info(dev, "Registered %u LEDs\n", num_leds);

	return 0;
}

static void sun50i_a100_ledc_remove(struct platform_device *pdev)
{
	struct sun50i_a100_ledc *priv = platform_get_drvdata(pdev);

	for (u32 i = 0; i < priv->num_leds; i++)
		led_classdev_multicolor_unregister(&priv->leds[i].mc_cdev);
	sun50i_a100_ledc_stop(&pdev->dev);
}

static const struct of_device_id sun50i_a100_ledc_of_match[] = {
	{ .compatible = "allwinner,sun50i-a100-ledc" },
	{}
};
MODULE_DEVICE_TABLE(of, sun50i_a100_ledc_of_match);

static DEFINE_SIMPLE_DEV_PM_OPS(sun50i_a100_ledc_pm,
				sun50i_a100_ledc_suspend,
				sun50i_a100_ledc_resume);

static struct platform_driver sun50i_a100_ledc_driver = {
	.probe		= sun50i_a100_ledc_probe,
	.remove		= sun50i_a100_ledc_remove,
	.shutdown	= sun50i_a100_ledc_remove,
	.driver		= {
		.name		= "sun50i-a100-ledc",
		.of_match_table	= sun50i_a100_ledc_of_match,
		.pm		= pm_ptr(&sun50i_a100_ledc_pm),
	},
};
module_platform_driver(sun50i_a100_ledc_driver);

MODULE_AUTHOR("Samuel Holland <samuel@sholland.org>");
MODULE_DESCRIPTION("Allwinner A100 LED controller driver");
MODULE_LICENSE("GPL");

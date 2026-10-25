// SPDX-License-Identifier: GPL-2.0+
/* Copyright (c) 2018 Jernej Skrabec <jernej.skrabec@siol.net> */


#include <linux/bitfield.h>
#include <linux/component.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/sun8i-tcon-top.h>

#include <drm/drm_device.h>

#include "sun4i_drv.h"
#include "sun8i_tcon_top.h"

struct sun8i_tcon_top_quirks {
	bool has_tcon_tv0;
	bool early_clock_provider;
	bool has_tcon_tv1;
	bool has_dsi;
};

static bool sun8i_tcon_top_node_is_tcon_top(struct device_node *node)
{
	return !!of_match_node(sun8i_tcon_top_of_table, node);
}

static unsigned int sun8i_tcon_top_get_tcon_map(struct device_node *node)
{
	static const u32 out_ports[] = {
		TCON_TOP_MIXER0_OUT_PORT,
		TCON_TOP_MIXER1_OUT_PORT,
	};
	unsigned int i, map = 0;

	for (i = 0; i < ARRAY_SIZE(out_ports); i++) {
		struct device_node *port;

		port = of_graph_get_port_by_id(node, out_ports[i]);
		if (!port)
			continue;

		for_each_of_graph_port_endpoint(port, ep) {
			struct of_endpoint endpoint;

			if (of_graph_parse_endpoint(ep, &endpoint))
				continue;

			if (endpoint.id < TCON_TOP_PORT_TCON_NUM)
				map |= BIT(endpoint.id);
		}

		of_node_put(port);
	}

	return map;
}

static unsigned int sun8i_tcon_top_park_index(struct sun8i_tcon_top *tcon_top,
					      int tcon)
{
	unsigned int candidates;

	candidates = tcon_top->tcon_map & ~BIT(tcon);
	if (!candidates)
		candidates = GENMASK(TCON_TOP_PORT_TCON_NUM - 1, 0) & ~BIT(tcon);

	return ffs(candidates) - 1;
}

int sun8i_tcon_top_set_hdmi_src(struct device *dev, int tcon)
{
	struct sun8i_tcon_top *tcon_top = dev_get_drvdata(dev);
	const struct sun8i_tcon_top_quirks *quirks;
	unsigned long flags;
	u32 val;

	if (!sun8i_tcon_top_node_is_tcon_top(dev->of_node)) {
		dev_err(dev, "Device is not TCON TOP!\n");
		return -EINVAL;
	}

	quirks = of_device_get_match_data(dev);
	if (!quirks->has_tcon_tv0)
		return -EOPNOTSUPP;

	if (tcon < 2 || tcon > 3) {
		dev_err(dev, "TCON index must be 2 or 3!\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&tcon_top->reg_lock, flags);

	val = readl(tcon_top->regs + TCON_TOP_GATE_SRC_REG);
	val &= ~TCON_TOP_HDMI_SRC_MSK;
	val |= FIELD_PREP(TCON_TOP_HDMI_SRC_MSK, tcon - 1);
	writel(val, tcon_top->regs + TCON_TOP_GATE_SRC_REG);

	spin_unlock_irqrestore(&tcon_top->reg_lock, flags);

	return 0;
}
EXPORT_SYMBOL(sun8i_tcon_top_set_hdmi_src);

int sun8i_tcon_top_de_config(struct device *dev, int mixer, int tcon)
{
	struct sun8i_tcon_top *tcon_top = dev_get_drvdata(dev);
	u32 mixer_msk, other_msk;
	unsigned long flags;
	u32 reg;
	bool a133 = of_device_is_compatible(dev->of_node,
					 "allwinner,sun50i-a133-tcon-top");

	if (!sun8i_tcon_top_node_is_tcon_top(dev->of_node)) {
		dev_err(dev, "Device is not TCON TOP!\n");
		return -EINVAL;
	}

	/* Only DE0/TCON0 is source-correlated for this experiment. Preserve
	 * the other routing field: no second-mixer topology is qualified.
	 */
	if (a133 && (mixer != 0 || tcon != 0))
		return -EINVAL;

	if (mixer > 1) {
		dev_err(dev, "Mixer index is too high!\n");
		return -EINVAL;
	}

	if (tcon < 0 || tcon >= TCON_TOP_PORT_TCON_NUM) {
		dev_err(dev, "TCON index is invalid!\n");
		return -EINVAL;
	}

	mixer_msk = mixer ? TCON_TOP_PORT_DE1_MSK : TCON_TOP_PORT_DE0_MSK;
	other_msk = mixer ? TCON_TOP_PORT_DE0_MSK : TCON_TOP_PORT_DE1_MSK;

	spin_lock_irqsave(&tcon_top->reg_lock, flags);

	reg = readl(tcon_top->regs + TCON_TOP_PORT_SEL_REG);

	reg &= ~mixer_msk;
	reg |= field_prep(mixer_msk, tcon);

	if (!a133 && field_get(other_msk, reg) == (u32)tcon) {
		reg &= ~other_msk;
		reg |= field_prep(other_msk,
				  sun8i_tcon_top_park_index(tcon_top, tcon));
	}

	writel(reg, tcon_top->regs + TCON_TOP_PORT_SEL_REG);

	spin_unlock_irqrestore(&tcon_top->reg_lock, flags);

	return 0;
}
EXPORT_SYMBOL(sun8i_tcon_top_de_config);


static struct clk_hw *sun8i_tcon_top_register_gate(struct device *dev,
						   const char *parent,
						   void __iomem *regs,
						   spinlock_t *lock,
						   u8 bit, int name_index)
{
	const char *clk_name, *parent_name;
	int ret, index;

	index = of_property_match_string(dev->of_node, "clock-names", parent);
	if (index < 0)
		return ERR_PTR(index);

	parent_name = of_clk_get_parent_name(dev->of_node, index);

	ret = of_property_read_string_index(dev->of_node,
					    "clock-output-names", name_index,
					    &clk_name);
	if (ret)
		return ERR_PTR(ret);

	return clk_hw_register_gate(dev, clk_name, parent_name,
				    CLK_SET_RATE_PARENT,
				    regs + TCON_TOP_GATE_SRC_REG,
				    bit, 0, lock);
};

static int sun8i_tcon_top_init(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct clk_hw_onecell_data *clk_data;
	struct sun8i_tcon_top *tcon_top;
	const struct sun8i_tcon_top_quirks *quirks;
	void __iomem *regs;
	int ret, i;

	quirks = of_device_get_match_data(&pdev->dev);

	tcon_top = devm_kzalloc(dev, sizeof(*tcon_top), GFP_KERNEL);
	if (!tcon_top)
		return -ENOMEM;

	clk_data = devm_kzalloc(dev, struct_size(clk_data, hws, CLK_NUM),
				GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;
	clk_data->num = CLK_NUM;
	tcon_top->clk_data = clk_data;
	tcon_top->tcon_map = sun8i_tcon_top_get_tcon_map(dev->of_node);

	spin_lock_init(&tcon_top->reg_lock);

	tcon_top->rst = devm_reset_control_get(dev, NULL);
	if (IS_ERR(tcon_top->rst)) {
		dev_err(dev, "Couldn't get our reset line\n");
		return PTR_ERR(tcon_top->rst);
	}

	tcon_top->bus = devm_clk_get(dev, "bus");
	if (IS_ERR(tcon_top->bus)) {
		dev_err(dev, "Couldn't get the bus clock\n");
		return PTR_ERR(tcon_top->bus);
	}

	regs = devm_platform_ioremap_resource(pdev, 0);
	tcon_top->regs = regs;
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	ret = reset_control_deassert(tcon_top->rst);
	if (ret) {
		dev_err(dev, "Could not deassert ctrl reset control\n");
		return ret;
	}

	ret = clk_prepare_enable(tcon_top->bus);
	if (ret) {
		dev_err(dev, "Could not enable bus clock\n");
		goto err_assert_reset;
	}

	writel(0, regs + TCON_TOP_PORT_SEL_REG);
	writel(0, regs + TCON_TOP_GATE_SRC_REG);

	/*
	 * TCON TOP has two muxes, which select parent clock for each TCON TV
	 * channel clock. Parent could be either TCON TV or TVE clock. For now
	 * we leave this fixed to TCON TV, since TVE driver for R40 is not yet
	 * implemented. Once it is, graph needs to be traversed to determine
	 * if TVE is active on each TCON TV. If it is, mux should be switched
	 * to TVE clock parent.
	 */
	i = 0;
	if (quirks->has_tcon_tv0)
		clk_data->hws[CLK_TCON_TOP_TV0] =
			sun8i_tcon_top_register_gate(dev, "tcon-tv0", regs,
						     &tcon_top->reg_lock,
						     TCON_TOP_TCON_TV0_GATE, i++);

	if (quirks->has_tcon_tv1)
		clk_data->hws[CLK_TCON_TOP_TV1] =
			sun8i_tcon_top_register_gate(dev, "tcon-tv1", regs,
						     &tcon_top->reg_lock,
						     TCON_TOP_TCON_TV1_GATE, i++);

	if (quirks->has_dsi)
		clk_data->hws[CLK_TCON_TOP_DSI] =
			sun8i_tcon_top_register_gate(dev, "dsi", regs,
						     &tcon_top->reg_lock,
						     TCON_TOP_TCON_DSI_GATE, i++);

	for (i = 0; i < CLK_NUM; i++)
		if (IS_ERR(clk_data->hws[i])) {
			ret = PTR_ERR(clk_data->hws[i]);
			goto err_unregister_gates;
		}

	ret = of_clk_add_hw_provider(dev->of_node, of_clk_hw_onecell_get,
				     clk_data);
	if (ret)
		goto err_unregister_gates;

	dev_set_drvdata(dev, tcon_top);

	return 0;

err_unregister_gates:
	for (i = 0; i < CLK_NUM; i++)
		if (!IS_ERR_OR_NULL(clk_data->hws[i]))
			clk_hw_unregister_gate(clk_data->hws[i]);
	clk_disable_unprepare(tcon_top->bus);
err_assert_reset:
	reset_control_assert(tcon_top->rst);

	return ret;
}

static void sun8i_tcon_top_fini(struct device *dev)
{
	struct sun8i_tcon_top *tcon_top = dev_get_drvdata(dev);
	struct clk_hw_onecell_data *clk_data = tcon_top->clk_data;
	int i;

	of_clk_del_provider(dev->of_node);
	for (i = 0; i < CLK_NUM; i++)
		if (clk_data->hws[i])
			clk_hw_unregister_gate(clk_data->hws[i]);

	clk_disable_unprepare(tcon_top->bus);
	reset_control_assert(tcon_top->rst);
}

static int sun8i_tcon_top_sleep_save(struct device *dev)
{
	struct sun8i_tcon_top *top = dev_get_drvdata(dev);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&top->reg_lock, flags);
	if (readl(top->regs + TCON_TOP_GATE_SRC_REG) & BIT(TCON_TOP_TCON_DSI_GATE))
		ret = -EBUSY;
	top->sleep_route = readl(top->regs + TCON_TOP_PORT_SEL_REG) &
			   TCON_TOP_PORT_DE0_MSK;
	spin_unlock_irqrestore(&top->reg_lock, flags);
	return ret;
}

static int sun8i_tcon_top_deep_restore(struct device *dev)
{
	struct sun8i_tcon_top *top = dev_get_drvdata(dev);
	unsigned long flags;
	u32 val;
	int ret;

	ret = reset_control_status(top->rst);
	if (ret)
		return ret < 0 ? ret : -EIO;
	if (!__clk_is_enabled(top->bus))
		return -EIO;
	spin_lock_irqsave(&top->reg_lock, flags);
	/* DSI has dropped its gate reference; do not revive scanout here. */
	val = readl(top->regs + TCON_TOP_GATE_SRC_REG);
	writel(val & ~BIT(TCON_TOP_TCON_DSI_GATE), top->regs + TCON_TOP_GATE_SRC_REG);
	val = readl(top->regs + TCON_TOP_PORT_SEL_REG);
	writel((val & ~TCON_TOP_PORT_DE0_MSK) | top->sleep_route,
	       top->regs + TCON_TOP_PORT_SEL_REG);
	ret = ((readl(top->regs + TCON_TOP_PORT_SEL_REG) & TCON_TOP_PORT_DE0_MSK) !=
	       top->sleep_route ||
	       (readl(top->regs + TCON_TOP_GATE_SRC_REG) & BIT(TCON_TOP_TCON_DSI_GATE))) ?
	      -EIO : 0;
	spin_unlock_irqrestore(&top->reg_lock, flags);
	return ret;
}

static int sun8i_tcon_top_bind(struct device *dev, struct device *master,
			       void *data)
{
	const struct sun8i_tcon_top_quirks *quirks = of_device_get_match_data(dev);
	struct sun4i_drv *drv = ((struct drm_device *)data)->dev_private;
	int ret = quirks->early_clock_provider ? 0 : sun8i_tcon_top_init(dev);

	if (!ret) {
		struct sun8i_tcon_top *top = dev_get_drvdata(dev);

		top->drv = drv;
	}
	if (!ret && of_machine_is_compatible("trimui,brick") &&
	    of_device_is_compatible(dev->of_node, "allwinner,sun50i-a133-tcon-top"))
		drv->sleep[SUN4I_SLEEP_TOP] = (struct sun4i_sleep_client) {
			.dev = dev, .save = sun8i_tcon_top_sleep_save,
			.restore = sun8i_tcon_top_deep_restore,
		};
	return ret;
}

static void sun8i_tcon_top_unbind(struct device *dev, struct device *master,
				  void *data)
{
	const struct sun8i_tcon_top_quirks *quirks = of_device_get_match_data(dev);
	struct sun8i_tcon_top *top = dev_get_drvdata(dev);
	struct sun4i_drv *drv = top->drv;

	if (drv->sleep[SUN4I_SLEEP_TOP].dev == dev)
		drv->sleep[SUN4I_SLEEP_TOP] = (struct sun4i_sleep_client) { };
	if (!quirks->early_clock_provider)
		sun8i_tcon_top_fini(dev);
}

static const struct component_ops sun8i_tcon_top_ops = {
	.bind = sun8i_tcon_top_bind,
	.unbind = sun8i_tcon_top_unbind,
};

static int sun8i_tcon_top_probe(struct platform_device *pdev)
{
	const struct sun8i_tcon_top_quirks *quirks = of_device_get_match_data(&pdev->dev);
	int ret;

	/* DSI must resolve this clock before the DRM component aggregate binds. */
	if (quirks->early_clock_provider) {
		ret = sun8i_tcon_top_init(&pdev->dev);
		if (ret)
			return ret;
	}
	ret = component_add(&pdev->dev, &sun8i_tcon_top_ops);
	if (ret && quirks->early_clock_provider)
		sun8i_tcon_top_fini(&pdev->dev);
	return ret;
}

static void sun8i_tcon_top_remove(struct platform_device *pdev)
{
	const struct sun8i_tcon_top_quirks *quirks = of_device_get_match_data(&pdev->dev);

	component_del(&pdev->dev, &sun8i_tcon_top_ops);
	if (quirks->early_clock_provider)
		sun8i_tcon_top_fini(&pdev->dev);
}

static const struct sun8i_tcon_top_quirks sun8i_r40_tcon_top_quirks = {
	.has_tcon_tv0 = true,
	.has_tcon_tv1	= true,
	.has_dsi	= true,
};

static const struct sun8i_tcon_top_quirks sun20i_d1_tcon_top_quirks = {
	.has_tcon_tv0 = true,
	.has_dsi	= true,
};

static const struct sun8i_tcon_top_quirks sun50i_h6_tcon_top_quirks = {
	.has_tcon_tv0 = true,
};

static const struct sun8i_tcon_top_quirks sun50i_a133_tcon_top_quirks = {
	.has_dsi = true,
	.early_clock_provider = true,
};

/* sun4i_drv uses this list to check if a device node is a TCON TOP */
const struct of_device_id sun8i_tcon_top_of_table[] = {
	{
		.compatible = "allwinner,sun8i-r40-tcon-top",
		.data = &sun8i_r40_tcon_top_quirks
	},
	{
		.compatible = "allwinner,sun20i-d1-tcon-top",
		.data = &sun20i_d1_tcon_top_quirks
	},
	{
		.compatible = "allwinner,sun50i-h6-tcon-top",
		.data = &sun50i_h6_tcon_top_quirks
	},
	{
		.compatible = "allwinner,sun50i-a133-tcon-top",
		.data = &sun50i_a133_tcon_top_quirks
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sun8i_tcon_top_of_table);
EXPORT_SYMBOL(sun8i_tcon_top_of_table);

static struct platform_driver sun8i_tcon_top_platform_driver = {
	.probe		= sun8i_tcon_top_probe,
	.remove		= sun8i_tcon_top_remove,
	.driver		= {
		.name		= "sun8i-tcon-top",
		.of_match_table	= sun8i_tcon_top_of_table,
	},
};
module_platform_driver(sun8i_tcon_top_platform_driver);

MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@siol.net>");
MODULE_DESCRIPTION("Allwinner R40 TCON TOP driver");
MODULE_LICENSE("GPL");

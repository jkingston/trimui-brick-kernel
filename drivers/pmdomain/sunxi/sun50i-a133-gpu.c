// SPDX-License-Identifier: GPL-2.0-only
/* A133 revision 6 GPU domain. Power-off and suspend are not implemented. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/reset.h>

#include "sun50i-a133-gpu-sequence.h"

struct a133_gpu_domain {
	struct generic_pm_domain genpd;
	struct device *dev;
	void __iomem *ppu, *gpu, *dfs;
	struct clk *core, *bus, *ppu_clk, *parent;
	struct reset_control *gpu_reset, *ppu_reset;
	bool attempted;
	int result;
};

static int a133_gpu_power_on(struct generic_pm_domain *genpd)
{
	struct a133_gpu_domain *p = container_of(genpd, struct a133_gpu_domain, genpd);
	u32 value;
	int ret;

	if (p->attempted)
		return p->result;
	/* Once hardware is touched, preserve ownership until reboot even on
	 * failure. Reversing this sequence is not a validated power-off path.
	 */
	p->attempted = true;
	p->result = -EIO;
	ret = reset_control_deassert(p->ppu_reset);
	if (ret)
		goto failed;
	ret = clk_prepare_enable(p->ppu_clk);
	if (ret)
		goto failed;
	if ((readl(p->ppu + 0x20) & 3) != 2) {
		ret = -EALREADY;
		goto failed;
	}
	writel(1, p->ppu + 0x24);
	writel(0, p->ppu + 0x2c);
	brick_ppu_power_on(p->ppu);
	if (!(readl(p->ppu) & 1) || !(readl(p->ppu + 4) & 1) ||
	    (readl(p->ppu + 8) & 3) || (readl(p->ppu + 0x20) & 3)) {
		ret = -EIO;
		goto failed;
	}
	ret = clk_prepare_enable(p->parent);
	if (ret)
		goto failed;
	ret = reset_control_deassert(p->gpu_reset);
	if (ret)
		goto failed;
	ret = clk_prepare_enable(p->bus);
	if (ret)
		goto failed;
	ret = clk_prepare_enable(p->core);
	if (ret)
		goto failed;
	if (clk_get_rate(p->core) != 432000000 ||
	    readq(p->gpu + 0x20) != 0x0016006600360026ULL ||
	    readl(p->dfs) != 1) {
		ret = -ENODEV;
		goto failed;
	}
	/* Transfer to hardware control before the GPU consumer can probe.
	 * Requests remain masked; firmware owns the subsequent GPU activity.
	 */
	value = readl(p->ppu + 0x2c);
	writel(value & ~1U, p->ppu + 0x2c);
	value = readl(p->ppu + 0x24);
	writel((value & ~3U) | 2U, p->ppu + 0x24);
	ret = readl_poll_timeout(p->ppu + 0x24, value, (value & 3) == 2, 1, 1000);
	if (ret)
		goto failed;
	if (readl(p->ppu + 0x2c) & 1) {
		ret = -EIO;
		goto failed;
	}
	p->result = 0;
	dev_info(p->dev, "GPU domain on, hardware control at 432 MHz\n");
	return 0;
failed:
	p->result = ret;
	dev_err(p->dev, "GPU power-on failed: %d; reboot required\n", ret);
	return ret;
}

static int a133_gpu_power_off(struct generic_pm_domain *genpd)
{
	return -EBUSY;
}

static int a133_gpu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct a133_gpu_domain *p;
	void __iomem *revision;
	struct resource *res;
	u32 version;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != 0x07001000 || resource_size(res) != 0x34)
		return -EINVAL;
	revision = ioremap(0x03000024, 4);
	if (!revision)
		return -ENOMEM;
	version = readl(revision);
	iounmap(revision);
	if ((version & 7) != 6)
		return dev_err_probe(dev, -ENODEV, "unsupported silicon revision\n");
	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	p->ppu = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->ppu))
		return PTR_ERR(p->ppu);
	/* Read-only identity checks; the GPU driver owns the GPU register range. */
	p->gpu = devm_ioremap(dev, 0x01800000, 0x28);
	p->dfs = devm_ioremap(dev, 0x01880020, 4);
	if (!p->gpu || !p->dfs)
		return -ENOMEM;
	p->core = devm_clk_get(dev, "core");
	if (IS_ERR(p->core))
		return dev_err_probe(dev, PTR_ERR(p->core), "core clock\n");
	p->bus = devm_clk_get(dev, "bus");
	if (IS_ERR(p->bus))
		return dev_err_probe(dev, PTR_ERR(p->bus), "bus clock\n");
	p->ppu_clk = devm_clk_get(dev, "ppu");
	if (IS_ERR(p->ppu_clk))
		return dev_err_probe(dev, PTR_ERR(p->ppu_clk), "PPU clock\n");
	p->parent = clk_get_parent(p->core);
	if (IS_ERR_OR_NULL(p->parent) || !clk_get_rate(p->parent))
		return -EINVAL;
	p->gpu_reset = devm_reset_control_get_exclusive(dev, "gpu");
	if (IS_ERR(p->gpu_reset))
		return dev_err_probe(dev, PTR_ERR(p->gpu_reset), "GPU reset\n");
	p->ppu_reset = devm_reset_control_get_exclusive(dev, "ppu");
	if (IS_ERR(p->ppu_reset))
		return dev_err_probe(dev, PTR_ERR(p->ppu_reset), "PPU reset\n");
	ret = reset_control_status(p->ppu_reset);
	if (ret != 1)
		return ret < 0 ? ret : -EALREADY;
	ret = reset_control_status(p->gpu_reset);
	if (ret != 1)
		return ret < 0 ? ret : -EALREADY;
	p->genpd.name = "a133-gpu";
	p->genpd.power_on = a133_gpu_power_on;
	p->genpd.power_off = a133_gpu_power_off;
	ret = pm_genpd_init(&p->genpd, NULL, true);
	if (ret)
		return ret;
	ret = of_genpd_add_provider_simple(dev->of_node, &p->genpd);
	if (ret)
		pm_genpd_remove(&p->genpd);
	return ret;
}

static int a133_gpu_suspend_prepare(struct device *dev)
{
	return -EBUSY;
}

static const struct dev_pm_ops a133_gpu_pm_ops = {
	.prepare = a133_gpu_suspend_prepare,
};
static const struct of_device_id a133_gpu_match[] = {
	{ .compatible = "allwinner,sun50i-a133-gpu-power-domain" },
	{ }
};
static struct platform_driver a133_gpu_driver = {
	.probe = a133_gpu_probe,
	.driver = {
		.name = "sun50i-a133-gpu-power-domain",
		.of_match_table = a133_gpu_match,
		.pm = &a133_gpu_pm_ops,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(a133_gpu_driver);
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0-only
/* A133 revision 6 GPU domain with DDK-controlled internal power transitions. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sun50i-a133-gpu.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/reset.h>

#include "sun50i-a133-gpu-sequence.h"
#include "sun50i-a133-gpu-off.h"

enum a133_handoff_state {
	A133_BOOT, A133_HW, A133_SW_HOST, A133_INTERNAL_OFF,
	A133_RESTORING, A133_FAULT
};

struct a133_gpu_domain {
	struct generic_pm_domain genpd;
	struct device *dev;
	void __iomem *ppu, *gpu, *dfs;
	struct clk *core, *bus, *ppu_clk, *parent;
	struct reset_control *gpu_reset, *ppu_reset;
	struct mutex handoff_lock;
	enum a133_handoff_state handoff_state;
	const char *failure_stage;
	bool attempted;
	bool boot_ready;
	int result;
};

/* Built-in single provider; unbind is disabled to preserve attached lifetime. */
static struct a133_gpu_domain *a133_provider;

static int a133_gpu_power_on(struct generic_pm_domain *genpd)
{
	struct a133_gpu_domain *p = container_of(genpd, struct a133_gpu_domain, genpd);
	u32 value;
	int ret;

	if (p->attempted)
		return READ_ONCE(p->result);
	/* Once hardware is touched, preserve ownership until reboot even on
	 * failure. Reversing this sequence is not a validated power-off path.
	 */
	p->attempted = true;
	WRITE_ONCE(p->result, -EIO);
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
	p->handoff_state = A133_HW;
	WRITE_ONCE(p->result, 0);
	/* Publish completed boot initialization to transition callers. */
	smp_store_release(&p->boot_ready, true);
	dev_info(p->dev, "GPU domain on, hardware control at 432 MHz\n");
	return 0;
failed:
	WRITE_ONCE(p->result, ret);
	dev_err(p->dev, "GPU power-on failed: %d; reboot required\n", ret);
	return ret;
}

struct a133_snapshot {
	u32 reset, gate, isolation, status, control, irq, irqmask, dfs;
	unsigned long rate;
};

/* Transition snapshots never read the separately mapped DFS register. */
static struct a133_snapshot a133_ppu_snapshot(struct a133_gpu_domain *p)
{
	struct a133_snapshot s = {0};

	s.reset = readl(p->ppu);
	s.gate = readl(p->ppu + 4);
	s.isolation = readl(p->ppu + 8);
	s.status = readl(p->ppu + 0x20);
	s.control = readl(p->ppu + 0x24);
	s.irq = readl(p->ppu + 0x28);
	s.irqmask = readl(p->ppu + 0x2c);
	return s;
}
static struct a133_snapshot a133_snapshot(struct a133_gpu_domain *p)
{
	struct a133_snapshot s = a133_ppu_snapshot(p);

	s.dfs = readl(p->dfs);
	s.rate = clk_get_rate(p->core);
	return s;
}

static bool a133_handoff_valid(const struct a133_snapshot *s, u32 mode,
	bool host_only)
{
	if (!(s->reset & 1) || !(s->gate & 1) || (s->control & 3) != mode ||
	    (s->irqmask & 1) || s->dfs != 1 || s->rate != 432000000UL)
		return false;
	return ((s->status & 3) == 1 && (s->isolation & 3) == 2) ||
	       (!host_only && !(s->status & 3) && !(s->isolation & 3));
}

/* handoff_lock serializes the first error across all consumer lifetimes. */
static int a133_fault(struct a133_gpu_domain *p, const char *stage, int error)
{
	if (!READ_ONCE(p->result)) {
		p->failure_stage = stage;
		WRITE_ONCE(p->result, error);
		dev_err(p->dev, "GPU power failure at %s: %d; reboot required\n",
			stage, error);
	}
	p->handoff_state = A133_FAULT;
	return READ_ONCE(p->result);
}

int a133_gpu_latch_fault(struct device *consumer, int error)
{
	/* Pair with provider publication after boot resource setup. */
	struct a133_gpu_domain *p = smp_load_acquire(&a133_provider);
	int ret;

	if (!p || !consumer || consumer->pm_domain != &p->genpd.domain)
		return -ENODEV;
	if (error >= 0)
		return -EINVAL;
	mutex_lock(&p->handoff_lock);
	ret = a133_fault(p, "consumer", error);
	mutex_unlock(&p->handoff_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(a133_gpu_latch_fault);

static bool a133_internal_tuple(const struct a133_snapshot *s, bool off)
{
	return (s->reset & 1) == !off && (s->gate & 1) == !off &&
	       (s->isolation & 3) == (off ? 3 : 0) &&
	       (s->status & 3) == (off ? 2 : 0) &&
	       (s->control & 3) == 1 && !(s->irqmask & 1);
}
/* Caller owns DDK power lock, successful same-transaction stop evidence and
 * synchronous IRQ barrier. Returns zero on success, negative error on failure.
 */
int a133_gpu_internal_off(struct device *consumer)
{
	/* Pair with provider publication after boot resource setup. */
	struct a133_gpu_domain *p = smp_load_acquire(&a133_provider);
	struct a133_snapshot s;
	int ret = 0;

	if (!p || !consumer || consumer->pm_domain != &p->genpd.domain)
		return -ENODEV;
	mutex_lock(&p->handoff_lock);
	if (p->handoff_state == A133_FAULT || READ_ONCE(p->result)) {
		ret = READ_ONCE(p->result);
		goto out;
	}
	/* Pair with successful boot initialization publication. */
	if (!smp_load_acquire(&p->boot_ready) ||
	    p->handoff_state != A133_SW_HOST) {
		ret = -EINVAL;
		goto out;
	}
	s = a133_ppu_snapshot(p);
	if (!(s.reset & 1) || !(s.gate & 1) || (s.isolation & 3) != 2 ||
	    (s.status & 3) != 1 || (s.control & 3) != 1 || (s.irqmask & 1)) {
		ret = -EINVAL;
		goto out;
	}
	s = a133_snapshot(p);
	if (!a133_handoff_valid(&s, 1, true)) {
		ret = -EINVAL;
		goto out;
	}
	/* No attempt can be retried after the first destructive operation. */
	brick_ppu_power_off(p->ppu);
	s = a133_ppu_snapshot(p);

	if (!a133_internal_tuple(&s, true)) {
		ret = a133_fault(p, "off-readback", -EIO);
		goto out;
	}
	p->handoff_state = A133_INTERNAL_OFF;
	ret = 0;
out:
	mutex_unlock(&p->handoff_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(a133_gpu_internal_off);

/* DDK and provider locks held. No DFS read until powered PPU tuple validated. */
static int a133_internal_restore(struct a133_gpu_domain *p)
{
	struct a133_snapshot s = a133_ppu_snapshot(p);
	u32 control = s.control, value;
	int ret;

	if (!a133_internal_tuple(&s, true))
		return a133_fault(p, "restore-entry", -EIO);
	p->handoff_state = A133_RESTORING;
	brick_ppu_power_on(p->ppu);
	s = a133_ppu_snapshot(p);
	if (!a133_internal_tuple(&s, false) || s.control != control)
		return a133_fault(p, "restore-readback", -EIO);
	/* External resources were never gated. Internal reset/gate/isolation now
	 * establish accessibility before the separate DFS read.
	 */
	s = a133_snapshot(p);
	if (s.dfs != 1 || s.rate != 432000000UL)
		return a133_fault(p, "restore-clock", -EIO);
	writel((control & ~3U) | 2, p->ppu + 0x24);
	ret = readl_poll_timeout(p->ppu + 0x24, value, (value & 3) == 2, 1, 1000);
	s = a133_ppu_snapshot(p);
	if (ret || !(s.reset & 1) || !(s.gate & 1) ||
	    (s.isolation & 3) || (s.status & 3) || (s.irqmask & 1) ||
	    s.control != ((control & ~3U) | 2))
		return a133_fault(p, "restore-handoff", ret ? ret : -EIO);
	p->handoff_state = A133_HW;
	return 0;
}

int a133_gpu_apm_handoff(struct device *consumer, bool wake, bool forced)
{
	/* Pair with provider publication after boot resource setup. */
	struct a133_gpu_domain *p = smp_load_acquire(&a133_provider);
	struct a133_snapshot before, after;
	u32 target, value;
	int ret = 0;
	const char *reason = "unexpected entry state";

	/* Compare identity before any conversion/dereference of consumer pm_domain.
	 * The built-in provider has no remove and no userspace unbind; genpd attachment
	 * keeps consumer tied to this provider for the DDK callback lifetime.
	 */

	if (!p || !consumer || consumer->pm_domain != &p->genpd.domain)
		return -ENODEV;
	might_sleep();
	mutex_lock(&p->handoff_lock);
	if (p->handoff_state == A133_FAULT) {
		ret = READ_ONCE(p->result);
		goto out;
	}
	if (!p->attempted || READ_ONCE(p->result) || p->handoff_state == A133_BOOT) {
		reason = "boot initialization incomplete";
		goto fault;
	}
	if (p->handoff_state == A133_INTERNAL_OFF) {
		if (!wake) {
			ret = -EINVAL;
			goto out;
		}
		ret = a133_internal_restore(p);
		goto out;
	}
	before = a133_snapshot(p);
	if (p->handoff_state == A133_HW) {
		if (!a133_handoff_valid(&before, 2, false))
			goto fault;
		if (wake)
			goto out;
		if (!(before.status & 3)) {
			if (forced) {
				/* Probe/shutdown forced OFF may leave the physical PPU in 3D.
				 * Do not manufacture HOST or alter its boot transaction.
				 */
				goto out;
			}
			reason = "non-forced OFF did not reach HOST";
			goto fault;
		}
		target = 1;
	} else if (p->handoff_state == A133_SW_HOST) {
		if (!a133_handoff_valid(&before, 1, true))
			goto fault;
		if (!wake)
			goto out;
		target = 2;
	} else {
		goto fault;
	}
	/* Exactly one owned control-field write; preserve all high bits.
	 * IRQ remains masked, and request/status registers are never acknowledged.
	 */
	writel((before.control & ~3U) | target, p->ppu + 0x24);
	ret = readl_poll_timeout(p->ppu + 0x24, value,
				(value & 3) == target, 1, 1000);
	after = a133_snapshot(p);
	if (ret || !a133_handoff_valid(&after, target, true) ||
	    (after.control & ~3U) != (before.control & ~3U)) {
		reason = "control handoff readback mismatch";
		goto fault;
	}
	p->handoff_state = wake ? A133_HW : A133_SW_HOST;
	goto out;
fault:
	ret = a133_fault(p, reason, ret ? ret : -EIO);
out:
	mutex_unlock(&p->handoff_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(a133_gpu_apm_handoff);

/* Sticky fault only, not an accessibility grant during OFF/RESTORING.
 * RAM-only and nonblocking: DDK callers must not recurse into provider locks.
 * Built-in provider/attachment lifetime is the same as the handoff API.
 */
bool a133_gpu_power_fault(struct device *consumer)
{
	/* Pair with provider publication after boot resource setup. */
	struct a133_gpu_domain *p = smp_load_acquire(&a133_provider);

	if (!p || !consumer || consumer->pm_domain != &p->genpd.domain)
		return true;
	/* Pair with successful boot initialization publication. */
	if (!smp_load_acquire(&p->boot_ready))
		return true;
	return READ_ONCE(p->result) != 0;
}
EXPORT_SYMBOL_GPL(a133_gpu_power_fault);

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
	mutex_init(&p->handoff_lock);
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
	/* Publish initialized registry before genpd attachment becomes visible. */
	smp_store_release(&a133_provider, p);
	ret = of_genpd_add_provider_simple(dev->of_node, &p->genpd);
	if (ret) {
		/* Withdraw publication if registration fails. */
		smp_store_release(&a133_provider, NULL);
		pm_genpd_remove(&p->genpd);
	}
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

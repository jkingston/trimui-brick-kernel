// SPDX-License-Identifier: GPL-2.0-only
/* A133 CPU-only clock transactions with an explicit OPP completion result. */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk/sunxi-a133.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "ccu_common.h"
#include "ccu_gate.h"
#include "ccu_mult.h"
#include "ccu_mux.h"
#include "ccu-sun50i-a133-cpufreq.h"

#define A133_N		GENMASK(15, 8)
#define A133_GATE	BIT(27)
#define A133_LOCK	BIT(28)
#define A133_ENABLE	BIT(31)
#define A133_PLL_PARENT	3
#define A133_BYPASS	4
#define A133_BYPASS_RATE	600000000UL

struct a133_cpu_guard {
	struct device *dev;
	struct ccu_common *pll, *cpu;
	struct ccu_mux_internal *mux;
	struct clk *cpu_clk, *bypass_clk;
	struct notifier_block nb;
	unsigned long target;
	unsigned long requested;
	const char *stage;
	u32 saved;
	bool active, faulted;
};

/* Serializes exported transactions against provider removal. */
static DEFINE_MUTEX(a133_cpu_mutex);
static struct a133_cpu_guard *a133_guard;

static bool a133_rate_valid(unsigned long rate)
{
	return rate >= 408000000 && rate <= 1992000000 && !(rate % 24000000);
}

static bool a133_pll_valid(struct a133_cpu_guard *g, unsigned long rate)
{
	u32 value = readl(g->pll->base + g->pll->reg);
	u32 status = A133_ENABLE | A133_GATE | A133_LOCK;

	return (value & status) == status &&
	       (FIELD_GET(A133_N, value) + 1) * 24000000UL == rate &&
	       (value & ~(A133_N | A133_LOCK)) ==
	       (g->saved & ~(A133_N | A133_LOCK));
}

static int a133_mux_set(struct a133_cpu_guard *g, u8 parent)
{
	int ret;

	ret = ccu_mux_helper_set_parent(g->cpu, g->mux, parent);
	udelay(1);
	if (ret)
		return ret;
	return ccu_mux_helper_get_parent(g->cpu, g->mux) == parent ? 0 : -EIO;
}

static void a133_fault(struct a133_cpu_guard *g)
{
	if (!g->faulted)
		dev_err(g->dev,
			"CPU clock fault at %s: requested=%lu target=%lu pll=%#x saved=%#x mux=%#x active=%u\n",
			g->stage, g->requested, g->target,
			readl(g->pll->base + g->pll->reg), g->saved,
			readl(g->cpu->base + g->cpu->reg), g->active);
	g->faulted = true;
	g->active = false;
	if (a133_mux_set(g, A133_BYPASS))
		dev_emerg(g->dev, "CPU bypass selection failed\n");
}

/* CCF ignores set_rate errors. PRE validates; POST and exported API verify. */
int sun50i_a133_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			   unsigned long parent_rate)
{
	struct ccu_common *common = &hw_to_ccu_mult(hw)->common;
	unsigned long flags;
	u32 value;

	if (!a133_guard || !a133_guard->active || a133_guard->faulted ||
	    a133_guard->target != rate)
		return -EPERM;
	if (parent_rate != 24000000 || !a133_rate_valid(rate))
		return -EINVAL;
	spin_lock_irqsave(common->lock, flags);
	value = readl(common->base + common->reg);
	value &= ~A133_N;
	value |= FIELD_PREP(A133_N, rate / 24000000 - 1);
	writel(value, common->base + common->reg);
	spin_unlock_irqrestore(common->lock, flags);
	return 0;
}

static int a133_cpu_notify(struct notifier_block *nb, unsigned long event,
			   void *data)
{
	struct a133_cpu_guard *g = container_of(nb, struct a133_cpu_guard, nb);
	struct clk_notifier_data *change = data;
	u32 value;
	int ret;

	switch (event) {
	case PRE_RATE_CHANGE:
		g->stage = "pre-state";
		if (g->faulted || g->active || !a133_rate_valid(change->new_rate))
			return NOTIFY_BAD;
		g->stage = "pre-pll";
		if (ccu_mux_helper_get_parent(g->cpu, g->mux) != A133_PLL_PARENT ||
		    !a133_pll_valid(g, change->old_rate))
			return NOTIFY_BAD;
		g->saved = readl(g->pll->base + g->pll->reg);
		g->target = change->new_rate;
		g->active = true;
		g->stage = "pre-bypass";
		if (a133_mux_set(g, A133_BYPASS)) {
			a133_fault(g);
			return NOTIFY_BAD;
		}
		return NOTIFY_OK;
	case POST_RATE_CHANGE:
		if (!g->active || g->faulted)
			return NOTIFY_DONE;
		g->stage = "post-lock";
		ccu_gate_helper_disable(g->pll, A133_GATE);
		ret = ccu_gate_helper_enable(g->pll, A133_GATE);
		if (!ret)
			ret = readl_poll_timeout(g->pll->base + g->pll->reg, value,
						value & A133_LOCK, 100, 70000);
		if (ret) {
			a133_fault(g);
			return NOTIFY_BAD;
		}
		g->stage = "post-verify";
		if (!a133_pll_valid(g, g->target) ||
		    a133_mux_set(g, A133_PLL_PARENT)) {
			a133_fault(g);
			return NOTIFY_BAD;
		}
		g->active = false;
		return NOTIFY_OK;
	case ABORT_RATE_CHANGE:
		if (!g->active)
			return NOTIFY_DONE;
		g->stage = "abort-verify";
		value = readl(g->pll->base + g->pll->reg);
		if (g->faulted ||
		    (value & ~A133_LOCK) != (g->saved & ~A133_LOCK) ||
		    !a133_pll_valid(g, change->old_rate) ||
		    a133_mux_set(g, A133_PLL_PARENT)) {
			a133_fault(g);
			return NOTIFY_BAD;
		}
		g->active = false;
		return NOTIFY_OK;
	}
	return NOTIFY_DONE;
}

int sun50i_a133_cpux_is_ready(struct clk *clk)
{
	struct a133_cpu_guard *g;
	int ret = -ENODEV;

	mutex_lock(&a133_cpu_mutex);
	g = a133_guard;
	if (g && clk_is_match(clk, g->cpu_clk)) {
		ret = g->faulted || g->active ||
		      !a133_pll_valid(g, clk_get_rate(g->cpu_clk)) ||
		      ccu_mux_helper_get_parent(g->cpu, g->mux) != A133_PLL_PARENT ?
		      -EIO : 0;
	}
	mutex_unlock(&a133_cpu_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(sun50i_a133_cpux_is_ready);

int sun50i_a133_cpux_set_rate_checked(struct clk *clk, unsigned long rate)
{
	struct a133_cpu_guard *g;
	int ret = -ENODEV;

	mutex_lock(&a133_cpu_mutex);
	g = a133_guard;
	if (!g || !clk_is_match(clk, g->cpu_clk))
		goto out;
	if (g->faulted) {
		ret = -EIO;
		goto out;
	}
	if (!a133_rate_valid(rate)) {
		ret = -EINVAL;
		goto out;
	}
	/* Only this clock handle owns the exclusive rate-control reference. */
	g->requested = rate;
	g->stage = "set-rate";
	ret = clk_set_rate(g->cpu_clk, rate);
	if (ret || g->faulted || g->active || !a133_pll_valid(g, rate) ||
	    ccu_mux_helper_get_parent(g->cpu, g->mux) != A133_PLL_PARENT) {
		a133_fault(g);
		/* Outside CCF notifier: reconcile the cached parent with hardware. */
		if (clk_set_parent(g->cpu_clk, g->bypass_clk))
			dev_err(g->dev, "CPU bypass cache reconciliation failed\n");
		ret = -EIO;
	}
out:
	mutex_unlock(&a133_cpu_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(sun50i_a133_cpux_set_rate_checked);

static void a133_cpu_unregister(void *data)
{
	struct a133_cpu_guard *g = data;

	mutex_lock(&a133_cpu_mutex);
	a133_guard = NULL;
	clk_notifier_unregister(g->pll->hw.clk, &g->nb);
	mutex_unlock(&a133_cpu_mutex);
}

int sun50i_a133_cpufreq_register(struct device *dev, struct ccu_common *pll,
			       struct ccu_common *cpu,
			       struct ccu_mux_internal *mux,
			       struct clk_hw *bypass)
{
	struct a133_cpu_guard *g;
	int ret;

	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;
	g->dev = dev;
	g->pll = pll;
	g->cpu = cpu;
	g->mux = mux;
	g->saved = readl(pll->base + pll->reg);
	g->cpu_clk = devm_clk_hw_get_clk(dev, &cpu->hw, NULL);
	if (IS_ERR(g->cpu_clk))
		return PTR_ERR(g->cpu_clk);
	g->bypass_clk = devm_clk_hw_get_clk(dev, bypass, NULL);
	if (IS_ERR(g->bypass_clk))
		return PTR_ERR(g->bypass_clk);
	if (clk_get_rate(g->bypass_clk) != A133_BYPASS_RATE ||
	    clk_hw_get_rate(clk_hw_get_parent(&pll->hw)) != 24000000 ||
	    ccu_mux_helper_get_parent(cpu, mux) != A133_PLL_PARENT ||
	    !a133_pll_valid(g, clk_get_rate(g->cpu_clk)))
		return -EINVAL;
	ret = devm_clk_rate_exclusive_get(dev, g->bypass_clk);
	if (ret)
		return ret;
	ret = devm_clk_rate_exclusive_get(dev, g->cpu_clk);
	if (ret)
		return ret;
	g->nb.notifier_call = a133_cpu_notify;
	mutex_lock(&a133_cpu_mutex);
	if (a133_guard) {
		ret = -EBUSY;
		goto unlock;
	}
	ret = clk_notifier_register(pll->hw.clk, &g->nb);
	if (!ret)
		a133_guard = g;
unlock:
	mutex_unlock(&a133_cpu_mutex);
	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, a133_cpu_unregister, g);
}

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _CCU_SUN50I_A133_CPUFREQ_H
#define _CCU_SUN50I_A133_CPUFREQ_H

struct ccu_common;
struct ccu_mux_internal;
struct device;
struct clk_hw;

int sun50i_a133_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			   unsigned long parent_rate);
int sun50i_a133_cpufreq_register(struct device *dev, struct ccu_common *pll,
			       struct ccu_common *cpu,
			       struct ccu_mux_internal *mux,
			       struct clk_hw *bypass);
#endif

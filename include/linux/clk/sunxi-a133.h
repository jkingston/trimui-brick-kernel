/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_CLK_SUNXI_A133_H
#define _LINUX_CLK_SUNXI_A133_H

#include <linux/errno.h>
#include <linux/kconfig.h>

struct clk;

#if IS_REACHABLE(CONFIG_SUN50I_A100_CCU)
int sun50i_a133_cpux_is_ready(struct clk *clk);
int sun50i_a133_cpux_set_rate_checked(struct clk *clk, unsigned long rate);
#else
static inline int sun50i_a133_cpux_is_ready(struct clk *clk)
{
	return -ENODEV;
}

static inline int sun50i_a133_cpux_set_rate_checked(struct clk *clk,
						 unsigned long rate)
{
	return -ENODEV;
}
#endif
#endif

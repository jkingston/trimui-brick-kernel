/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _CCU_SUN50I_A133_SLEEP_H
#define _CCU_SUN50I_A133_SLEEP_H
#include <linux/kconfig.h>
struct device;
#if IS_ENABLED(CONFIG_SUN50I_A133_CCU_SLEEP)
int sun50i_a133_ccu_sleep_init(struct device *dev, void __iomem *base, bool r_ccu);
#else
static inline int sun50i_a133_ccu_sleep_init(struct device *dev, void __iomem *base,
					  bool r_ccu)
{
	return 0;
}
#endif
#endif

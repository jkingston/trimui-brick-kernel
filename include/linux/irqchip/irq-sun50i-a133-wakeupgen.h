/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_IRQCHIP_SUN50I_A133_WAKEUPGEN_H
#define _LINUX_IRQCHIP_SUN50I_A133_WAKEUPGEN_H

#include <linux/kconfig.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_SUN50I_A133_WAKEUPGEN)
bool sun50i_a133_wakeupgen_irq_ready(unsigned int irq);
#else
static inline bool sun50i_a133_wakeupgen_irq_ready(unsigned int irq)
{
	return false;
}
#endif

#endif

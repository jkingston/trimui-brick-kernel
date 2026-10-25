/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SUN50I_A133_CCU_SLEEP_H
#define _LINUX_SUN50I_A133_CCU_SLEEP_H
#include <linux/errno.h>
#include <linux/kconfig.h>
#if IS_ENABLED(CONFIG_SUN50I_A133_CCU_SLEEP)
int sun50i_a133_ccu_deep_ready(void);
#else
static inline int sun50i_a133_ccu_deep_ready(void)
{
	return -EOPNOTSUPP;
}
#endif
#endif

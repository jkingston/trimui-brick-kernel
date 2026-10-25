/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _SUN4I_SLEEP_H_
#define _SUN4I_SLEEP_H_

#include <linux/device.h>

/* The DRM aggregate, not component device PM order, owns reconstruction. */
enum sun4i_sleep_stage {
	SUN4I_SLEEP_TOP,
	SUN4I_SLEEP_ENGINE,
	SUN4I_SLEEP_TCON,
	SUN4I_SLEEP_OUTPUT,
	SUN4I_SLEEP_STAGES,
};

struct sun4i_sleep_client {
	struct device *dev;
	/* RAM-only ownership check; no MMIO after power-down. */
	int (*check)(struct device *dev);
	/* Read owned CCF fields while clocks are still available. */
	int (*save)(struct device *dev);
	/* Called after all checks, while the pipeline is disabled. */
	void (*suspend)(struct device *dev);
	/* CCU readiness is checked before any reconstruction callback. */
	int (*restore)(struct device *dev);
	/* Report errors hidden by DRM's void encoder enable callback. */
	int (*result)(struct device *dev);
};

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SUN50I_A133_GPU_H
#define _LINUX_SUN50I_A133_GPU_H

#include <linux/types.h>
struct device;

/* Callers hold the DDK power mutex; never invoke from IRQ/runtime-PM context. */
int a133_gpu_apm_handoff(struct device *consumer, bool wake, bool forced);
/* Requires a consumed successful stop token and synchronous hard-IRQ barrier. */
int a133_gpu_internal_off(struct device *consumer);
/* Persist a negative consumer error until reboot, without hardware access. */
int a133_gpu_latch_fault(struct device *consumer, int error);
/* RAM-only sticky fault query; false is not an accessibility guarantee. */
bool a133_gpu_power_fault(struct device *consumer);
#endif

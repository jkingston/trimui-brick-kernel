/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SUN50I_A133_GPU_H
#define _LINUX_SUN50I_A133_GPU_H

#include <linux/types.h>

/* Fixed Brick stock clock; established before the GPU consumer probes. */
#define A133_GPU_FIXED_RATE 702000000UL

struct device;

/* Callers hold the DDK power mutex; never invoke from IRQ/runtime-PM context. */
int a133_gpu_apm_handoff(struct device *consumer, bool wake, bool forced);
/* Requires a consumed successful stop token and synchronous hard-IRQ barrier. */
int a133_gpu_internal_off(struct device *consumer);
/* Persist a negative consumer error until reboot, without hardware access. */
int a133_gpu_latch_fault(struct device *consumer, int error);
/* Validate retained ownership without waking or changing external resources. */
int a133_gpu_s2idle_retained(struct device *consumer);
/* DDK clock transaction only: cache while internally OFF, write/readback otherwise. */
int a133_gpu_set_dfs(struct device *consumer, unsigned long rate);
/* RAM-only expected DFS; caller holds the DDK power mutex. Zero on fault. */
u32 a133_gpu_dfs_code(struct device *consumer);
/* RAM-only sticky fault query; false is not an accessibility guarantee. */
bool a133_gpu_power_fault(struct device *consumer);
#endif

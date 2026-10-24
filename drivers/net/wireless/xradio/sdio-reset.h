/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_SDIO_RESET_H
#define XRS_SDIO_RESET_H

#include <linux/types.h>
struct sdio_func;

/* Caller: callbacks/work quiesced, old BH joined, SDIO IRQ released.
 * Success: function1 enabled,512-byte blocks, no IRQ; firmware not loaded.
 * Failure: remain stopped. -EINPROGRESS: MMC scheduled device removal.
 */
int xrs_sdio_power_reset(struct sdio_func *func, bool *enabled);

#endif

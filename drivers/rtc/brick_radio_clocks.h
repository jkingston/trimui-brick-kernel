/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BRICK_RADIO_CLOCKS_H
#define BRICK_RADIO_CLOCKS_H
#include <linux/io.h>
struct device;
struct brick_radio_clocks;
int brick_radio_clocks_register(struct device *dev, void __iomem *base,
			       struct brick_radio_clocks **result);
/* Noirq retention boundary; no clock votes or mux fields are replayed. */
int brick_radio_clocks_suspend(struct brick_radio_clocks *clocks);
int brick_radio_clocks_resume(struct brick_radio_clocks *clocks);
void brick_radio_clocks_fault(struct brick_radio_clocks *clocks);
void brick_radio_clocks_sync(struct brick_radio_clocks *clocks);
#endif

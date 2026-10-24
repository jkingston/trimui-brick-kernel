/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BRICK_RADIO_CLOCKS_H
#define BRICK_RADIO_CLOCKS_H
#include <linux/io.h>
struct device;
struct brick_radio_clocks;
int brick_radio_clocks_register(struct device *dev, void __iomem *base,
			       struct brick_radio_clocks **result);
void brick_radio_clocks_sync(struct brick_radio_clocks *clocks);
#endif

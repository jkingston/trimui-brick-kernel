/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2015 Free Electrons
 * Copyright (C) 2015 NextThing Co
 *
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 */

#ifndef _SUN4I_DRV_H_
#define _SUN4I_DRV_H_

#include <linux/clk.h>
#include <linux/list.h>
#include <linux/regmap.h>

#include "sun4i_sleep.h"

struct sun4i_drv {
	struct sun4i_sleep_client sleep[SUN4I_SLEEP_STAGES];
	bool deep_suspended;
	int sleep_error;
	struct list_head	engine_list;
	struct list_head	frontend_list;
	struct list_head	tcon_list;
};

#endif /* _SUN4I_DRV_H_ */

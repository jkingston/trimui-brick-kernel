// SPDX-License-Identifier: GPL-2.0
/* A133 vendor tcon0_cfg_mode_tri arithmetic, with no implicit clock fallback.
 * Register widths follow vendor de_lcd_type.h, CPU_TRI0/1/2.
 * This helper deliberately performs no hardware access.
 */
#include <linux/errno.h>
#include <linux/overflow.h>
#include "sun50i_a133_tcon.h"

int sun50i_a133_tcon_calc_trigger(const struct sun50i_a133_tcon_timing *t,
				struct sun50i_a133_tcon_trigger *out)
{
	struct sun50i_a133_tcon_trigger r;
	u32 product, divisor, blank;

	if (!t || !out || !t->width || !t->height || !t->htotal ||
	    !t->vtotal || !t->de_mhz || !t->pixel_mhz || !t->pixel_bits ||
	    !t->lanes || !t->tcon_div)
		return -EINVAL;
	if (t->width > 4096 || t->height > 4096 || t->htotal <= t->width ||
	    t->vtotal < t->height || t->vtotal - t->height < 9)
		return -ERANGE;
	blank = t->vtotal - t->height - 9;
	if (check_mul_overflow(blank, t->htotal, &product) ||
	    check_mul_overflow(product, t->de_mhz, &product))
		return -ERANGE;
	r.start_delay = product / t->pixel_mhz / 8;
	r.block_size = t->width - 1;
	r.block_num = t->height - 1;
	if (t->command_mode) {
		r.block_space = t->htotal - t->width - 1;
		r.trans_start_set = t->width - 1;
	} else {
		if (check_mul_overflow(t->htotal, t->pixel_bits, &product) ||
		    check_mul_overflow(t->tcon_div, t->lanes, &divisor))
			return -ERANGE;
		product /= divisor;
		if (product < t->width + 40)
			return -ERANGE;
		r.block_space = product - t->width - 40;
		r.trans_start_set = 10;
	}
	if (r.block_space > 4095 || r.start_delay > 65535)
		return -ERANGE;
	*out = r;
	return 0;
}

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SUN50I_A133_TCON_H_
#define _SUN50I_A133_TCON_H_

#include <linux/types.h>

struct sun50i_a133_tcon_timing {
	u32 width, height, htotal, vtotal;
	u32 de_mhz, pixel_mhz, pixel_bits, lanes, tcon_div;
	bool command_mode;
};

struct sun50i_a133_tcon_trigger {
	u32 block_size, block_num, block_space, trans_start_set, start_delay;
};

int sun50i_a133_tcon_calc_trigger(const struct sun50i_a133_tcon_timing *timing,
				struct sun50i_a133_tcon_trigger *trigger);

#endif

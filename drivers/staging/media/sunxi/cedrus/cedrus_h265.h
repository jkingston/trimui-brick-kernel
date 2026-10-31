/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CEDRUS_H265_H_
#define _CEDRUS_H265_H_

#include <linux/types.h>

static inline bool cedrus_h265_entry_points_match(u32 count,
						  u32 control_count)
{
	return !count || count == control_count;
}

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_STATION_BA_H
#define XRS_STATION_BA_H
#include "xr829-wire.h"

/* Vendor AP_AGGREGATE_FW_FIX: four hardware BA contexts (TIDs 0, 4, 5, 7).
 * Firmware negotiates BA; mac80211 owns receive reordering and replay checks.
 */
#define XRS_BA_TIDS 0xb1
struct xrs_ba_state {
	u8 enabled;
	u8 rx;
};

static inline int xrs_ba_start(struct xrs_ba_state *ba, unsigned int tid,
			       unsigned int ssn, unsigned int window)
{
	if (!ba->enabled)
		return -EOPNOTSUPP;
	if (tid >= 8 || !(XRS_BA_TIDS & (1U << tid)) || ssn > 4095 ||
	    !window || window > 64)
		return -EINVAL;
	if (ba->rx & (1U << tid))
		return -EALREADY;
	ba->rx |= 1U << tid;
	return 0;
}

static inline int xrs_ba_stop(struct xrs_ba_state *ba, unsigned int tid)
{
	if (tid >= 8)
		return -EINVAL;
	ba->rx &= ~(1U << tid);
	return 0;
}

/* Firmware delivers individual MPDUs, with aggregate/first/last metadata.
 * Defragmented frames remain unsupported; no aggregate delimiter bytes exist
 * in this WSM record. FIRST/LAST without AGGREGATE is malformed.
 */
static inline int xrs_ba_rx_flags(u32 flags)
{
	if (flags & 0x40)
		return -EOPNOTSUPP;
	if ((flags & 0x30) && !(flags & 0x08))
		return -EINVAL;
	return 0;
}
#endif

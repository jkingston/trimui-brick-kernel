/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_STATION_PS_H
#define XRS_STATION_PS_H
#include "xr829-wire.h"

struct xrs_ps_state {
	u8 applied[4];
	u8 requested[4];
	bool valid, pending, indicated, failed;
};

/* WSM fast-PS intervals are 500us units, saturated to one byte. */
static inline void xrs_ps_encode(u8 wire[4], bool enabled, int timeout_ms)
{
	if (!enabled) {
		memset(wire, 0, 4);
		return;
	}
	wire[0] = enabled ? (timeout_ms > 0 ? 0x81 : 1) : 0;
	wire[1] = timeout_ms <= 0 ? 0 : timeout_ms >= 128 ? 255 : timeout_ms * 2;
	wire[2] = 200; /* Vendor AP PS transition period: 100ms. */
	wire[3] = 0; /* Disable automatic PS-Poll interval. */
}

static inline int xrs_ps_begin(struct xrs_ps_state *ps, const u8 wire[4])
{
	if (ps->failed)
		return -EIO;
	if (ps->pending)
		return -EBUSY;
	if (ps->valid && !memcmp(ps->applied, wire, 4))
		return 0;
	memcpy(ps->requested, wire, 4);
	ps->pending = true;
	ps->indicated = false;
	return 1;
}

static inline bool xrs_ps_indicate(struct xrs_ps_state *ps)
{
	if (!ps->pending || ps->indicated)
		return false;
	ps->indicated = true;
	return true;
}

static inline int xrs_ps_finish(struct xrs_ps_state *ps, int error)
{
	if (!ps->pending)
		return -EINVAL;
	if (!error && !ps->indicated)
		error = -ETIMEDOUT;
	ps->pending = false;
	if (error) {
		ps->valid = false;
		ps->failed = true;
		return error;
	}
	memcpy(ps->applied, ps->requested, 4);
	ps->valid = true;
	return 0;
}
#endif

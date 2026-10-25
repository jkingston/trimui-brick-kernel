/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_STATION_POWER_H
#define XRS_STATION_POWER_H
#include "xr829-wire.h"

/* Firmware limits and output use 0.1 dBm. Never raise a requested limit
 * to the firmware minimum; reject a limit the controller cannot satisfy.
 */
static inline int xrs_power_limit(int channel_dbm, int configured_dbm,
				  int bss_dbm, int firmware_min, int firmware_max,
				  int *result)
{
	long long power = firmware_max, limit;

	if (firmware_min > firmware_max)
		return -EINVAL;
	limit = (long long)channel_dbm * 10;
	if (limit < power)
		power = limit;
	limit = (long long)configured_dbm * 10;
	if (limit < power)
		power = limit;
	/* INT_MIN is mac80211's unconfigured per-BSS sentinel. */
	if (bss_dbm != (-2147483647 - 1)) {
		limit = (long long)bss_dbm * 10;
		if (limit < power)
			power = limit;
	}
	if (power < firmware_min)
		return -ERANGE;
	*result = (int)power;
	return 0;
}
#endif

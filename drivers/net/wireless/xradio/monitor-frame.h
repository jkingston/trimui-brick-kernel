/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_MONITOR_FRAME_H
#define XRS_MONITOR_FRAME_H

#include "xr829-wire.h"

/*
 * mac80211's no-ACK-reporting connection monitor uses directed probes,
 * followed by broadcast probes, for the currently associated SSID only.
 * This is ordinary management TX, not another foreground hardware scan.
 */
static inline int xrs_monitor_probe(const u8 *frame, size_t size,
				    const u8 source[6], const u8 bssid[6],
				    const u8 *ssid, size_t ssid_len,
				    int associated)
{
	static const u8 broadcast[6] = { 255, 255, 255, 255, 255, 255 };
	unsigned int ssids = 0;
	size_t offset;

	if (!associated)
		return -ENOTCONN;
	if (size < 26 || size > 24 + 2304 || ssid_len > 32)
		return -EMSGSIZE;

	/* Only Retry may vary; no DS, protection, fragmentation or HT control. */
	if ((xrw_get16(frame) & ~0x0800) != 0x0040 ||
	    (xrw_get16(frame + 22) & 15))
		return -EOPNOTSUPP;
	if (memcmp(frame + 10, source, 6))
		return -EINVAL;
	if (memcmp(frame + 4, bssid, 6) && memcmp(frame + 4, broadcast, 6))
		return -EINVAL;
	if (memcmp(frame + 16, frame + 4, 6))
		return -EINVAL;

	for (offset = 24; offset < size;) {
		size_t length;

		if (size - offset < 2)
			return -EMSGSIZE;
		length = frame[offset + 1];
		if (length > size - offset - 2)
			return -EMSGSIZE;
		if (!frame[offset]) {
			if (++ssids != 1 || length != ssid_len ||
			    memcmp(frame + offset + 2, ssid, ssid_len))
				return -EINVAL;
		}
		offset += 2 + length;
	}

	return ssids == 1 ? 0 : -EINVAL;
}

#endif

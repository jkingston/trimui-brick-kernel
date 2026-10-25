/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRADIO_SDD_POWER_H
#define XRADIO_SDD_POWER_H
#include "xr829-wire.h"

/* XR829 SDD: signed 1/16 dBm maximum and front-end loss. The remaining
 * per-rate entries are interpreted by firmware from the complete SDD.
 */
static inline int xrs_sdd_power(const u8 *data, size_t size, int *minimum,
				int *maximum)
{
	size_t off = 0;
	int max = 0, correction = 0, fixed, power;
	bool have_max = false, have_correction = false;

	while (off < size) {
		unsigned int id, len;
		int value;

		if (size - off < 2)
			return -EINVAL;
		id = data[off];
		len = data[off + 1];
		off += 2;
		if (len > size - off)
			return -EINVAL;
		if (id == 0xe3 || id == 0x30) {
			if (len < 2)
				return -EINVAL;
			value = (int)(data[off] | data[off + 1] << 8);
			if (value & 0x8000)
				value -= 0x10000;
			if (id == 0xe3) {
				if (have_max)
					return -EINVAL;
				max = value;
				have_max = true;
			} else {
				if (have_correction)
					return -EINVAL;
				correction = value;
				have_correction = true;
			}
		}
		off += len;
	}
	if (!have_max || !have_correction)
		return -EINVAL;
	fixed = (max - correction) * 10;
	/* Round the maximum down to 0.1 dBm, never above calibration. */
	power = fixed / 16;
	if (fixed < 0 && fixed % 16)
		power--;
	*maximum = power;
	/* Vendor documents a 12 dBm output-control span. Round its lower
	 * endpoint up so the request remains within that span.
	 */
	*minimum = power - 120 + (fixed % 16 != 0);
	return 0;
}
#endif

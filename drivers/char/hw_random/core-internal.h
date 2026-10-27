/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HW_RANDOM_CORE_INTERNAL_H
#define HW_RANDOM_CORE_INTERNAL_H

#include <linux/errno.h>
#include <linux/hw_random.h>

static inline int hwrng_validate_quality(const struct hwrng *rng,
					 unsigned short quality)
{
	if (quality > 1024)
		return -EINVAL;

	if (rng->no_entropy_credit && quality)
		return -EPERM;

	return 0;
}

#endif /* HW_RANDOM_CORE_INTERNAL_H */

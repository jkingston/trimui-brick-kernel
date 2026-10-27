/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SUN50I_A133_VE_DOMAIN_H
#define _LINUX_SUN50I_A133_VE_DOMAIN_H

#include <linux/err.h>
#include <linux/kconfig.h>

struct device;
struct sun50i_a133_ve_domain;

#if IS_ENABLED(CONFIG_SUN50I_A133_VE_DOMAIN)
struct sun50i_a133_ve_domain *
devm_sun50i_a133_ve_domain_get(struct device *dev);
int sun50i_a133_ve_domain_acquire(struct sun50i_a133_ve_domain *domain,
				  struct device *owner);
void sun50i_a133_ve_domain_release(struct sun50i_a133_ve_domain *domain,
				   struct device *owner);
int sun50i_a133_ve_domain_reset(struct sun50i_a133_ve_domain *domain,
				struct device *owner);
int sun50i_a133_ve_domain_assert(struct sun50i_a133_ve_domain *domain,
				 struct device *owner);
#else
static inline struct sun50i_a133_ve_domain *
devm_sun50i_a133_ve_domain_get(struct device *dev)
{
	return ERR_PTR(-ENODEV);
}

static inline int
sun50i_a133_ve_domain_acquire(struct sun50i_a133_ve_domain *domain,
			      struct device *owner)
{
	return -ENODEV;
}

static inline void
sun50i_a133_ve_domain_release(struct sun50i_a133_ve_domain *domain,
			      struct device *owner)
{
}

static inline int
sun50i_a133_ve_domain_reset(struct sun50i_a133_ve_domain *domain,
			    struct device *owner)
{
	return -ENODEV;
}

static inline int
sun50i_a133_ve_domain_assert(struct sun50i_a133_ve_domain *domain,
			     struct device *owner)
{
	return -ENODEV;
}
#endif

#endif

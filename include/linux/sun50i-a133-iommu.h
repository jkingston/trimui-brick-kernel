/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SUN50I_A133_IOMMU_H
#define _LINUX_SUN50I_A133_IOMMU_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;

struct sun50i_a133_iommu_fault {
	dma_addr_t iova;
	u32 status;
	u32 data;
	u8 master;
	u8 flags;
	bool flags_valid;
};

typedef void (*sun50i_a133_fault_handler_t)(struct device *dev,
		const struct sun50i_a133_iommu_fault *fault, void *data);

#if IS_ENABLED(CONFIG_SUN50I_A133_IOMMU)
int sun50i_a133_iommu_register_fault_handler(struct device *dev,
					     sun50i_a133_fault_handler_t handler,
					     void *data);
void sun50i_a133_iommu_unregister_fault_handler(struct device *dev);
int sun50i_a133_iommu_fault_complete(struct device *dev);
#else
static inline int sun50i_a133_iommu_register_fault_handler(struct device *dev,
							   sun50i_a133_fault_handler_t handler,
							   void *data)
{
	return -ENODEV;
}

static inline void
sun50i_a133_iommu_unregister_fault_handler(struct device *dev)
{
}

static inline int sun50i_a133_iommu_fault_complete(struct device *dev)
{
	return -ENODEV;
}
#endif

#endif

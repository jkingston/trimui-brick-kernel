/* SPDX-License-Identifier: GPL-2.0 OR MIT */
#ifndef PVR_PFN_COMPAT_H
#define PVR_PFN_COMPAT_H

#include <linux/version.h>
#include <linux/mm.h>

/* Linux 7.3 mm.h takes a plain PFN in vmf_insert_mixed(). These helpers
 * deliberately support only ordinary RAM/physical PFNs, as used by osfunc.c
 * with phys_to_pfn_t(address, 0). They do not model PFN_DEV/PFN_MAP flags.
 * Earlier-kernel branch retains the original pfn_t implementation.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0)
typedef unsigned long pvr_pfn_t;
#define pvr_pfn_to_pfn(pfn) (pfn)
static inline pvr_pfn_t pvr_page_to_pfn(struct page *page)
{
	return page_to_pfn(page);
}
static inline pvr_pfn_t pvr_phys_to_pfn(phys_addr_t address)
{
	return address >> PAGE_SHIFT;
}
static inline bool pvr_pfn_valid(pvr_pfn_t pfn)
{
	return pfn_valid(pfn);
}
static inline struct page *pvr_pfn_to_page(pvr_pfn_t pfn)
{
	return pfn_to_page(pfn);
}
#else
#include <linux/pfn_t.h>
typedef pfn_t pvr_pfn_t;
#define pvr_pfn_to_pfn(pfn) pfn_t_to_pfn(pfn)
#define pvr_page_to_pfn(page) page_to_pfn_t(page)
#define pvr_phys_to_pfn(address) phys_to_pfn_t(address, 0)
#define pvr_pfn_valid(pfn) pfn_t_valid(pfn)
#define pvr_pfn_to_page(pfn) pfn_t_to_page(pfn)
#endif
#endif

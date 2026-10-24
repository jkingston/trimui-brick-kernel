/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_SCAN_POLICY_H
#define XRS_SCAN_POLICY_H
#include "xr829-wire.h"
static inline int xrs_scan_policy_tightened(u32 old_flags,int old_power,
 u32 new_flags,int new_power,u32 restrictions)
{
 return !!((new_flags & ~old_flags) & restrictions) || new_power<old_power;
}
#endif

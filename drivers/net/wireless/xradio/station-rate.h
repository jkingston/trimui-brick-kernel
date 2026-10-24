/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_RATE_H
#define XRS_RATE_H
#include "station-wire.h"
/* mac80211 2.4GHz rate table: CCK0..3, OFDM4..11; firmware skips4/5. */
static inline int xrs_rate_hw(unsigned int index)
{
 if(index>=12)return -EINVAL;
 return index<4?index:index+2;
}
static inline int xrs_rate_policy(u8 *out,size_t size,unsigned int index,u32 supported)
{
 u16 rates[6]={0};int hw=xrs_rate_hw(index);
 if(hw<0 || !(supported&1) || !(supported&(1U<<hw)))return -EINVAL;
 rates[0]=hw?((unsigned int)hw<<4)|4:7;
 if(hw)rates[1]=3; /* Three1Mbps fallback attempts; seven total. */
 return xrs_policy_encode(out,size,index,6,6,rates);
}
#endif

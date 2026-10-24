/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_RATE_ENTRY_H
#define XRS_RATE_ENTRY_H
#include "xr829-wire.h"
/* XR829 SUPPORT_HT40 ABI, vendor wlan/txrx.h. Index is the mac80211
 * legacy table index or the single-stream MCS number, depending on ht.
 * Retry nibble is deliberately excluded from rate identity.
 */
struct xrs_rate_entry {
 u8 index,ht,width40,short_gi,greenfield,stbc,short_preamble;
};
static inline int xrs_rate_entry_decode(struct xrs_rate_entry *out,u16 wire)
{
 struct xrs_rate_entry r={0};
 unsigned int modem=wire>>14,bw=(wire>>12)&3,flags=(wire>>8)&15,index=(wire>>4)&15;
 if(modem==0) {
  if(bw || (flags&~1U) || index==4 || index==5 || index>13)return -EOPNOTSUPP;
  r.index=index<4?index:index-2;r.short_preamble=flags&1;
 } else if(modem==1) {
  if(bw>1 || (flags&1) || index>7)return -EOPNOTSUPP;
  r.ht=1;r.index=index;r.width40=bw;
  r.short_gi=!!(flags&2);r.greenfield=!!(flags&4);r.stbc=!!(flags&8);
 } else return -EOPNOTSUPP;
 *out=r;return 0;
}
static inline int xrs_rate_entry_encode(u16 *out,const struct xrs_rate_entry *r,u8 attempts)
{
 u16 wire;
 if(attempts>15 || r->ht>1 || r->width40>1 || r->short_gi>1 ||
    r->greenfield>1 || r->stbc>1 || r->short_preamble>1)return -EINVAL;
 if(r->ht) {
  if(r->index>7 || r->short_preamble)return -EINVAL;
  wire=0x4000|(r->width40<<12)|(r->short_gi<<9)|(r->greenfield<<10)|(r->stbc<<11)|(r->index<<4);
 } else {
  if(r->index>11 || r->width40 || r->short_gi || r->greenfield || r->stbc)return -EINVAL;
  wire=((r->index<4?r->index:r->index+2)<<4)|(r->short_preamble<<8);
 }
 *out=wire|attempts;return 0;
}
/* Select the lowest advertised basic rate by bitrate, not hardware index:
 * 6 and 9 Mbps precede CCK 11 Mbps. Keep long preamble for management. */
static inline int xrs_basic_rate_encode(u16 *out,u32 basic,u8 attempts)
{
 static const u8 order[]={0,1,2,6,7,3,8,9,10,11,12,13};
 unsigned int i;
 if(!basic || (basic&~0x3fcfU) || !attempts || attempts>15)return -EINVAL;
 for(i=0;i<sizeof(order);i++)if(basic&(1U<<order[i])) {
  *out=(order[i]<<4)|attempts;
  return 0;
 }
 return -EINVAL;
}
#endif

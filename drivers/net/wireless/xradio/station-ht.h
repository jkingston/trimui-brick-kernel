/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_HT_H
#define XRS_HT_H
#include "rate-entry.h"
struct xrs_ht_link { u8 rx_mcs,tx_mcs,enabled,short_gi,rx_short_gi,spacing; };
/* Initial HT20 association. HT40 channel-primary mapping needs validation
 * before this helper is extended; no silent bandwidth downgrade. */
static inline int xrs_ht_association(u8 *out,size_t size,
 const struct xrs_ht_link *link,u32 basic,unsigned int short_preamble)
{
 u16 phy=3;
 if(size<12)return -EMSGSIZE;
 if(link->enabled>1 || link->short_gi>1 || link->rx_short_gi>1 || short_preamble>1 ||
    link->spacing>7 || !basic || (basic&~0x3fcf) ||
    (link->enabled && (!link->rx_mcs || !link->tx_mcs)) ||
    (!link->enabled && (link->rx_mcs || link->tx_mcs || link->short_gi || link->rx_short_gi || link->spacing)))return -EINVAL;
 if(link->enabled)phy|=4|(link->short_gi?0x4000:0);
 if(short_preamble)phy|=0x1000;
 memset(out,0,12);out[0]=7;xrw_put16(out+2,phy);
 out[4]=link->spacing;xrw_put32(out+8,basic);return 12;
}
static inline u32 xrs_ht_protection(u16 operation)
{
 return ((operation&4)?2:0)|((operation&3)<<2);
}
static inline int xrs_ht_rx_allowed(const struct xrs_ht_link *link,
 const struct xrs_rate_entry *rate)
{
 if(!rate->ht)return 1;
 return link->enabled && rate->index<8 && (link->rx_mcs&(1U<<rate->index)) &&
        !rate->width40 && (!rate->short_gi || link->rx_short_gi) &&
        !rate->greenfield && !rate->stbc;
}
#endif

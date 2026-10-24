/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_STATION_POLICY_H
#define XRS_STATION_POLICY_H
#include "station-wire.h"
#include "station-ht.h"
#include "policy-cache.h"
#define XRS_SELECTED_RATES 4
#define XRS_POLICY_ATTEMPTS 16
struct xrs_policy_rate { struct xrs_rate_entry rate; unsigned int count; };
static inline int xrs_policy_rate_allowed(const struct xrs_rate_entry *rate,
                                          u32 legacy,const struct xrs_ht_link *ht)
{
 unsigned int hw;
 if(rate->ht) {
  if(!ht->enabled || rate->index>7 || !(ht->tx_mcs&(1U<<rate->index)) ||
     rate->width40 || rate->greenfield || rate->stbc ||
     (rate->short_gi && !ht->short_gi))return -EOPNOTSUPP;
 } else {
  if(rate->index>11)return -EINVAL;
  hw=rate->index<4?rate->index:rate->index+2;
  if(!(legacy&(1U<<hw)))return -EOPNOTSUPP;
 }
 return 0;
}
/* Preserve mac80211's order/counts exactly. No invented fallback, silent
 * count truncation, duplicate merging, or rate-to-policy slot aliases. */
static inline int xrs_policy_chain(struct xpc_key *out,
 const struct xrs_policy_rate *selected,size_t count,u32 legacy,const struct xrs_ht_link *ht)
{
 struct xpc_key key={.flags=0x0c};unsigned int i,total=0;int ret;
 if(!selected || !count || count>XRS_SELECTED_RATES)return -EINVAL;
 for(i=0;i<count;i++) {
  if(!selected[i].count || selected[i].count>15)return -EINVAL;
  ret=xrs_policy_rate_allowed(&selected[i].rate,legacy,ht);if(ret)return ret;
  ret=xrs_rate_entry_encode(&key.rates[i],&selected[i].rate,selected[i].count);if(ret)return ret;
  total+=selected[i].count;
 }
 if(total>XRS_POLICY_ATTEMPTS)return -ERANGE;
 key.short_retry=total-1;key.long_retry=total-1;
 *out=key;return 0;
}
/* Management policy is independent of data rate control and permanently
 * pinned by its own lease. Use the lowest negotiated basic rate. */
static inline int xrs_policy_management(struct xpc_key *out,u32 basic)
{
 struct xpc_key key={.short_retry=6,.long_retry=6,.flags=0x0c,.rates={7}};
 int ret=xrs_basic_rate_encode(&key.rates[0],basic,7);
 if(ret)return ret;
 *out=key;return 0;
}
static inline int xrs_policy_wire(u8 *out,size_t size,unsigned int slot,const struct xpc_key *key)
{
 unsigned int i;bool ended=false;struct xrs_rate_entry decoded;
 if(size<XRS_POLICY_SIZE)return -EMSGSIZE;
 if(slot>=16 || key->short_retry>15 || key->long_retry>15 ||
    key->flags!=0x0c || key->recovery || !key->rates[0])return -EINVAL;
 for(i=0;i<6;i++) {
  if(!key->rates[i])ended=true;
  else if(ended || !(key->rates[i]&15) || xrs_rate_entry_decode(&decoded,key->rates[i]))return -EINVAL;
 }
 memset(out,0,XRS_POLICY_SIZE);out[0]=1;out[4]=slot;
 out[5]=key->short_retry;out[6]=key->long_retry;out[7]=key->flags;out[8]=key->recovery;
 for(i=0;i<6;i++)xrw_put16(out+12+2*i,key->rates[i]);
 return XRS_POLICY_SIZE;
}
#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_FEEDBACK_H
#define XRS_FEEDBACK_H
#include "station-wire.h"
#include "rate-entry.h"
struct xrs_feedback { u8 index[4], count[4], short_preamble[4], used; struct xrs_rate_entry entry[4]; };
static inline int xrs_feedback_add(struct xrs_feedback *f,u16 rate,unsigned int count)
{
 unsigned int i;struct xrs_rate_entry entry;
 int ret=xrs_rate_entry_decode(&entry,rate);if(ret)return ret;
 if(!count)return 0;
 for(i=0;i<f->used;i++)
  if(!memcmp(&f->entry[i],&entry,sizeof(entry)))break;
 if(i==4)return -E2BIG;
 if(f->count[i]+count>31)return -ERANGE;
 if(i==f->used) {
  f->entry[i]=entry;f->index[i]=entry.index;
  f->short_preamble[i]=entry.short_preamble;f->used++;
 }
 f->count[i]+=count;return 0;
}
/* Vendor HT40 ABI: six rate entries encode failed attempts in low nibble.
 * A successful final transmit adds one attempt at txedRateEntry. No native
 * bitfields; preserve output on malformed/unsupported feedback.
 */
static inline int xrs_feedback_decode(struct xrs_feedback *out,const struct xrs_tx_confirm *c)
{
 struct xrs_feedback f={0};unsigned int i;int ret;
 /* Do not invent attempts from the final-rate field when firmware reports
  * success without explicit ACK or with firmware-owned aggregation. Keep
  * every status rate invalid rather than inventing per-MPDU attempts.
  */
 if(xrs_tx_success_no_rate(c)) {*out=f;return 0;}
 if(c->flags)return -EOPNOTSUPP;
 for(i=0;i<6;i++) {
  if(((c->retries[i]>>4)&15)==15)break;
  ret=xrs_feedback_add(&f,c->retries[i],c->retries[i]&15);
  if(ret)return ret;
 }
 if(!c->status) {
  struct xrs_rate_entry final;
  ret=xrs_feedback_add(&f,c->rate,1);if(ret)return ret;
  ret=xrs_rate_entry_decode(&final,c->rate);if(ret)return ret;
  /* mac80211 attributes success to the last reported rate. A repeated
   * final identity may have been merged into an earlier failed stage. */
  for(i=0;i<f.used;i++)if(!memcmp(&f.entry[i],&final,sizeof(final)))break;
  if(i+1<f.used) {
   u8 count=f.count[i],index=f.index[i],short_pre=f.short_preamble[i];unsigned int j;
   for(j=i;j+1<f.used;j++) {
    f.entry[j]=f.entry[j+1];f.count[j]=f.count[j+1];
    f.index[j]=f.index[j+1];f.short_preamble[j]=f.short_preamble[j+1];
   }
   f.entry[f.used-1]=final;f.count[f.used-1]=count;
   f.index[f.used-1]=index;f.short_preamble[f.used-1]=short_pre;
  }
 }
 *out=f;return 0;
}
#endif

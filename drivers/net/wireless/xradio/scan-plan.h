/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XR829_SCAN_PLAN_H
#define XR829_SCAN_PLAN_H
#include "xr829-wire.h"
#ifndef __KERNEL__
#include <stdbool.h>
#endif
#define XRS_SCAN_CHANNELS 14
#define XRS_SCAN_SSIDS 2
struct xrs_scan_channel {
 u8 number;
 bool disabled, no_ir, no_ofdm;
 int power_dbm;
};
struct xrs_scan_batch {
 u8 first, count;
 bool active;
 int power_dbm;
};
struct xrs_scan_plan {
 struct xrs_scan_batch batch[XRS_SCAN_CHANNELS];
 u8 count;
};
/* Preserve request order and every channel. Firmware probe count and output
 * power are per-command, so active/passive and power boundaries split batches.
 * The caller must confirm each active batch's power before command publication.
 * No channel record txPowerLevel field is assumed to enforce that limit. */
static inline int xrs_scan_plan_build(struct xrs_scan_plan *out,
 const struct xrs_scan_channel *channels,unsigned int count,unsigned int ssids,
 const u8 *ssid_lengths,int power_cap_dbm,bool no_cck)
{
 struct xrs_scan_plan p={0};
 unsigned int i;
 if(!channels || !count || count>XRS_SCAN_CHANNELS || ssids>XRS_SCAN_SSIDS ||
    (ssids && !ssid_lengths))return -EINVAL;
 for(i=0;i<ssids;i++)if(ssid_lengths[i]>32)return -EINVAL;
 for(i=0;i<count;i++) {
  const struct xrs_scan_channel *c=&channels[i];
  struct xrs_scan_batch *b=p.count?&p.batch[p.count-1]:NULL;
  bool active=ssids && !c->no_ir && !(no_cck && c->no_ofdm);
  int power=c->power_dbm<power_cap_dbm?c->power_dbm:power_cap_dbm;
  if(c->disabled || !c->number || c->number>14)return -EINVAL;
  if(!b || b->active!=active || (active && b->power_dbm!=power)) {
   b=&p.batch[p.count++];b->first=i;b->active=active;
   b->power_dbm=active?power:0;
  }
  b->count++;
 }
 *out=p;
 return 0;
}
#endif

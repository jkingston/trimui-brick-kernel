/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_STATION_BUS_H
#define XRS_STATION_BUS_H
#include "station-tx-state.h"
#include "station-wire.h"
/* Shared kernel/fake-bus credit transitions; caller holds the slot lock.
 * Positive return releases exactly one firmware credit, independently of whether
 * the completed SKB can yet be reported (early confirmations remain buffered).
 */
static inline int xrs_bus_confirm(struct xrs_tx_slot *slot, bool *credit,
                                  u32 cookie, u32 status, bool *done)
{
 *done=false;
 if(!*credit || slot->cookie!=cookie || slot->confirmed ||
    (slot->phase!=XRS_SENDING && slot->phase!=XRS_INFLIGHT)) return 0;
 *credit=false;
 *done=xrs_tx_confirmed(slot,cookie,status);
 return 1;
}
static inline int xrs_bus_written(struct xrs_tx_slot *slot,bool *credit,
                                  bool failed,bool *done)
{
 int release=0;
 if(failed && *credit) {*credit=false;release=1;}
 *done=xrs_tx_written(slot,failed);
 return release;
}
/* Batch aggregation is shared with the fake bus: duplicate entries must not
 * erase a prior completion bit or release another credit for the same cookie. */
static inline int xrs_bus_confirm_many(struct xrs_tx_slot *slots,bool *credits,
                                        unsigned int slot_count,
                                        const struct xrs_tx_confirm *records,
                                        unsigned int count,u16 *completed,
                                        u16 *acked,u16 *accepted)
{
 unsigned int k;int released=0;
 *completed=*acked=*accepted=0;
 if(slot_count>16 || count>16)return -EINVAL;
 for(k=0;k<count;k++) {
  const struct xrs_tx_confirm *c=&records[k];unsigned int i=c->packet_id&255;
  bool done;
  if(i>=slot_count)continue;
  if(!xrs_bus_confirm(slots+i,credits+i,c->packet_id,c->status,&done))continue;
  released++;*accepted|=(u16)1<<i;
  if(done)*completed|=(u16)1<<i;
  if(xrs_tx_success(c))*acked|=(u16)1<<i;
 }
 return released;
}
#endif

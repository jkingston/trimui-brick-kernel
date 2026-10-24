/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XR829_STATION_TX_STATE_H
#define XR829_STATION_TX_STATE_H
#include "xr829-wire.h"
/* Caller must serialize transitions. SKB/pointers stay in the kernel wrapper.
 * Completion claim means 'report/free once', never permission to reuse before
 * xrs_tx_released(). Uncertain bus writes poison the whole session externally.
 */
enum xrs_tx_phase { XRS_FREE, XRS_QUEUED, XRS_SENDING, XRS_INFLIGHT, XRS_DONE };
struct xrs_tx_slot {
 enum xrs_tx_phase phase;
 u16 generation;
 u32 cookie, status, enqueue_order;
 unsigned long queued_at;
 unsigned char confirmed, poisoned;
};
/* At most 16 * 4095 reservations per runtime (generations never wrap),
 * so the runtime's monotonically increasing order cannot wrap either. */
static inline unsigned int xrs_tx_oldest(const struct xrs_tx_slot *slots,
                                        unsigned int count)
{
 unsigned int i,best=count;
 for(i=0;i<count;i++)
  if(slots[i].phase==XRS_QUEUED &&
     (best==count || slots[i].enqueue_order<slots[best].enqueue_order))best=i;
 return best;
}
/* Balanced allocation preserves each slot's finite wire generation budget. */
static inline unsigned int xrs_tx_available(const struct xrs_tx_slot *slots,
                                            unsigned int count,
                                            unsigned int cursor)
{
 unsigned int offset,i;
 if(!count || cursor>=count)return count;
 for(offset=0;offset<count;offset++) {
  i=(cursor+offset)%count;
  if(slots[i].phase==XRS_FREE && slots[i].generation<4095)return i;
 }
 return count;
}
/* Rejected/unpublished reservations consume identities too. Busy slots alone
 * are not exhaustion; only the monotonic generation budget triggers rollover. */
static inline int xrs_tx_epoch_due(const struct xrs_tx_slot *slots,
                                  unsigned int count,unsigned int threshold)
{
 unsigned int i;
 if(!threshold || threshold>4095)return 0;
 for(i=0;i<count;i++)if(slots[i].generation>=threshold)return 1;
 return 0;
}
static inline int xrs_tx_pending(const struct xrs_tx_slot *s)
{
 return s->phase == XRS_QUEUED || s->phase == XRS_SENDING ||
        s->phase == XRS_INFLIGHT;
}
/* jiffies-style wrap-safe comparison; timeout must be below LONG_MAX. */
static inline int xrs_tx_expired(const struct xrs_tx_slot *s,
                                unsigned long now, unsigned long timeout)
{
 return xrs_tx_pending(s) && (long)(s->queued_at + timeout - now) < 0;
}
static inline int xrs_tx_reserve(struct xrs_tx_slot *s, u8 slot, u8 linux_queue)
{
 if (s->phase != XRS_FREE) return -EBUSY;
 if (slot >= 16 || linux_queue > 3) return -EINVAL;
 /* Never wrap a generation and accept an arbitrarily delayed old completion. */
 if (s->generation >= 4095) return -EOVERFLOW;
 ++s->generation;
 s->cookie = slot | ((u32)(s->generation & 255) << 8) |
             ((u32)linux_queue << 16) | ((u32)(s->generation >> 8) << 28);
 s->phase = XRS_QUEUED; s->confirmed = s->poisoned = 0; s->status = 0;
 return 0;
}
static inline int xrs_tx_begin(struct xrs_tx_slot *s)
{
 if (s->phase != XRS_QUEUED) return -EINVAL;
 s->phase = XRS_SENDING;
 return 0;
}
static inline int xrs_tx_confirmed(struct xrs_tx_slot *s, u32 cookie, u32 status)
{
 if (cookie != s->cookie || s->confirmed ||
     (s->phase != XRS_SENDING && s->phase != XRS_INFLIGHT)) return 0;
 s->confirmed = 1; s->status = status;
 if (s->phase == XRS_SENDING) return 0;
 s->phase = XRS_DONE;
 return 1;
}
static inline int xrs_tx_written(struct xrs_tx_slot *s, int failed)
{
 if (s->phase != XRS_SENDING) return 0;
 if (failed) { s->poisoned = 1; s->status = 0xffffffff; }
 if (failed || s->confirmed) { s->phase = XRS_DONE; return 1; }
 s->phase = XRS_INFLIGHT;
 return 0;
}
static inline int xrs_tx_abort(struct xrs_tx_slot *s, int transport_quiesced)
{
 if (s->phase == XRS_FREE || s->phase == XRS_DONE) return 0;
 if (s->phase != XRS_QUEUED && !transport_quiesced) return -EBUSY;
 s->poisoned = s->phase != XRS_QUEUED;
 s->status = 0xffffffff; s->phase = XRS_DONE;
 return 1;
}
static inline int xrs_tx_released(struct xrs_tx_slot *s)
{
 if (s->phase != XRS_DONE) return -EINVAL;
 s->phase = XRS_FREE;
 return 0;
}
#endif

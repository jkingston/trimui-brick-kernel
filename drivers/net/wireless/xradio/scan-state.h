/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XR829_SCAN_STATE_H
#define XR829_SCAN_STATE_H
/* Caller serializes every transition. Shared by driver and host race tests. */
enum xr829_scan_phase {
 XR_SCAN_IDLE, XR_SCAN_QUEUED, XR_SCAN_SUBMITTING, XR_SCAN_ACTIVE,
 XR_SCAN_COMPLETING, XR_SCAN_POISONED,
};
struct xr829_scan_state {
 enum xr829_scan_phase phase;
 bool pending, aborted, poison;
};
static inline bool xr829_scan_begin(struct xr829_scan_state *s)
{
 if (s->phase != XR_SCAN_IDLE)
  return false;
 *s = (struct xr829_scan_state){ .phase = XR_SCAN_SUBMITTING };
 return true;
}
static inline bool xr829_scan_event(struct xr829_scan_state *s,
                                   bool aborted, bool poison)
{
 if (s->phase != XR_SCAN_SUBMITTING && s->phase != XR_SCAN_ACTIVE)
  return false;
 s->pending = true;
 s->aborted |= aborted;
 s->poison |= poison;
 if (s->phase == XR_SCAN_SUBMITTING)
  return false;
 s->phase = XR_SCAN_COMPLETING;
 return true;
}
static inline bool xr829_scan_submitted(struct xr829_scan_state *s, bool failed)
{
 if (s->phase != XR_SCAN_SUBMITTING)
  return false;
 s->phase = XR_SCAN_ACTIVE;
 if (failed || s->pending)
  return xr829_scan_event(s, failed, failed);
 return false;
}
static inline void xr829_scan_notified(struct xr829_scan_state *s)
{
 if (s->phase == XR_SCAN_COMPLETING)
  s->phase = s->poison ? XR_SCAN_POISONED : XR_SCAN_IDLE;
}
#endif

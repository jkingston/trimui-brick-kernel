/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XPC_POLICY_CACHE_H
#define XPC_POLICY_CACHE_H
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
typedef u8 xpc_u8;
typedef u16 xpc_u16;
typedef u64 xpc_u64;
#else
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef uint8_t xpc_u8;
typedef uint16_t xpc_u16;
typedef uint64_t xpc_u64;
#endif
#define XPC_SERIAL_MAX (~(xpc_u64)0)
#define XPC_SLOTS 16
#define XPC_LEASES 256
/* Caller serializes all calls. This layer never sleeps or submits commands. */
struct xpc_key { xpc_u16 rates[6]; xpc_u8 short_retry,long_retry,flags,recovery; };
struct xpc_handle { xpc_u64 generation,serial; unsigned lease; };
struct xpc_ticket { xpc_u64 generation,serial; unsigned slot; };
enum xpc_state { XPC_EMPTY, XPC_RESERVED, XPC_UPLOADING, XPC_READY };
struct xpc_slot { struct xpc_key key; xpc_u64 serial; unsigned refs; enum xpc_state state; };
struct xpc_lease { xpc_u64 serial; unsigned slot; bool used,published; };
struct xpc_cache { struct xpc_slot slots[XPC_SLOTS]; struct xpc_lease leases[XPC_LEASES]; xpc_u64 generation,serial; unsigned evict_next; bool poisoned; };
enum { XPC_OK=0, XPC_WAIT=1, XPC_BUSY=-1, XPC_STALE=-2, XPC_FAILED=-3, XPC_INVALID=-4 };
static inline void xpc_init(struct xpc_cache *c) { memset(c,0,sizeof(*c));c->generation=1; }
/* Call only after confirmed firmware reset/drain; invalidate every old lease. */
static inline int xpc_reset(struct xpc_cache *c)
{
 xpc_u64 generation=c->generation;
 if(generation==XPC_SERIAL_MAX) {c->poisoned=true;return XPC_FAILED;}
 memset(c,0,sizeof(*c));c->generation=generation+1;return XPC_OK;
}
static inline bool xpc_equal(const struct xpc_key *a,const struct xpc_key *b)
{
 return !memcmp(a->rates,b->rates,sizeof(a->rates)) && a->short_retry==b->short_retry &&
 a->long_retry==b->long_retry && a->flags==b->flags && a->recovery==b->recovery;
}
static inline int xpc_next(struct xpc_cache *c,xpc_u64 *serial)
{
 if(c->serial==XPC_SERIAL_MAX) {c->poisoned=true;return XPC_FAILED;}
 *serial=++c->serial;return XPC_OK;
}
static inline struct xpc_lease *xpc_lookup(struct xpc_cache *c,struct xpc_handle h)
{
 struct xpc_lease *l;
 if(h.generation!=c->generation || h.lease>=XPC_LEASES)return 0;
 l=&c->leases[h.lease];return l->used && l->serial==h.serial?l:0;
}
/* A lease pins a queue entry through final completion/abort. WAIT means the
 * selected policy is not confirmed; caller must defer publication. */
static inline int xpc_reserve(struct xpc_cache *c,const struct xpc_key *key,struct xpc_handle *out)
{
 unsigned i,slot=XPC_SLOTS,lease=XPC_LEASES;xpc_u64 serial;
 if(c->poisoned)return XPC_FAILED;
 for(i=0;i<XPC_LEASES;i++)if(!c->leases[i].used){lease=i;break;}
 if(lease==XPC_LEASES)return XPC_BUSY;
 for(i=0;i<XPC_SLOTS;i++)if(c->slots[i].state!=XPC_EMPTY && xpc_equal(&c->slots[i].key,key)){slot=i;break;}
 if(slot==XPC_SLOTS) {
  /* Populate unused capacity before evicting a confirmed cached policy. */
  for(i=0;i<XPC_SLOTS;i++)if(c->slots[i].state==XPC_EMPTY){slot=i;break;}
  if(slot==XPC_SLOTS) {
   /* Round-robin eviction avoids repeatedly replacing the first free slot.
    * Referenced and unresolved-upload slots remain strictly ineligible. */
   for(i=0;i<XPC_SLOTS;i++) {
    unsigned candidate=(c->evict_next+i)%XPC_SLOTS;
    if(!c->slots[candidate].refs && c->slots[candidate].state!=XPC_UPLOADING) {
     slot=candidate;break;
    }
   }
  }
  if(slot==XPC_SLOTS)return XPC_BUSY;
  if(xpc_next(c,&serial))return XPC_FAILED;
  c->slots[slot]=(struct xpc_slot){.key=*key,.serial=serial,.state=XPC_RESERVED};
  c->evict_next=(slot+1)%XPC_SLOTS;
 }
 if(xpc_next(c,&serial))return XPC_FAILED;
 c->leases[lease]=(struct xpc_lease){.used=true,.slot=slot,.serial=serial};
 c->slots[slot].refs++;
 *out=(struct xpc_handle){.generation=c->generation,.serial=serial,.lease=lease};
 return c->slots[slot].state==XPC_READY?XPC_OK:XPC_WAIT;
}
/* Worker claims one upload. Copy immutable key/ticket under the same lock,
 * then submit outside it. A second worker cannot claim the same slot. */
static inline int xpc_begin_upload(struct xpc_cache *c,struct xpc_handle h,struct xpc_ticket *out,struct xpc_key *key)
{
 struct xpc_lease *l=xpc_lookup(c,h);struct xpc_slot *s;
 if(!l)return XPC_STALE;
 if(c->poisoned)return XPC_FAILED;
 s=&c->slots[l->slot];if(s->state!=XPC_RESERVED)return XPC_INVALID;
 s->state=XPC_UPLOADING;*key=s->key;
 *out=(struct xpc_ticket){.generation=c->generation,.serial=s->serial,.slot=l->slot};
 return XPC_OK;
}
/* False includes error, timeout, or uncertain command result. A fresh
 * confirmed firmware reset is required before any further publication. */
static inline int xpc_finish_upload(struct xpc_cache *c,struct xpc_ticket t,bool confirmed)
{
 struct xpc_slot *s;
 if(t.generation!=c->generation || t.slot>=XPC_SLOTS)return XPC_STALE;
 s=&c->slots[t.slot];if(t.serial!=s->serial || s->state!=XPC_UPLOADING)return XPC_STALE;
 if(c->poisoned)return XPC_FAILED;
 if(!confirmed){c->poisoned=true;return XPC_FAILED;}
 s->state=XPC_READY;return XPC_OK;
}
/* Commit this lease for one frame only; retains reference through completion. */
static inline int xpc_publish(struct xpc_cache *c,struct xpc_handle h,unsigned *slot)
{
 struct xpc_lease *l=xpc_lookup(c,h);
 if(!l)return XPC_STALE;
 if(c->poisoned)return XPC_FAILED;
 if(l->published)return XPC_INVALID;
 if(c->slots[l->slot].state!=XPC_READY)return XPC_WAIT;
 l->published=true;*slot=l->slot;return XPC_OK;
}
static inline int xpc_release(struct xpc_cache *c,struct xpc_handle h)
{
 struct xpc_lease *l=xpc_lookup(c,h);
 if(!l)return XPC_STALE;
 c->slots[l->slot].refs--;l->used=false;return XPC_OK;
}
#endif

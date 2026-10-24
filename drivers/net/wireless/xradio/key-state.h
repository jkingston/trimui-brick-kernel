/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_KEY_STATE_H
#define XRS_KEY_STATE_H
#include "key-wire.h"
/* Serialized by caller's control lock. No key bytes are retained here.
 * A nonzero owner is a session-unique identity, not the firmware slot number.
 * Failed commands make firmware ownership uncertain until verified reset.
 */
enum xrs_key_phase { XRS_KEY_FREE, XRS_KEY_RESERVED, XRS_KEY_INSTALLING, XRS_KEY_LIVE,
                     XRS_KEY_REMOVING, XRS_KEY_UNCERTAIN };
struct xrs_key_slot { enum xrs_key_phase phase; unsigned long long owner; };
struct xrs_keys { struct xrs_key_slot slots[XRS_KEY_SLOT_MAX+1]; int poisoned; };
static inline int xrs_key_install_begin(struct xrs_keys *keys,
                                        unsigned long long owner)
{
 unsigned int i;
 if(keys->poisoned)return -EIO;
 if(!owner)return -EINVAL;
 for(i=0;i<=XRS_KEY_SLOT_MAX;i++)
  if(keys->slots[i].phase!=XRS_KEY_FREE && keys->slots[i].owner==owner)
   return -EALREADY; /* do not reinstall/reset an existing key's PN */
 for(i=0;i<=XRS_KEY_SLOT_MAX;i++)if(keys->slots[i].phase==XRS_KEY_FREE) {
  keys->slots[i].owner=owner;keys->slots[i].phase=XRS_KEY_RESERVED;return i;
 }
 return -ENOSPC;
}
/* Local validation failed before any command publication: safe to release. */
static inline int xrs_key_install_cancel(struct xrs_keys *keys,unsigned int slot,
                                         unsigned long long owner)
{
 if(slot>XRS_KEY_SLOT_MAX || keys->slots[slot].owner!=owner ||
    keys->slots[slot].phase!=XRS_KEY_RESERVED)return -EINVAL;
 keys->slots[slot].phase=XRS_KEY_FREE;keys->slots[slot].owner=0;return 0;
}
/* Call immediately before publishing the command, while serialized. */
static inline int xrs_key_install_publish(struct xrs_keys *keys,unsigned int slot,
                                          unsigned long long owner)
{
 if(keys->poisoned)return -EIO;
 if(slot>XRS_KEY_SLOT_MAX || !owner || keys->slots[slot].owner!=owner ||
    keys->slots[slot].phase!=XRS_KEY_RESERVED)return -EINVAL;
 keys->slots[slot].phase=XRS_KEY_INSTALLING;return 0;
}
static inline int xrs_key_remove_begin(struct xrs_keys *keys,unsigned int slot,
                                       unsigned long long owner)
{
 if(keys->poisoned)return -EIO;
 if(slot>XRS_KEY_SLOT_MAX || !owner || keys->slots[slot].owner!=owner ||
    keys->slots[slot].phase!=XRS_KEY_LIVE)return -EINVAL;
 keys->slots[slot].phase=XRS_KEY_REMOVING;return 0;
}
static inline int xrs_key_complete(struct xrs_keys *keys,unsigned int slot,
                                   unsigned long long owner,int result)
{
 struct xrs_key_slot *s;
 if(slot>XRS_KEY_SLOT_MAX || !owner)return -EINVAL;
 s=&keys->slots[slot];
 if(s->owner!=owner || (s->phase!=XRS_KEY_INSTALLING &&
                       s->phase!=XRS_KEY_REMOVING))return -EINVAL;
 if(result) {
  s->phase=XRS_KEY_UNCERTAIN;keys->poisoned=1;
  return result<0?result:-EIO;
 }
 if(keys->poisoned) {s->phase=XRS_KEY_UNCERTAIN;return -EIO;}
 if(s->phase==XRS_KEY_INSTALLING)s->phase=XRS_KEY_LIVE;
 else {s->phase=XRS_KEY_FREE;s->owner=0;}
 return 0;
}
static inline int xrs_key_usable(const struct xrs_keys *keys,unsigned int slot,
                                 unsigned long long owner)
{
 return !keys->poisoned && owner && slot<=XRS_KEY_SLOT_MAX &&
        keys->slots[slot].phase==XRS_KEY_LIVE && keys->slots[slot].owner==owner;
}
#endif

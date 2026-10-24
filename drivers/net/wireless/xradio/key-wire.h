/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_KEY_WIRE_H
#define XRS_KEY_WIRE_H
#include "xr829-wire.h"
/* Payload only: WSM header is added by the command transport. No native union
 * padding, implicit sequence conversion, key logging or command submission.
 */
#define XRS_KEY_PAYLOAD_SIZE 44
#define XRS_KEY_SLOT_MAX 10
static inline int xrs_ccmp_pairwise_encode(u8 *out, size_t capacity, u8 slot,
                                           const u8 peer[6], const u8 *key,
                                           size_t key_size)
{
 unsigned int i, nonzero=0;
 if(capacity<XRS_KEY_PAYLOAD_SIZE)return -EMSGSIZE;
 if(slot>XRS_KEY_SLOT_MAX || key_size!=16 || !peer || !key || !out)return -EINVAL;
 for(i=0;i<6;i++)nonzero|=peer[i];
 if(!nonzero || (peer[0]&1))return -EINVAL;
 memset(out,0,XRS_KEY_PAYLOAD_SIZE);
 out[0]=5;out[1]=slot;
 memcpy(out+4,peer,6);memcpy(out+12,key,16);
 return XRS_KEY_PAYLOAD_SIZE;
}
/* Sequence is deliberately an eight-byte firmware-format input. Its conversion
 * from mac80211 PN and firmware replay semantics still require evidence; callers
 * must not silently zero an existing group's replay state.
 */
static inline int xrs_ccmp_group_encode(u8 *out, size_t capacity, u8 slot,
                                        u8 key_id, const u8 *key, size_t key_size,
                                        const u8 sequence_wire[8])
{
 if(capacity<XRS_KEY_PAYLOAD_SIZE)return -EMSGSIZE;
 if(slot>XRS_KEY_SLOT_MAX || key_id>3 || key_size!=16 ||
    !key || !sequence_wire || !out)return -EINVAL;
 memset(out,0,XRS_KEY_PAYLOAD_SIZE);
 out[0]=4;out[1]=slot;memcpy(out+4,key,16);out[20]=key_id;
 memcpy(out+24,sequence_wire,8);
 return XRS_KEY_PAYLOAD_SIZE;
}
static inline int xrs_key_remove_encode(u8 *out, size_t capacity, u8 slot)
{
 if(capacity<4)return -EMSGSIZE;
 if(slot>XRS_KEY_SLOT_MAX || !out)return -EINVAL;
 memset(out,0,4);out[0]=slot;return 4;
}
#endif

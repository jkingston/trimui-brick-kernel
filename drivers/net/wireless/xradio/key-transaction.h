/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_KEY_TRANSACTION_H
#define XRS_KEY_TRANSACTION_H
#include "key-state.h"
/* Caller holds its sleeping control lock. send must copy the borrowed payload
 * before returning, even on timeout: firmware/SDIO may not retain this pointer.
 * Key ownership is never inferred from command publication alone.
 */
typedef int (*xrs_key_send_fn)(void *context,u16 command,const u8 *payload,size_t size);
static inline void xrs_key_wipe(u8 *data,size_t size)
{
#ifdef __KERNEL__
 memzero_explicit(data,size);
#else
 volatile u8 *p=data;
 while(size--)*p++=0;
#endif
}
static inline int xrs_key_install(struct xrs_keys *keys,unsigned long long owner,
                                  const u8 *peer,u8 key_id,const u8 *key,
                                  size_t key_size,const u8 *group_sequence,
                                  xrs_key_send_fn send,void *context)
{
 u8 payload[XRS_KEY_PAYLOAD_SIZE];
 int slot,ret;
 if(!send)return -EINVAL;
 slot=xrs_key_install_begin(keys,owner);
 if(slot<0)return slot;
 if(peer)
  ret=xrs_ccmp_pairwise_encode(payload,sizeof(payload),slot,peer,key,key_size);
 else
  ret=xrs_ccmp_group_encode(payload,sizeof(payload),slot,key_id,key,key_size,group_sequence);
 if(ret<0) {
  xrs_key_wipe(payload,sizeof(payload));
  xrs_key_install_cancel(keys,slot,owner);return ret;
 }
 ret=xrs_key_install_publish(keys,slot,owner);
 if(ret) {
  xrs_key_wipe(payload,sizeof(payload));
  xrs_key_install_cancel(keys,slot,owner);return ret;
 }
 ret=send(context,0x000c,payload,sizeof(payload));
 xrs_key_wipe(payload,sizeof(payload));
 ret=xrs_key_complete(keys,slot,owner,ret);
 return ret?ret:slot;
}
static inline int xrs_key_remove(struct xrs_keys *keys,unsigned int slot,
                                 unsigned long long owner,xrs_key_send_fn send,
                                 void *context)
{
 u8 payload[4];int ret;
 if(!send)return -EINVAL;
 /* Validate/encode before transitioning the known live key. */
 if(slot>XRS_KEY_SLOT_MAX)return -EINVAL;
 ret=xrs_key_remove_encode(payload,sizeof(payload),slot);
 if(ret<0)return ret;
 ret=xrs_key_remove_begin(keys,slot,owner);if(ret)return ret;
 ret=send(context,0x000d,payload,sizeof(payload));
 return xrs_key_complete(keys,slot,owner,ret);
}
#endif

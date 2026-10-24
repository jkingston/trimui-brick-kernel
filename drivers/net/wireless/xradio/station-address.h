/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_STATION_ADDRESS_H
#define XRS_STATION_ADDRESS_H
#include "xr829-wire.h"
/* control_lock serializes all operations, including the synchronous sender. */
struct xrs_address {
 u8 desired[6],confirmed[6];
 unsigned char installed,valid,poisoned;
};
static inline int xrs_address_valid(const u8 *address)
{
 unsigned int i;u8 any=0;
 if(address[0]&1)return 0;
 for(i=0;i<6;i++)any|=address[i];
 return any!=0;
}
static inline void xrs_address_reset(struct xrs_address *state)
{ state->valid=0; }
static inline void xrs_address_remove(struct xrs_address *state)
{
 state->installed=0;state->valid=0;
 memset(state->desired,0,6);memset(state->confirmed,0,6);
 /* Never clear uncertainty without a new hardware lifetime. */
}
static inline int xrs_address_apply(struct xrs_address *state,const u8 *address,
                                    int install,
                                    int (*send)(void *,const u8 *),void *context)
{
 int ret;
 if(state->poisoned)return -EIO;
 if(!xrs_address_valid(address))return -EADDRNOTAVAIL;
 if(install ? state->installed :
    (!state->installed || memcmp(state->desired,address,6)))return -EINVAL;
 /* Reassert explicitly even after RESET or when restoring permanent MAC. */
 state->valid=0;
 ret=send(context,address);
 if(ret) {state->poisoned=1;return ret;}
 memcpy(state->desired,address,6);memcpy(state->confirmed,address,6);
 state->installed=1;state->valid=1;
 return 0;
}
#endif

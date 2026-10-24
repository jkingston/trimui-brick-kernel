/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_KEY_COMMAND_H
#define XRS_KEY_COMMAND_H
#include "key-wire.h"
static inline int xrs_key_command_allowed(u16 command,const u8 *data,size_t size)
{
 const u8 *p;unsigned int i,nonzero=0;
 if(command==13) return size==8 && data[4]<=10 && !data[5] && !data[6] && !data[7];
 if(command!=12 || size!=48)return 0;
 p=data+4;
 if(p[1]>10 || p[2] || p[3])return 0;
 if(p[0]==5) {
  for(i=4;i<10;i++)nonzero|=p[i];
  if(!nonzero || (p[4]&1) || p[10] || p[11])return 0;
  for(i=28;i<44;i++)if(p[i])return 0;
 } else if(p[0]==4) {
  if(p[20]>3 || p[21] || p[22] || p[23])return 0;
  for(i=32;i<44;i++)if(p[i])return 0;
 } else return 0;
 return 1;
}
#endif

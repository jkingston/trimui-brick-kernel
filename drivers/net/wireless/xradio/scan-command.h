/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_SCAN_COMMAND_H
#define XRS_SCAN_COMMAND_H
#include "xr829-wire.h"
#include "station-address.h"
#define XRS_SCAN_IE_LIMIT 512
static inline int xrs_scan_command_valid(const u8 *data,size_t size)
{
 unsigned int channels,ssids,i,j;size_t offset;
 if(size<16)return 0;
 channels=data[13];ssids=data[14];
 if(data[4] || data[5] || xrw_get32(data+8) ||
    (xrw_get16(data+6)!=0 && xrw_get16(data+6)!=0x60) ||
    !channels || channels>14 || ssids>2 ||
    (data[12]!=0 && data[12]!=2) ||
    (!data[12] && ssids) || (data[12] && !ssids) ||
    size!=16U+channels*16U+ssids*36U)return 0;
 for(i=0,offset=16;i<channels;i++,offset+=16) {
  u16 channel=xrw_get16(data+offset);
  u32 minimum=xrw_get32(data+offset+4),maximum=xrw_get32(data+offset+8);
  if(!channel || channel>14 || xrw_get16(data+offset+2) ||
     !minimum || minimum>maximum || maximum>1000 ||
     xrw_get32(data+offset+12))return 0;
 }
 for(i=0;i<ssids;i++,offset+=36) {
  u32 len=xrw_get32(data+offset);
  if(len>32)return 0;
  for(j=len;j<32;j++)if(data[offset+4+j])return 0;
 }
 return 1;
}
/* MIB payload only: LE16 template type, LE16 rate entry, LE32 frame size, frame. */
static inline int xrs_probe_template_valid(const u8 *data,size_t size)
{
 const u8 *frame;size_t len,pos;unsigned int i;int broadcast=1;
 if(size<34 || size>34+XRS_SCAN_IE_LIMIT || xrw_get16(data) ||
    (xrw_get16(data+2)!=0 && xrw_get16(data+2)!=0x60))return 0;
 len=xrw_get32(data+4);frame=data+8;
 if(len!=size-8 || xrw_get16(frame)!=0x0040 ||
    !xrs_address_valid(frame+10) || frame[24]!=0 || frame[25]!=0)return 0;
 for(i=0;i<6;i++)if(frame[4+i]!=0xff)broadcast=0;
 if(!broadcast && !xrs_address_valid(frame+4))return 0;
 if(memcmp(frame+4,frame+16,6))return 0;
 for(pos=26;pos<len;) {
  if(len-pos<2 || frame[pos+1]>len-pos-2)return 0;
  /* SSID is supplied via scan SSID records, not extra template IEs. */
  if(frame[pos]==0)return 0;
  pos+=2+frame[pos+1];
 }
 return 1;
}
#endif

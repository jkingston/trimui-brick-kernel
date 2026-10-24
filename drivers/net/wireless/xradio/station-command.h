/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_STATION_COMMAND_H
#define XRS_STATION_COMMAND_H
#include "xr829-wire.h"
#include "station-address.h"
#include "scan-command.h"
/* Publication identity is checked under the same lock as command replacement. */
static inline int xrs_command_copy_current(unsigned long long current,
                                           unsigned long long copied)
{
 return current==copied;
}
/* Exact WSM lengths include4-byte header; MIB lengths include its4-byte prefix.
 * Scan's deeper semantic validation remains in wsm_scan(). No key/AP commands.
 */
static inline int xrs_command_allowed(u16 cmd,const u8 *data,size_t size,int station)
{
 u16 mib,length;
 if(size<4)return 0;
 if(cmd==7)return xrs_scan_command_valid(data,size) && (station || !data[12]);
 if(cmd==9)return size==772;
 if(station) {
  /* VIF0 RESET, preserve statistics; reject link IDs and reserved bits. */
  if(cmd==0x000a)return size==8 && xrw_get32(data+4)==1;
  if(cmd==0x000b)return size==64;
  if(cmd==0x0011)return size==12;
  if(cmd==0x0012)return size==16;
  if(cmd==0x0013)return size==48;
 }
 if(cmd!=6 || size<8)return 0;
 mib=xrw_get16(data+4);length=xrw_get16(data+6);
 if(size!=8U+length)return 0;
 if(mib==0x1006 || mib==0x1009)return length==4;
 if(!station)return 0;
 switch(mib) {
 case 0x1002:return xrs_probe_template_valid(data+8,length);
 case 0x1046:return length==6 && xrs_address_valid(data+8); /* station MAC */
 case 0x1016:return length==24; /* six-entry retry policy */
 case 0x1021:return length==4; /* HT protection */
 case 0x1010:return length==12; /* XR829 association mode, explicit padding */
 case 0x1005:return length==8; /* beacon filter enable+count */
 case 0x0006: /* output power, 0.1dBm */
 case 0x0003: /* slot time, microseconds */
 case 0x100e: /* block ACK policy */
 case 0x1020: /* protected management policy */
 case 0x1003: /* receive filter */
 case 0x1026: /* BSSID filter disable */
  return length==4;
 default:return 0;
 }
}
#endif

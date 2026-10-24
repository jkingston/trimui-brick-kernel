/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_CCMP_RX_H
#define XRS_CCMP_RX_H
#include "station-wire.h"
struct xrs_ccmp_rx_view {
 struct xrs_rx_view frame;
 u8 key_id, group;
};
/* Firmware-decrypted CCMP only. Caller supplies a serialized snapshot of keys
 * confirmed installed in this association, excluding removing/uncertain keys.
 * No bytes are stripped or replay state updated here. Retained CCMP header/MIC
 * go to mac80211; never assert PN_VALIDATED, IV_STRIPPED or ALLOW_SAME_PN.
 */
static inline int xrs_ccmp_rx_decode(struct xrs_ccmp_rx_view *out,const u8 *data,
                                     size_t size,const u8 bssid[6],
                                     int pairwise_live,u8 group_live_mask)
{
 struct xrs_ccmp_rx_view v={0};
 const u8 *frame,*ccmp;u16 fc;int header;
 if(size<16+24+8+8 || size>16+26+8+3839+8)return -EMSGSIZE;
 xrw_rx_decode(&v.frame.radio,data,size);
 /* NO_KEY_FOUND/MIC failure are errors, regardless of the encryption flags. */
 if(v.frame.radio.status || v.frame.radio.channel<1 || v.frame.radio.channel>14 ||
    xrs_rate_entry_decode(&v.frame.rate,v.frame.radio.rate) ||
    (v.frame.radio.flags&7)!=3 || (v.frame.radio.flags&0x78))
  return -EOPNOTSUPP; /* aggregate/defragmented unsupported */
 frame=data+16;fc=xrw_get16(frame);
 header=xrs_rx_data_header_size(frame,size-16);
 if(header<0)return header;
 v.frame.amsdu=xrs_rx_amsdu(frame,header);
 if(size<16+(size_t)header+8+8 || size>16+(size_t)header+8+(v.frame.amsdu?3839:2304)+8)return -EMSGSIZE;
 if((fc&3) || (fc&0x000c)!=8 ||
    (fc&0x0300)!=0x0200 || !(fc&0x4000) || (fc&0x0400) ||
    (xrw_get16(frame+22)&15) || memcmp(frame+10,bssid,6))return -EINVAL;
 ccmp=frame+header;
 /* Reserved octet/bits clear and ExtIV set; upper two bits are key ID. */
 if(ccmp[2] || (ccmp[3]&0x3f)!=0x20)return -EINVAL;
 v.key_id=ccmp[3]>>6;v.group=!!(frame[4]&1);
 if(v.group) {
  if(!(group_live_mask&(1U<<v.key_id)))return -ENOKEY;
 } else if(v.key_id || !pairwise_live)return -ENOKEY;
 v.frame.frame_size=size-16;
 v.frame.signal_dbm=v.frame.radio.rssi/2-110;
 if(v.frame.signal_dbm>0)v.frame.signal_dbm=0;
 v.frame.rate_index=v.frame.rate.index;
 *out=v;return 16;
}
#endif

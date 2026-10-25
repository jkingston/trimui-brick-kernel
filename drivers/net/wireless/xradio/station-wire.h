/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XR829_STATION_WIRE_H
#define XR829_STATION_WIRE_H
#include "xr829-wire.h"
#include "station-ba.h"
#include "rate-entry.h"
/* All offsets in this file are explicit wire offsets, never native bitfields.
 * TX request includes WSM header. TX confirmation excludes WSM header.
 */
#define XRS_TX_SIZE 24
#define XRS_TX_CONFIRM_SIZE 32
#define XRS_POLICY_SIZE 24
struct xrs_tx {
 u32 packet_id, expiry;
 u16 rate;
 u8 queue, priority, policy;
};
static inline int xrs_legacy_rate(u16 rate)
{
 unsigned int index = (rate >> 4) & 15;
 return !(rate & ~0x01ff) && index != 4 && index != 5 && index <= 13;
}
/* Unfragmented three-address data headers. QoS priorities 0..7 only;
 * TX A-MSDU, alternate ACK policies and HT-control remain unsupported.
 * RX accepts a complete unicast A-MSDU for mac80211 deaggregation.
 */
static inline int xrs_data_header_size_mode(const u8 *frame,size_t size,int rx)
{
 u16 fc;
 if(size<24)return -EMSGSIZE;
 fc=xrw_get16(frame);
 if((fc&3) || (fc&0x8400) || (fc&0x0300)==0x0300 ||
    (xrw_get16(frame+22)&15))return -EOPNOTSUPP;
 if((fc&0x00fc)==0x0008)return 24;
 if((fc&0x00fc)!=0x0088)return -EOPNOTSUPP;
 if(size<26)return -EMSGSIZE;
 if(frame[24]&(rx?0x68:0xe8))return -EOPNOTSUPP;
 if(rx && (frame[24]&0x80) && (frame[4]&1))return -EOPNOTSUPP;
 return 26;
}
static inline int xrs_data_header_size(const u8 *frame,size_t size)
{ return xrs_data_header_size_mode(frame,size,0); }
static inline int xrs_rx_data_header_size(const u8 *frame,size_t size)
{ return xrs_data_header_size_mode(frame,size,1); }
/* A-MSDU is one MPDU: retain its QoS bit and complete payload for mac80211.
 * MAX_AMSDU clear advertises a 3839-byte aggregate, not absence of support. */
static inline int xrs_rx_amsdu(const u8 *frame,int header)
{ return header==26 && !!(frame[24]&0x80); }
/* Aligned frame follows directly at offset24: no implicit two-byte pad.
 * Dedicated station VIF0/link0. BH must add only WSM sequence bits later.
 */
static inline int xrs_tx_encode_mode(u8 *out, size_t capacity, const struct xrs_tx *tx,
                                const u8 *frame, size_t frame_size,
                                size_t firmware_input_limit, int ccmp)
{
 size_t total;
 if (frame_size > 2304 + 36) return -EMSGSIZE;
 total = XRS_TX_SIZE + frame_size + (ccmp ? 8 : 0);
 u16 fc;int header=24;struct xrs_rate_entry selected;
 if (frame_size < 24 || frame_size > 2304 + 36 ||
     total > firmware_input_limit || total > 65535 || total > capacity)
  return -EMSGSIZE;
 if (xrs_rate_entry_decode(&selected,tx->rate) || !(tx->rate & 15) || tx->queue > 3 ||
     tx->priority > 7 || tx->policy > 15 ||
     (tx->packet_id & 0x0ff00000) || ((tx->packet_id >> 16) & 15) > 3)
  return -EINVAL;
 /* CCMP input includes the mac80211-generated IV,
  * but no MIC: append exactly eight zero bytes for firmware to fill. */
 fc = xrw_get16(frame);
 if ((fc & 3) || (fc & 0x0400) || (xrw_get16(frame + 22) & 15) ||
     (fc & 0x000c) == 4 || (fc & 0x000c) == 12)
  return -EOPNOTSUPP;
 if ((fc&0x000c)==8) {
  header=xrs_data_header_size(frame,frame_size);if(header<0)return header;
  if(!ccmp && frame_size>(size_t)header+2304)return -EMSGSIZE;
 }
 if (ccmp) {
  if (frame_size > (size_t)header + 8 + 2304) return -EMSGSIZE;
  if (frame_size < (size_t)header+8 || (fc & 0x437c) != 0x4108 ||
      (frame[4] & 1) || frame[header+2] || frame[header+3] != 0x20)
   return -EOPNOTSUPP;
 } else if (fc & 0x4000) return -EOPNOTSUPP;
 memset(out, 0, XRS_TX_SIZE);
 xrw_put16(out, total); xrw_put16(out + 2, 4);
 xrw_put32(out + 4, tx->packet_id);
 out[8] = tx->queue; out[10] = XRS_TX_SIZE; out[11] = tx->priority;
 xrw_put16(out + 12, tx->rate); out[14] = tx->policy;
 xrw_put32(out + 16, tx->expiry);
 memcpy(out + XRS_TX_SIZE, frame, frame_size);
 if (ccmp) memset(out + XRS_TX_SIZE + frame_size, 0, 8);
 return total;
}
/* Open transport stays fail-closed for protected frames. */
static inline int xrs_tx_encode(u8 *out, size_t capacity, const struct xrs_tx *tx,
                                const u8 *frame, size_t size, size_t limit)
{
 return xrs_tx_encode_mode(out, capacity, tx, frame, size, limit, 0);
}
static inline int xrs_tx_ccmp_encode(u8 *out, size_t capacity, const struct xrs_tx *tx,
                                     const u8 *frame, size_t size, size_t limit)
{
 return xrs_tx_encode_mode(out, capacity, tx, frame, size, limit, 1);
}
struct xrs_tx_confirm {
 u32 packet_id, status, media_delay, queue_delay;
 u16 rate, retries[6];
 u8 ack_failures, flags;
};
/* Vendor XR829 and cw1200 report status-success NO_EXPLICIT_ACK completions
 * as successful TX. This is an ABI compatibility mapping, not proof that an
 * on-air ACK was observed. Firmware-owned aggregate/BlockAck completions also
 * succeed, but do not expose enough per-MPDU retry information for rate feedback.
 */
static inline int xrs_tx_success_no_rate(const struct xrs_tx_confirm *c)
{
	return !c->status && (c->flags == 0x08 ||
		(!(c->flags & ~0x0d) && (c->flags & 0x0c) != 0x04 &&
		 ((c->flags & 1) || (c->flags & 0x0c) == 0x0c)));
}
static inline int xrs_tx_success(const struct xrs_tx_confirm *c)
{
 return !c->status && (!c->flags || xrs_tx_success_no_rate(c));
}
static inline int xrs_tx_confirm_decode(struct xrs_tx_confirm *out,
                                       const u8 *data, size_t size)
{
 struct xrs_tx_confirm c;
 unsigned int i;
 if (size != XRS_TX_CONFIRM_SIZE) return -EMSGSIZE;
 c.packet_id = xrw_get32(data); c.status = xrw_get32(data + 4);
 c.rate = xrw_get16(data + 8); c.ack_failures = data[10]; c.flags = data[11];
 for (i = 0; i < 6; i++) c.retries[i] = xrw_get16(data + 12 + 2 * i);
 c.media_delay = xrw_get32(data + 24); c.queue_delay = xrw_get32(data + 28);
 *out = c;
 return XRS_TX_CONFIRM_SIZE;
}
/* MIB1016: one policy. Six16-bit rate entries, zero terminates retry chain. */
static inline int xrs_policy_encode(u8 *out, size_t capacity, u8 index,
                                    u8 short_retry, u8 long_retry,
                                    const u16 rates[6])
{
 unsigned int i;
 int ended = 0;
 if (capacity < XRS_POLICY_SIZE) return -EMSGSIZE;
 if (index > 15 || short_retry > 15 || long_retry > 15 || !rates[0]) return -EINVAL;
 for (i = 0; i < 6; i++) {
  if (!rates[i]) ended = 1;
  else if (ended || !xrs_legacy_rate(rates[i]) || !(rates[i] & 15)) return -EINVAL;
 }
 memset(out, 0, XRS_POLICY_SIZE);
 out[0] = 1; out[4] = index; out[5] = short_retry; out[6] = long_retry;
 out[7] = 0x0c; /* stop at end; initial transmission counts in policy */
 for (i = 0; i < 6; i++) xrw_put16(out + 12 + 2 * i, rates[i]);
 return XRS_POLICY_SIZE;
}
/* 041e batched confirmations: countLE32 followed by count records. */
static inline int xrs_tx_batch_decode(struct xrs_tx_confirm *out, size_t count_max,
                                      const u8 *data, size_t size)
{
 u32 count, i;
 if (size < 4) return -EMSGSIZE;
 count = xrw_get32(data);
 if (!count || count > 16 || count > count_max) return -EINVAL;
 if (size != 4 + count * XRS_TX_CONFIRM_SIZE) return -EMSGSIZE;
 for (i = 0; i < count; i++)
  xrs_tx_confirm_decode(out + i, data + 4 + i * XRS_TX_CONFIRM_SIZE,
                        XRS_TX_CONFIRM_SIZE);
 return count;
}
/* SET_BSS_PARAMS0011: reserved8, beaconLostCount8, AID16, rateMask32. */
static inline int xrs_bss_encode(u8 *out, size_t size, u8 lost, u16 aid, u32 rates)
{
 if (size < 8) return -EMSGSIZE;
 if (!aid || aid > 2007 || !rates || (rates & ~0x3fcf)) return -EINVAL;
 out[0] = 0; out[1] = lost; xrw_put16(out + 2, aid); xrw_put32(out + 4, rates);
 return 8;
}
struct xrs_join_confirm { u32 status, minimum_power, maximum_power; };
static inline int xrs_join_confirm_decode(struct xrs_join_confirm *out,
                                          const u8 *data, size_t size)
{
 if (size != 12) return -EMSGSIZE;
 out->status = xrw_get32(data); out->minimum_power = xrw_get32(data + 4);
 out->maximum_power = xrw_get32(data + 8);
 return 12;
}
/* Initial RX gate: unencrypted, unfragmented station traffic only.
 * Return value16 is the frame offset; caller copies before releasing bus SKB.
 */
struct xrs_rx_view {
 struct xrw_rx radio;
 struct xrs_rate_entry rate;
 size_t frame_size;
 int signal_dbm;
 u8 rate_index, amsdu;
};
static inline int xrs_rx_open_decode(struct xrs_rx_view *out, const u8 *data,
                                     size_t size, const u8 bssid[6])
{
 struct xrs_rx_view v={0};
 const u8 *frame;
 u16 fc;
 if (size < 16 + 24 || size > 16 + 26 + 3839) return -EMSGSIZE;
 xrw_rx_decode(&v.radio, data, size);
	if (v.radio.status || v.radio.channel < 1 || v.radio.channel > 14 ||
	    xrs_rate_entry_decode(&v.rate, v.radio.rate) || (v.radio.flags & 7) ||
	    xrs_ba_rx_flags(v.radio.flags))
		return -EOPNOTSUPP;
 frame = data + 16; fc = xrw_get16(frame);
 if ((fc & 3) || (fc & 0x4400) || (xrw_get16(frame + 22) & 15))
  return -EOPNOTSUPP; /* version, more fragments, protected, fragment number */
 if ((fc & 0x000c) == 0) {
  if (size > 16+36+2304) return -EMSGSIZE;
  if ((fc & 0x0300) || memcmp(frame + 16, bssid, 6)) return -EINVAL;
 } else if ((fc & 0x000c) == 8) {
  int header=xrs_rx_data_header_size(frame,size-16);
  if(header<0)return header;
  v.amsdu=xrs_rx_amsdu(frame,header);
  if(size>16+(size_t)header+(v.amsdu?3839:2304))return -EMSGSIZE;
  if ((fc & 0x0300) != 0x0200 ||
      memcmp(frame + 10, bssid, 6)) return -EOPNOTSUPP;
 } else return -EOPNOTSUPP;
 v.frame_size = size - 16;
 v.signal_dbm = v.radio.rssi / 2 - 110;
 if (v.signal_dbm > 0) v.signal_dbm = 0;
 v.rate_index = v.rate.index;
 *out = v;
 return 16;
}
#endif

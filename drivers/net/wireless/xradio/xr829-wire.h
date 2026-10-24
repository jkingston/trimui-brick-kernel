/* SPDX-License-Identifier: GPL-2.0-only */
/* XR829 vendor SUPPORT_HT40 wire ABI, encoded without C bitfields/padding.
 */
#ifndef XR829_WIRE_H
#define XR829_WIRE_H
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#include <linux/errno.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

#define XR829_SCAN_PREFIX_SIZE 12
#define XR829_JOIN_SIZE 60
#define XR829_START_SIZE 52
#define XR829_ASSOC_SIZE 12
#define XR829_RX_SIZE 16

static inline void xrw_put16(u8 *p, u16 v)
{ p[0] = v; p[1] = v >> 8; }
static inline void xrw_put32(u8 *p, u32 v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline u16 xrw_get16(const u8 *p)
{ return (u16)p[0] | (u16)p[1] << 8; }
static inline u32 xrw_get32(const u8 *p)
{ return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* PHY modem flags, band bit8, preamble bits13:12. No HT40/SGI/GF/STBC. */
static inline int xrw_phy20(u8 band, u8 short_preamble, u8 ht, u16 *out)
{
 if (band > 1 || short_preamble > 1 || ht > 1) return -EINVAL;
 *out = (band ? 2 : 3) | (ht ? 4 : 0) | ((u16)band << 8) |
        ((u16)short_preamble << 12);
 return 0;
}
static inline int xrw_phy20_valid(u16 phy)
{
 return (phy & 3) && !(phy & ~0x1107);
}

struct xrw_scan {
 u8 band, type, flags, probes, channels, ssids, probe_delay;
 u16 rate;
 u32 interval;
};
static inline int xrw_scan_encode(u8 *out, size_t size, const struct xrw_scan *a)
{
 if (size < XR829_SCAN_PREFIX_SIZE) return -EMSGSIZE;
 if (a->band > 1 || a->type > 3 || (a->flags & 0xc0) ||
     a->channels > 48 || a->ssids > 2) return -EINVAL;
 /* Initial station scan accepts legacy20MHz rates1/2Mbps or6..54Mbps. */
 if ((a->rate & 0xff0f) || ((a->rate >> 4) > 1 && (a->rate >> 4) < 6) ||
     (a->rate >> 4) > 13) return -EOPNOTSUPP;
 memset(out, 0, XR829_SCAN_PREFIX_SIZE);
 out[0] = a->band; out[1] = a->flags | (a->type << 6);
 xrw_put16(out + 2, a->rate); xrw_put32(out + 4, a->interval);
 out[8] = a->probes; out[9] = a->channels;
 out[10] = a->ssids; out[11] = a->probe_delay;
 return XR829_SCAN_PREFIX_SIZE;
}
struct xrw_join {
 u16 phy, channel, atim;
 u8 bssid[6], mode, probe, dtim, flags, ssid[32];
 u32 ssid_len, beacon_interval, basic_rates;
};
static inline int xrw_join_encode(u8 *out, size_t size, const struct xrw_join *a)
{
 if (size < XR829_JOIN_SIZE) return -EMSGSIZE;
 if (a->ssid_len > 32 || !xrw_phy20_valid(a->phy)) return -EINVAL;
 memset(out, 0, XR829_JOIN_SIZE);
 xrw_put16(out, a->phy); xrw_put16(out + 2, a->channel);
 memcpy(out + 4, a->bssid, 6); xrw_put16(out + 10, a->atim);
 out[12] = a->mode; out[13] = a->probe; out[14] = a->dtim; out[15] = a->flags;
 xrw_put32(out + 16, a->ssid_len); memcpy(out + 20, a->ssid, a->ssid_len);
 xrw_put32(out + 52, a->beacon_interval); xrw_put32(out + 56, a->basic_rates);
 return XR829_JOIN_SIZE;
}
struct xrw_start {
 u16 phy, channel;
 u32 ct_window, beacon_interval, basic_rates;
 u8 mode, dtim, probe_delay, ssid_len, ssid[32];
};
static inline int xrw_start_encode(u8 *out, size_t size, const struct xrw_start *a)
{
 if (size < XR829_START_SIZE) return -EMSGSIZE;
 if (a->ssid_len > 32 || !xrw_phy20_valid(a->phy)) return -EINVAL;
 memset(out, 0, XR829_START_SIZE);
 xrw_put16(out, a->phy); xrw_put16(out + 2, a->channel);
 xrw_put32(out + 4, a->ct_window); xrw_put32(out + 8, a->beacon_interval);
 out[12] = a->mode; out[13] = a->dtim; out[14] = a->probe_delay;
 out[15] = a->ssid_len; memcpy(out + 16, a->ssid, a->ssid_len);
 xrw_put32(out + 48, a->basic_rates);
 return XR829_START_SIZE;
}
/* Vendor sends sizeof(unpacked AArch64 struct): flags0, pad1, PHY2,
 * spacing4, pad5..7, basicRateSet8. Intentionally preserve those12 bytes.
 */
static inline int xrw_assoc_encode(u8 *out, size_t size, u8 flags,
                                  u16 phy, u8 spacing, u32 basic_rates)
{
 if (size < XR829_ASSOC_SIZE) return -EMSGSIZE;
 if ((flags & ~7) || !xrw_phy20_valid(phy)) return -EINVAL;
 memset(out, 0, XR829_ASSOC_SIZE);
 out[0] = flags; xrw_put16(out + 2, phy); out[4] = spacing;
 xrw_put32(out + 8, basic_rates);
 return XR829_ASSOC_SIZE;
}
struct xrw_rx {
 u32 status, flags;
 u16 channel, rate;
 u8 rssi;
};
static inline int xrw_rx_decode(struct xrw_rx *out, const u8 *buf, size_t size)
{
 if (size < XR829_RX_SIZE) return -EMSGSIZE;
 out->status = xrw_get32(buf); out->channel = xrw_get16(buf + 4);
 out->rate = xrw_get16(buf + 6); out->rssi = buf[11];
 out->flags = xrw_get32(buf + 12);
 return XR829_RX_SIZE;
}
/* Vendor scan channel record: channel16, pad16, min32, max32, pad32. */
static inline int xrw_scan_channel_encode(u8 *out, size_t size, u16 channel,
                                         u32 minimum, u32 maximum)
{
 if (size < 16) return -EMSGSIZE;
 if (!channel || channel > 14 || !minimum || minimum > maximum) return -EINVAL;
 memset(out, 0, 16);
 xrw_put16(out, channel); xrw_put32(out + 4, minimum);
 xrw_put32(out + 8, maximum);
 return 16;
}
struct xrw_scan_complete { u32 status; u8 psm, channels; };
static inline int xrw_scan_complete_decode(struct xrw_scan_complete *out,
                                           const u8 *data, size_t size)
{
 if (size < 6) return -EMSGSIZE;
 out->status = xrw_get32(data); out->psm = data[4]; out->channels = data[5];
 return 6;
}
#endif

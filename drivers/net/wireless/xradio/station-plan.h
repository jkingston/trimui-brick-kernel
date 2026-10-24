/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XR829_STATION_PLAN_H
#define XR829_STATION_PLAN_H
#include "station-wire.h"
/* Pure preparation: no command publication. Association AID is supplied only
 * after mac80211 completes association; no firmware success is implied here.
 */
struct xrs_station_settings {
 u8 bssid[6], ssid[32], ssid_len, channel, dtim;
 u16 beacon_interval, aid;
 u32 basic_rates, operational_rates;
 unsigned char privacy;
};
struct xrs_station_plan {
 u8 join[60], policy[24], association[12], bss[8];
};
int xrs_station_prepare(struct xrs_station_plan *out,
                        const struct xrs_station_settings *settings);
#endif

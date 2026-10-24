/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_STATION_POWER_H
#define XRS_STATION_POWER_H
#include "xr829-wire.h"
/* Firmware limits and output use 0.1 dBm. The conservative bring-up ceiling
 * remains 10 dBm. Never raise a requested limit to the firmware minimum. */
static inline int xrs_power_limit(int channel_dbm,int configured_dbm,
                                  int bss_dbm,int firmware_min,int firmware_max,
                                  int *result)
{
 long long power=100,limit;
 if(firmware_min>firmware_max)return -EINVAL;
 limit=(long long)channel_dbm*10;if(limit<power)power=limit;
 limit=(long long)configured_dbm*10;if(limit<power)power=limit;
 /* INT_MIN is mac80211's unconfigured per-BSS sentinel. */
 if(bss_dbm!=(-2147483647-1)) {
  limit=(long long)bss_dbm*10;if(limit<power)power=limit;
 }
 if(firmware_max<power)power=firmware_max;
 if(power<firmware_min)return -ERANGE;
 *result=(int)power;return 0;
}
#endif

// SPDX-License-Identifier: GPL-2.0-only
#include "station-plan.h"
int xrs_station_prepare(struct xrs_station_plan *out,
                        const struct xrs_station_settings *settings)
{
 struct xrs_station_plan plan;
 struct xrw_join join = {};
 u16 rates[6] = {0};
 unsigned int i;
 u8 any = 0;
 int ret;
 if (settings->privacy > 1) return -EINVAL;
 for (i = 0; i < 6; i++) any |= settings->bssid[i];
 if (!any || (settings->bssid[0] & 1) || settings->ssid_len > 32 ||
     !settings->channel || settings->channel > 14 || !settings->dtim ||
     !settings->beacon_interval || !settings->basic_rates ||
     (settings->basic_rates & ~0x3fcf) ||
     (settings->basic_rates & ~settings->operational_rates)) return -EINVAL;
 ret=xrs_basic_rate_encode(&rates[0],settings->basic_rates,7);
 if(ret)return ret;
 join.phy = 3; join.channel = settings->channel; join.mode = 1;
 join.dtim = settings->dtim; join.ssid_len = settings->ssid_len;
 join.beacon_interval = settings->beacon_interval;
 join.basic_rates = settings->basic_rates;
 memcpy(join.bssid, settings->bssid, 6);
 memcpy(join.ssid, settings->ssid, settings->ssid_len);
 ret = xrw_join_encode(plan.join, sizeof(plan.join), &join);
 if (ret < 0) return ret;
 ret = xrs_policy_encode(plan.policy, sizeof(plan.policy), 0, 6, 6, rates);
 if (ret < 0) return ret;
 ret = xrw_assoc_encode(plan.association, sizeof(plan.association), 3, 3, 0,
                        settings->basic_rates);
 if (ret < 0) return ret;
 ret = xrs_bss_encode(plan.bss, sizeof(plan.bss), 20, settings->aid,
                      settings->operational_rates);
 if (ret < 0) return ret;
 *out = plan;
 return 0;
}

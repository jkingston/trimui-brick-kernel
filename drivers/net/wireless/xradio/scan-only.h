/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XR829_SCAN_ONLY_H
#define XR829_SCAN_ONLY_H
#include <linux/types.h>
struct xradio_common;
struct ieee80211_ops;
struct wiphy;
struct regulatory_request;
void xr829_scan_reg_notifier(struct wiphy *wiphy, struct regulatory_request *request);
extern const struct ieee80211_ops xr829_scan_ops;
bool xr829_scan_poisoned(struct xradio_common *hw);
int xr829_scan_quiesce(struct xradio_common *hw);
struct ieee80211_vif;
int xr829_restore_interface(struct xradio_common *hw,struct ieee80211_vif *vif);
void xr829_scan_new_epoch(struct xradio_common *hw);
void xr829_scan_init(struct xradio_common *hw);
void xr829_scan_done(struct xradio_common *hw, u32 status, u8 channels);
int xr829_scan_rx(struct xradio_common *hw, const u8 *data, size_t len);
#endif

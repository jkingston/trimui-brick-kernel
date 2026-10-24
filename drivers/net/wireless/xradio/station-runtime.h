/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_RUNTIME_H
#define XRS_RUNTIME_H
#include <linux/types.h>
#include <net/mac80211.h>
struct xradio_common; struct sk_buff; struct ieee80211_vif; struct ieee80211_hw;
struct ieee80211_prep_tx_info; struct ieee80211_bss_conf;
extern const bool allow_station;
extern bool allow_ht;
u8 *xrs_runtime_command_buffer(struct xradio_common *hw);
int xrs_runtime_init(struct xradio_common *hw);
void xrs_runtime_free(struct xradio_common *hw);
void xrs_runtime_quiesce(struct xradio_common *hw);
int xrs_runtime_detached(struct xradio_common *hw);
void xrs_runtime_new_epoch(struct xradio_common *hw);
int xrs_runtime_add_address(struct xradio_common *hw,const u8 *address);
void xrs_runtime_remove_address(struct xradio_common *hw);
void xrs_runtime_stop(struct xradio_common *hw);
void xrs_runtime_tx(struct xradio_common *hw, struct ieee80211_sta *sta, struct sk_buff *skb);
int xrs_runtime_get_tx(struct xradio_common *hw, u8 **data, size_t *len);
int xrs_runtime_written(struct xradio_common *hw, u8 *data, bool failed);
int xrs_runtime_confirm(struct xradio_common *hw, int id, const u8 *data, size_t size);
int xrs_runtime_rx(struct xradio_common *hw, const u8 *data, size_t size);
void xrs_mgd_prepare(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                      struct ieee80211_prep_tx_info *info);
void xrs_bss_changed(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                      struct ieee80211_bss_conf *info, u64 changed);
int xrs_runtime_config(struct xradio_common *hw,u32 changed);
int xrs_get_txpower(struct ieee80211_hw *,struct ieee80211_vif *,unsigned int,int *);
bool xrs_runtime_joined(struct xradio_common *hw);
int xrs_set_key(struct ieee80211_hw *,enum set_key_cmd,struct ieee80211_vif *,
                struct ieee80211_sta *,struct ieee80211_key_conf *);
int xrs_conf_tx(struct ieee80211_hw *,struct ieee80211_vif *,unsigned int,u16,
                const struct ieee80211_tx_queue_params *);
#endif

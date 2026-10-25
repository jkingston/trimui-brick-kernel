// SPDX-License-Identifier: GPL-2.0-only
/* Owned foreground scan requests with serialized firmware batches. */
#include <linux/firmware.h>
#include <linux/rtnetlink.h>
#include "xradio.h"
#include "station-runtime.h"
#include "recovery.h"
#include "scan-only.h"
#include "xr829-wire.h"

#include "scan-plan.h"
#include "scan-policy.h"
#include "station-power.h"

struct xr829_scan_request {
 struct xrs_scan_plan plan;
 struct wsm_scan_ch channels[XRS_SCAN_CHANNELS];
 struct xrs_scan_channel policy[XRS_SCAN_CHANNELS];
 u8 n_channels;
 struct wsm_ssid ssids[XRS_SCAN_SSIDS];
 struct sk_buff *probe;
 u8 batch, n_ssids, rate;
 int requested_power; /* accepted under wiphy mutex; invalidation aborts it */
};

static enum xr829_scan_phase scan_phase(struct xradio_common *hw)
{
 unsigned long flags;
 enum xr829_scan_phase phase;
 spin_lock_irqsave(&hw->scan_only_lock, flags);
 phase = hw->scan_only_state.phase;
 spin_unlock_irqrestore(&hw->scan_only_lock, flags);
 return phase;
}

bool xr829_scan_poisoned(struct xradio_common *hw)
{
 return scan_phase(hw) == XR_SCAN_POISONED;
}

/* Queue under the state lock: quiesce cannot miss a producer that has
 * already observed an accepted request. Only the worker sends commands,
 * releases the copied request, and publishes stack completion. */
static void finish_scan(struct xradio_common *hw, bool abort, bool poison)
{
 unsigned long flags;
 struct xr829_scan_state *s = &hw->scan_only_state;
 spin_lock_irqsave(&hw->scan_only_lock, flags);
 if (s->phase != XR_SCAN_IDLE && s->phase != XR_SCAN_POISONED &&
     hw->scan_request) {
  s->aborted |= abort;
  s->poison |= poison;
  s->pending = true;
  if (s->phase != XR_SCAN_SUBMITTING)
   s->phase = XR_SCAN_COMPLETING;
  queue_work(system_long_wq, &hw->scan_only_work);
 }
 spin_unlock_irqrestore(&hw->scan_only_lock, flags);
}

static void scan_advance(struct work_struct *work)
{
 struct xradio_common *hw = container_of(work, struct xradio_common,
                                         scan_only_work);
 unsigned long flags;
 mutex_lock(&hw->scan_control);
 for (;;) {
  struct xr829_scan_request *r;
  struct xrs_scan_batch *batch;
  struct wsm_scan scan = {};
  int ret = 0;
  spin_lock_irqsave(&hw->scan_only_lock, flags);
  r = hw->scan_request;
  if (!r) {
   spin_unlock_irqrestore(&hw->scan_only_lock, flags);
   mutex_unlock(&hw->scan_control);
   return;
  }
  if (hw->scan_only_state.phase == XR_SCAN_COMPLETING) {
   if (!hw->scan_only_state.aborted && ++r->batch < r->plan.count) {
    hw->scan_only_state.phase = XR_SCAN_QUEUED;
    hw->scan_only_state.pending = false;
   } else {
    struct cfg80211_scan_info info = {
     .aborted = hw->scan_only_state.aborted,
    };
    hw->scan_request = NULL;
    spin_unlock_irqrestore(&hw->scan_only_lock, flags);
    cancel_delayed_work_sync(&hw->scan_only_timeout);
    dev_kfree_skb(r->probe);
    kfree(r);
    ieee80211_scan_completed(hw->hw, &info);
    spin_lock_irqsave(&hw->scan_only_lock, flags);
    xr829_scan_notified(&hw->scan_only_state);
    spin_unlock_irqrestore(&hw->scan_only_lock, flags);
    wake_up_all(&hw->scan_only_notified);
    mutex_unlock(&hw->scan_control);
    return;
   }
  }
  if (hw->scan_only_state.phase != XR_SCAN_QUEUED) {
   spin_unlock_irqrestore(&hw->scan_only_lock, flags);
   mutex_unlock(&hw->scan_control);
   return;
  }
  batch = &r->plan.batch[r->batch];
  hw->scan_only_state.phase = XR_SCAN_SUBMITTING;
  hw->scan_only_channels = batch->count;
  spin_unlock_irqrestore(&hw->scan_only_lock, flags);
  scan.band = 0;
  scan.scanType = WSM_SCAN_TYPE_FOREGROUND;
  scan.numOfChannels = batch->count;
  scan.ch = &r->channels[batch->first];
  scan.maxTransmitRate = r->rate;
  if (batch->active) {
   struct wsm_template_frame template = {
    .frame_type = 0, .rate = r->rate << 4, .skb = r->probe,
   };
   int power;
   ret = xrs_power_limit(batch->power_dbm, r->requested_power,
                         INT_MIN, hw->scan_power.min_power_level,
                         hw->scan_power.max_power_level, &power);
   if (!ret) ret = wsm_set_output_power(hw, power, 0);
   if (!ret) ret = wsm_set_template_frame(hw, &template, 0);
   scan.numOfProbeRequests = 2;
   scan.numOfSSIDs = r->n_ssids;
   scan.ssids = r->ssids;
  }
  /* Cancellation while a preceding MIB waits must not start a new scan. */
  spin_lock_irqsave(&hw->scan_only_lock, flags);
  if (hw->scan_only_state.aborted && !ret) ret = -ECANCELED;
  spin_unlock_irqrestore(&hw->scan_only_lock, flags);
  if (!ret && READ_ONCE(hw->bh_error)) ret = -EIO;
  if (!ret) ret = wsm_scan(hw, &scan, 0);
  spin_lock_irqsave(&hw->scan_only_lock, flags);
  xr829_scan_submitted(&hw->scan_only_state, ret != 0);
  spin_unlock_irqrestore(&hw->scan_only_lock, flags);
 }
}

static void scan_timeout(struct work_struct *work)
{
 struct xradio_common *hw = container_of(to_delayed_work(work),
                                         struct xradio_common, scan_only_timeout);
 dev_err(hw->pdev, "XR829 scan timed out; recovery required\n");
 finish_scan(hw, true, true);
}

void xr829_scan_init(struct xradio_common *hw)
{
 mutex_init(&hw->scan_control);
 spin_lock_init(&hw->scan_only_lock);
 init_waitqueue_head(&hw->scan_only_notified);
 hw->scan_only_state.phase = XR_SCAN_IDLE;
 INIT_WORK(&hw->scan_only_work, scan_advance);
 INIT_DELAYED_WORK(&hw->scan_only_timeout, scan_timeout);
}

void xr829_scan_done(struct xradio_common *hw, u32 status, u8 channels)
{
 unsigned long flags;
 spin_lock_irqsave(&hw->scan_only_lock, flags);
 if (hw->scan_only_state.phase == XR_SCAN_SUBMITTING ||
     hw->scan_only_state.phase == XR_SCAN_ACTIVE) {
  hw->scan_firmware_events++;
  xr829_scan_event(&hw->scan_only_state,
                    status || channels != hw->scan_only_channels, false);
  queue_work(system_long_wq, &hw->scan_only_work);
 }
 spin_unlock_irqrestore(&hw->scan_only_lock, flags);
}

static int configure_radio(struct ieee80211_hw *dev)
{
	struct xradio_common *hw = dev->priv;
	/* Vendor wsm.h: operational active + disableMoreFlagUsage; RCPI
	 * subscription with no threshold notifications, averaging 16 samples. */
	u8 active[4] = { 0x10, 0, 0, 0 };
	u8 rcpi[4] = { 0x0d, 0, 0, 16 };
	u8 mac[ETH_ALEN];
	struct wsm_configuration cfg = {};
	int ret;

	if (READ_ONCE(hw->removing) || READ_ONCE(hw->recovery_state)==XRS_DEAD)
		return -ENODEV;
	if (scan_phase(hw) == XR_SCAN_POISONED)
		return -EIO;
	if (!hw->scan_only_configured) {
		if (!hw->sdd || hw->sdd->size != 744)
			return -EINVAL;
		memcpy(mac, dev->wiphy->perm_addr, ETH_ALEN);
		cfg.dot11StationId = mac;
		cfg.dpdData = hw->sdd->data;
		cfg.dpdData_size = hw->sdd->size;
		ret = wsm_configuration(hw, &cfg, 0);
		if (ret)
			return ret;
		hw->scan_power = cfg.txPowerRange[0];
		hw->scan_power.min_power_level = max(hw->scan_power.min_power_level,
						     hw->sdd_power_min);
		hw->scan_power.max_power_level = min(hw->scan_power.max_power_level,
						     hw->sdd_power_max);
		if (hw->scan_power.min_power_level > hw->scan_power.max_power_level)
			return -ERANGE;
		hw->scan_only_configured = true;
	}
	ret = wsm_write_mib(hw, 0x1006, active, sizeof(active), 0);
	if (!ret)
		ret = wsm_write_mib(hw, 0x1009, rcpi, sizeof(rcpi), 0);
	return ret;
}

static int scan_start(struct ieee80211_hw *dev)
{
 struct xradio_common *hw=dev->priv;
 int ret=configure_radio(dev);
 if(ret && hw->transport_epoch) {
  WRITE_ONCE(hw->recovery_state,XRS_DEAD);
  WRITE_ONCE(hw->bh_error,1);
  ieee80211_stop_queues(dev);
 }
 return ret;
}

static void scan_cancel(struct ieee80211_hw *dev, struct ieee80211_vif *vif)
{
	struct xradio_common *hw = dev->priv;
	/* No new command may overlap a scan whose firmware completion is unknown. */
	finish_scan(hw, true, true);
	cancel_delayed_work_sync(&hw->scan_only_timeout);
	flush_work(&hw->scan_only_work);
}

static void scan_stop(struct ieee80211_hw *dev, bool suspend)
{
	scan_cancel(dev, NULL);
	xrs_runtime_stop(dev->priv);
}

static int apply_interface(struct ieee80211_hw *dev, struct ieee80211_vif *vif)
{
	struct xradio_common *hw = dev->priv;
	if (!allow_station && memcmp(vif->addr,dev->wiphy->perm_addr,ETH_ALEN))
		return -EOPNOTSUPP;
	if (vif->type != NL80211_IFTYPE_STATION || hw->scan_only_vif)
		return -EOPNOTSUPP;
	if (allow_station) {
		int ret = xrs_runtime_add_address(hw, vif->addr);
		if (ret) return ret;
	}
	hw->scan_only_vif = true;
	hw->recovery_vif = vif;
	return 0;
}

static int scan_add(struct ieee80211_hw *dev, struct ieee80211_vif *vif)
{
 struct xradio_common *hw=dev->priv;
 int ret=apply_interface(dev,vif);
 if(ret && READ_ONCE(hw->recovery_state)==XRS_RESTORING) {
  WRITE_ONCE(hw->recovery_state,XRS_DEAD);
  WRITE_ONCE(hw->bh_error,1);
  ieee80211_stop_queues(dev);
 }
 return ret;
}

static void scan_remove(struct ieee80211_hw *dev, struct ieee80211_vif *vif)
{
	struct xradio_common *hw = dev->priv;
	scan_cancel(dev, vif);
	xrs_runtime_stop(dev->priv);
	xrs_runtime_remove_address(hw);
	hw->scan_only_vif = false;
	hw->recovery_vif = NULL;

}

static int hardware_scan(struct ieee80211_hw *dev, struct ieee80211_vif *vif,
                         struct ieee80211_scan_request *request)
{
 struct xradio_common *hw = dev->priv;
 struct cfg80211_scan_request *req = &request->req;
 struct xr829_scan_request *r;
 struct xrs_scan_channel channels[XRS_SCAN_CHANNELS] = {};
 u8 lengths[XRS_SCAN_SSIDS];
 unsigned long flags;
 int i, ret;

 if (READ_ONCE(hw->bh_error)) return -EIO;
 if (READ_ONCE(hw->recovery_state) != XRS_RUNNING) return -EBUSY;
 if (xrs_runtime_joined(hw) || scan_phase(hw) != XR_SCAN_IDLE) return -EBUSY;
 if ((!allow_station && req->n_ssids) || req->n_ssids > XRS_SCAN_SSIDS || req->ie_len > 512 ||
     (req->flags & ~(NL80211_SCAN_FLAG_COLOCATED_6GHZ | NL80211_SCAN_FLAG_FLUSH)) ||
     req->duration_mandatory || !req->n_channels ||
     req->n_channels > XRS_SCAN_CHANNELS)
  return -EOPNOTSUPP;
 r = kzalloc(sizeof(*r), GFP_KERNEL);
 if (!r) return -ENOMEM;
 r->n_channels = req->n_channels;
 r->requested_power = dev->conf.power_level;
 r->n_ssids = req->n_ssids;
 r->rate = req->no_cck ? WSM_TRANSMIT_RATE_6 : WSM_TRANSMIT_RATE_1;
 for (i = 0; i < req->n_ssids; i++) {
  lengths[i] = req->ssids[i].ssid_len;
  if (lengths[i] > 32) { ret = -EINVAL; goto free; }
  r->ssids[i].length = lengths[i];
  memcpy(r->ssids[i].ssid, req->ssids[i].ssid, lengths[i]);
 }
 for (i = 0; i < req->n_channels; i++) {
  struct ieee80211_channel *ch = req->channels[i];
  if (ch->band != NL80211_BAND_2GHZ || ch->hw_value < 1 ||
      ch->hw_value > 14) { ret = -EINVAL; goto free; }
  channels[i] = (struct xrs_scan_channel) {
   .number = ch->hw_value, .disabled = ch->flags & (IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_20MHZ),
   .no_ir = ch->flags & IEEE80211_CHAN_NO_IR,
   .no_ofdm = ch->flags & IEEE80211_CHAN_NO_OFDM,
   .power_dbm = ch->max_power,
  };
  r->channels[i].number = ch->hw_value;
  r->channels[i].minChannelTime = 110;
  r->channels[i].maxChannelTime = 120;
 }
 memcpy(r->policy, channels, sizeof(channels));
	ret = xrs_scan_plan_build(&r->plan, channels, req->n_channels,
				  req->n_ssids, lengths, dev->conf.power_level,
				  req->no_cck);
 if (ret) goto free;
 /* Validate every active batch before accepting ownership or transmitting. */
 for (i = 0; i < r->plan.count; i++) {
  int power;
  if (!r->plan.batch[i].active) continue;
  ret = xrs_power_limit(r->plan.batch[i].power_dbm, dev->conf.power_level,
                        INT_MIN, hw->scan_power.min_power_level,
                        hw->scan_power.max_power_level, &power);
  if (ret) goto free;
 }
 if (req->n_ssids) {
  if (!is_broadcast_ether_addr(req->bssid) && !is_valid_ether_addr(req->bssid)) {
   ret = -EINVAL; goto free;
  }
  r->probe = ieee80211_probereq_get(dev, vif->addr, NULL, 0, req->ie_len);
  if (!r->probe) { ret = -ENOMEM; goto free; }
  if (skb_cow_head(r->probe, 8)) { ret = -ENOMEM; goto free; }
  skb_put_data(r->probe, req->ie, req->ie_len);
  /* Match mac80211 software scan: directed probes address both fields. */
  ether_addr_copy(((struct ieee80211_mgmt *)r->probe->data)->da, req->bssid);
  ether_addr_copy(((struct ieee80211_mgmt *)r->probe->data)->bssid, req->bssid);
 }
 cancel_delayed_work_sync(&hw->scan_only_timeout);
 flush_work(&hw->scan_only_work);
 spin_lock_irqsave(&hw->scan_only_lock, flags);
 if (!xr829_scan_begin(&hw->scan_only_state)) {
  spin_unlock_irqrestore(&hw->scan_only_lock, flags);
  ret = -EBUSY;
  goto free;
 }
 hw->scan_request = r;
 hw->scan_only_state.phase = XR_SCAN_QUEUED;
 queue_delayed_work(system_long_wq, &hw->scan_only_timeout, 30 * HZ);
 queue_work(system_long_wq, &hw->scan_only_work);
 spin_unlock_irqrestore(&hw->scan_only_lock, flags);
 return 0;
free:
 dev_kfree_skb(r->probe);
 kfree(r);
 return ret;
}

int xr829_scan_rx(struct xradio_common *hw, const u8 *data, size_t len)
{
	struct xrw_rx rx;
	struct sk_buff *skb;
	struct ieee80211_rx_status *status;
	struct ieee80211_channel *channel;
	__le16 fc;
	unsigned int rate;
	int freq;

	if ((scan_phase(hw) != XR_SCAN_SUBMITTING &&
	     scan_phase(hw) != XR_SCAN_ACTIVE) ||
	    xrw_rx_decode(&rx, data, len) < 0 || rx.status ||
	    rx.channel < 1 || rx.channel > 14 || len < 16 + 36)
		return 0;
	/* Management beacons/probe responses only; legacy 20 MHz rate domain. */
	rate = (rx.rate >> 4) & 15;
	if ((rx.rate & 0xf000) || rate > 13 || rate == 4 || rate == 5)
		return 0;
	memcpy(&fc, data + 16, sizeof(fc));
	if (!ieee80211_is_beacon(fc) && !ieee80211_is_probe_resp(fc))
		return 0;
	freq = ieee80211_channel_to_frequency(rx.channel, NL80211_BAND_2GHZ);
	channel = ieee80211_get_channel(hw->hw->wiphy, freq);
	if (!channel || channel->flags & IEEE80211_CHAN_DISABLED)
		return 0;
	skb = dev_alloc_skb(len - 16);
	if (!skb)
		return 0;
	skb_put_data(skb, data + 16, len - 16);
	status = IEEE80211_SKB_RXCB(skb);
	memset(status, 0, sizeof(*status));
	status->band = NL80211_BAND_2GHZ;
	status->freq = freq;
	status->signal = rx.rssi / 2 - 110;
	status->rate_idx = rate >= 6 ? rate - 2 : rate;
	ieee80211_rx_irqsafe(hw->hw, skb);
	return 0;
}

static void reject_tx(struct ieee80211_hw *dev,
		struct ieee80211_tx_control *control, struct sk_buff *skb)
{
	if (allow_station) xrs_runtime_tx(dev->priv, control->sta, skb);
	else ieee80211_free_txskb(dev, skb);
}

static int scan_config(struct ieee80211_hw *dev, int radio_idx, u32 changed)
{
 struct xradio_common *hw = dev->priv;
 bool active;
 unsigned long flags;
 int ret;
 lockdep_assert_wiphy(dev->wiphy);
 if (!(changed & (IEEE80211_CONF_CHANGE_POWER | IEEE80211_CONF_CHANGE_CHANNEL)))
  return xrs_runtime_config(hw, changed);
 /* Publish invalidation before waiting for the transaction. An in-flight
  * command can finish, but no subsequent batch may start. */
 spin_lock_irqsave(&hw->scan_only_lock, flags);
 active = hw->scan_only_state.phase != XR_SCAN_IDLE;
 if (active && hw->scan_request) {
  hw->scan_only_state.aborted = true;
  hw->scan_only_state.poison = true;
 }
 spin_unlock_irqrestore(&hw->scan_only_lock, flags);
 if (active && !hw->recovery_in_progress) {
  /* Drain the complete submission before resetting. Never hold this mutex
   * across recovery: scan quiesce flushes the worker that also acquires it. */
  mutex_lock(&hw->scan_control);
  mutex_unlock(&hw->scan_control);
  ret = xrs_recovery_sync_locked(hw);
  if (ret) return ret;
 }
 return xrs_runtime_config(hw, changed);
}

/* Normal (non-self-managed) cfg80211 notifier: RTNL -> wiphy mutex.
 * Initial registration also calls here outside wiphy mutex. Deregistration
 * under RTNL serializes notifier lifetime with free_common. A Country IE
 * that merely relaxes restrictions must not cause a disconnect/rejoin loop. */
void xr829_scan_reg_notifier(struct wiphy *wiphy,
                            struct regulatory_request *request)
{
 struct xradio_common *hw = wiphy_to_ieee80211_hw(wiphy)->priv;
 struct ieee80211_supported_band *band = wiphy->bands[NL80211_BAND_2GHZ];
 const u32 restrictions = IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_IR |
                          IEEE80211_CHAN_NO_OFDM | IEEE80211_CHAN_NO_20MHZ;
 unsigned long flags;
 bool enforce = false;
 int i, j, ret;
 ASSERT_RTNL();
 spin_lock_irqsave(&hw->scan_only_lock, flags);
 if (hw->scan_request) {
  struct xr829_scan_request *r = hw->scan_request;
  for (i = 0; i < r->n_channels; i++) {
   struct xrs_scan_channel *old = &r->policy[i];
   for (j = 0; j < band->n_channels; j++) {
    struct ieee80211_channel *ch = &band->channels[j];
    if (ch->hw_value != old->number) continue;
    u32 old_flags=(old->disabled?IEEE80211_CHAN_DISABLED:0) |
                  (old->no_ir?IEEE80211_CHAN_NO_IR:0) |
                  (old->no_ofdm?IEEE80211_CHAN_NO_OFDM:0);
    if (xrs_scan_policy_tightened(old_flags,old->power_dbm,
                                  ch->flags,ch->max_power,restrictions))
     enforce = true;
   }
  }
  if (enforce) {
   hw->scan_only_state.aborted = true;
   hw->scan_only_state.poison = true;
  }
 } else if (hw->scan_only_state.phase != XR_SCAN_IDLE) {
  /* The notifier may have detached the request but not yet published its
   * completion. A cancelled/poisoned request can still own autonomous RF. */
  enforce = true;
 }
 spin_unlock_irqrestore(&hw->scan_only_lock, flags);
 wiphy_lock(wiphy);
 for (i = 0; i < band->n_channels; i++) {
  struct ieee80211_channel *ch = &band->channels[i];
  int n = ch->hw_value - 1;
  if (n < 0 || n >= 14) continue;
  if (hw->regulatory_snapshot_valid && xrs_runtime_joined(hw) &&
      hw->hw->conf.chandef.chan == ch &&
      xrs_scan_policy_tightened(hw->regulatory_flags[n],hw->regulatory_power[n],
                                ch->flags,ch->max_power,restrictions))
   enforce = true;
  hw->regulatory_flags[n] = ch->flags;
  hw->regulatory_power[n] = ch->max_power;
 }
 hw->regulatory_snapshot_valid = true;
 if (enforce && READ_ONCE(hw->driver_ready) && !READ_ONCE(hw->removing)) {
  /* Synchronous physical reset is mandatory, independent of allow_recovery.
   * Local scan completion and a STOP confirmation are not used as RF fences. */
  ret = xrs_recovery_sync_locked(hw);
  if (ret)
   dev_err(hw->pdev, "XR829 regulatory enforcement failed: %d\n", ret);
 }
 wiphy_unlock(wiphy);
}

static void scan_filter(struct ieee80211_hw *dev, unsigned int changed,
		unsigned int *total, u64 multicast)
{
	*total = 0;
}

/* mac80211 checks callback presence before the hardware AMPDU flag when
 * rate control requests BA. Explicitly decline until firmware/host BA
 * ownership and RX reorder are implemented; firmware BA masks stay zero. */
static int xrs_ampdu_action(struct ieee80211_hw *hw,struct ieee80211_vif *vif,
                            struct ieee80211_ampdu_params *params)
{
 return -EOPNOTSUPP;
}

const struct ieee80211_ops xr829_scan_ops = {
 .ampdu_action = xrs_ampdu_action,
	.set_key = xrs_set_key,
	.conf_tx = xrs_conf_tx,
	.mgd_prepare_tx = xrs_mgd_prepare,
	.bss_info_changed = xrs_bss_changed,
	.start = scan_start, .stop = scan_stop,
	.add_interface = scan_add, .remove_interface = scan_remove,
	.tx = reject_tx, .hw_scan = hardware_scan, .cancel_hw_scan = scan_cancel,
	.config = scan_config, .configure_filter = scan_filter,
	.get_txpower = xrs_get_txpower,
	.wake_tx_queue = ieee80211_handle_wake_tx_queue,
	.add_chanctx = ieee80211_emulate_add_chanctx,
	.remove_chanctx = ieee80211_emulate_remove_chanctx,
	.change_chanctx = ieee80211_emulate_change_chanctx,
	.switch_vif_chanctx = ieee80211_emulate_switch_vif_chanctx,
};

int xr829_scan_quiesce(struct xradio_common *hw)
{
 cancel_delayed_work_sync(&hw->scan_only_timeout);
 finish_scan(hw,true,true);
 flush_work(&hw->scan_only_work);
 /* A concurrent BH completion can own COMPLETING. Wait until its stack
  * notification is published before the recovery caller flushes wiphy work. */
 if(!wait_event_timeout(hw->scan_only_notified,
      scan_phase(hw)!=XR_SCAN_COMPLETING,HZ))return -ETIMEDOUT;
 if(scan_phase(hw)==XR_SCAN_SUBMITTING)return -EBUSY;
 return 0;
}
void xr829_scan_new_epoch(struct xradio_common *hw)
{
 unsigned long flags;
 spin_lock_irqsave(&hw->scan_only_lock,flags);
 /* BH joined, timeout canceled, all accepted scan notifications published. */
 memset(&hw->scan_only_state,0,sizeof(hw->scan_only_state));
 hw->scan_only_state.phase=XR_SCAN_IDLE;
 hw->scan_only_configured=false;
 hw->scan_only_vif=false;
 hw->recovery_vif=NULL;
 hw->scan_only_channels=0;
 spin_unlock_irqrestore(&hw->scan_only_lock,flags);
}

/* Called with wiphy mutex after real reset and fresh STARTUP. This restores
 * only the device side of an existing, disconnected mac80211 interface. */
int xr829_restore_interface(struct xradio_common *hw,struct ieee80211_vif *vif)
{
 int ret;
 lockdep_assert_wiphy(hw->hw->wiphy);
 if(READ_ONCE(hw->recovery_state)!=XRS_RESTORING || vif->cfg.assoc)
  return -EINVAL;
 ret=configure_radio(hw->hw);
 if(ret)return ret;
 return apply_interface(hw->hw,vif);
}

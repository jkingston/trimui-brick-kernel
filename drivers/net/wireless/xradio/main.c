/*
 * Main code of XRadio drivers
 *
 * Copyright (c) 2013, XRadio
 * Author: XRadio
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/firmware.h>

#include <net/cfg80211.h>
#include <linux/of_net.h>
#include <linux/mmc/sdio_func.h>
#include <linux/version.h>

#include "xradio.h"
#include "main.h"
#include "fwio.h"
#include "hwio.h"
#include "bh.h"
#include "sta.h"
#include "ap.h"
#include "scan.h"
#include "pm.h"
#include "sdio.h"
#include "station-runtime.h"
#include "recovery.h"

/* TODO: use rates and channels from the device */
#define RATETAB_ENT(_rate, _rateid, _flags)		\
	{						\
		.bitrate  = (_rate),    \
		.hw_value = (_rateid),  \
		.flags    = (_flags),   \
	}

static struct ieee80211_rate xradio_rates[] = {
	RATETAB_ENT(10,  0,   0),
	RATETAB_ENT(20,  1,   0),
	RATETAB_ENT(55,  2,   0),
	RATETAB_ENT(110, 3,   0),
	RATETAB_ENT(60,  6,  0),
	RATETAB_ENT(90,  7,  0),
	RATETAB_ENT(120, 8,  0),
	RATETAB_ENT(180, 9,  0),
	RATETAB_ENT(240, 10, 0),
	RATETAB_ENT(360, 11, 0),
	RATETAB_ENT(480, 12, 0),
	RATETAB_ENT(540, 13, 0),
};

static struct ieee80211_rate xradio_mcs_rates[] = {
	RATETAB_ENT(65,  14, IEEE80211_TX_RC_MCS),
	RATETAB_ENT(130, 15, IEEE80211_TX_RC_MCS),
	RATETAB_ENT(195, 16, IEEE80211_TX_RC_MCS),
	RATETAB_ENT(260, 17, IEEE80211_TX_RC_MCS),
	RATETAB_ENT(390, 18, IEEE80211_TX_RC_MCS),
	RATETAB_ENT(520, 19, IEEE80211_TX_RC_MCS),
	RATETAB_ENT(585, 20, IEEE80211_TX_RC_MCS),
	RATETAB_ENT(650, 21, IEEE80211_TX_RC_MCS),
};

#define xradio_g_rates      (xradio_rates + 0)
#define xradio_a_rates      (xradio_rates + 4)
#define xradio_n_rates      (xradio_mcs_rates)

#define xradio_g_rates_size (ARRAY_SIZE(xradio_rates))
#define xradio_a_rates_size (ARRAY_SIZE(xradio_rates) - 4)
#define xradio_n_rates_size (ARRAY_SIZE(xradio_mcs_rates))

#define CHAN2G(_channel, _freq, _flags) {   \
	.band             = NL80211_BAND_2GHZ,  \
	.center_freq      = (_freq),              \
	.hw_value         = (_channel),           \
	.flags            = (_flags),             \
	.max_antenna_gain = 0,                    \
	.max_power        = 30,                   \
}

#define CHAN5G(_channel, _flags) {   \
	.band             = NL80211_BAND_5GHZ,     \
	.center_freq      = 5000 + (5 * (_channel)), \
	.hw_value         = (_channel),              \
	.flags            = (_flags),                \
	.max_antenna_gain = 0,                       \
	.max_power        = 30,                      \
}

static struct ieee80211_channel xradio_2ghz_chantable[] = {
	CHAN2G(1, 2412, 0),
	CHAN2G(2, 2417, 0),
	CHAN2G(3, 2422, 0),
	CHAN2G(4, 2427, 0),
	CHAN2G(5, 2432, 0),
	CHAN2G(6, 2437, 0),
	CHAN2G(7, 2442, 0),
	CHAN2G(8, 2447, 0),
	CHAN2G(9, 2452, 0),
	CHAN2G(10, 2457, 0),
	CHAN2G(11, 2462, 0),
	CHAN2G(12, 2467, 0),
	CHAN2G(13, 2472, 0),
	CHAN2G(14, 2484, 0),
};

static struct ieee80211_supported_band xradio_band_2ghz = {
	.channels = xradio_2ghz_chantable,
	.n_channels = ARRAY_SIZE(xradio_2ghz_chantable),
	.bitrates = xradio_g_rates,
	.n_bitrates = xradio_g_rates_size,
	.ht_cap = {
		.cap = IEEE80211_HT_CAP_GRN_FLD |
		       (1 << IEEE80211_HT_CAP_RX_STBC_SHIFT),
		.ht_supported  = 1,
		.ampdu_factor  = IEEE80211_HT_MAX_AMPDU_32K,
		.ampdu_density = IEEE80211_HT_MPDU_DENSITY_NONE,
		.mcs = {
			.rx_mask[0] = 0xFF,
			.rx_highest = __cpu_to_le16(0x41),
			.tx_params  = IEEE80211_HT_MCS_TX_DEFINED,
		},
	},
};

#include "scan-only.h"
static const u32 xr829_station_ciphers[] = { WLAN_CIPHER_SUITE_CCMP };



/*************************************** functions ***************************************/

static struct ieee80211_hw *xradio_init_common(size_t hw_priv_data_len)
{
	int i;
	struct ieee80211_hw *hw;
	struct xradio_common *hw_priv;
	struct ieee80211_supported_band *sband;
	int band;

	/* Alloc ieee_802.11 hw and xradio_common struct. */
	hw = ieee80211_alloc_hw(hw_priv_data_len, &xr829_scan_ops);
	if (!hw)
		return NULL;
	hw_priv = hw->priv;
	memset(hw_priv, 0, sizeof(*hw_priv));

	/* Initialize members of hw_priv. */
	hw_priv->hw = hw;
	hw_priv->if_id_slot = 0;
	hw_priv->roc_if_id = -1;
	atomic_set(&hw_priv->num_vifs, 0);
	/* initial rates and channels TODO: fetch from FW */
	hw_priv->rates = xradio_rates;
	hw_priv->mcs_rates = xradio_n_rates;
	hw_priv->noise = -94;
	/* hw_priv->beacon_req_id = cpu_to_le32(0); */

	/* Initialize members of ieee80211_hw, it works in UMAC. */
	hw->sta_data_size = sizeof(struct xradio_sta_priv);
	hw->vif_data_size = sizeof(struct xradio_vif);

	ieee80211_hw_set(hw, SIGNAL_DBM);
	ieee80211_hw_set(hw, SW_CRYPTO_CONTROL);
	hw->wiphy->cipher_suites = xr829_station_ciphers;
	hw->wiphy->n_cipher_suites = ARRAY_SIZE(xr829_station_ciphers);

	hw->wiphy->addresses   = hw_priv->addresses;
	hw->extra_tx_headroom = WSM_TX_EXTRA_HEADROOM +
	                        8  /* TKIP IV */      +
	                        12 /* TKIP ICV and MIC */;
	hw->wiphy->bands[NL80211_BAND_2GHZ] = &xradio_band_2ghz;
	hw->queues         = AC_QUEUE_NUM;
	hw->max_rates      = MAX_RATES_STAGE;
	hw->max_rate_tries = MAX_RATES_RETRY;
	/* Channel params have to be cleared before registering wiphy again */
	for (band = 0; band < NUM_NL80211_BANDS; band++) {
		sband = hw->wiphy->bands[band];
		if (!sband)
			continue;
		for (i = 0; i < sband->n_channels; i++) {
			sband->channels[i].flags = 0;
			sband->channels[i].max_antenna_gain = 0;
			sband->channels[i].max_power = 30;
		}
	}
	/* hw_priv->channel init value is the local->oper_channel init value;when transplanting,take care */
	for (band = 0; band < NUM_NL80211_BANDS; band++) {
		sband = hw->wiphy->bands[band];
		if (!sband)
			continue;
		if(!hw_priv->channel){
			hw_priv->channel = &sband->channels[2];
		}
	}
	hw->wiphy->max_scan_ssids = WSM_SCAN_MAX_NUM_OF_SSIDS;
	hw->wiphy->max_scan_ie_len = IEEE80211_MAX_DATA_LEN;
	SET_IEEE80211_PERM_ADDR(hw, hw_priv->addresses[0].addr);

	/* Initialize locks. */
	spin_lock_init(&hw_priv->vif_list_lock);
	mutex_init(&hw_priv->wsm_cmd_mux);
	mutex_init(&hw_priv->conf_mutex);
	mutex_init(&hw_priv->wsm_oper_lock);
	atomic_set(&hw_priv->tx_lock, 0);
	sema_init(&hw_priv->tx_lock_sem, 1);

	hw_priv->workqueue = create_singlethread_workqueue(XRADIO_WORKQUEUE);
	if (!hw_priv->workqueue) {
		ieee80211_free_hw(hw);
		return NULL;
	}
	init_waitqueue_head(&hw_priv->channel_switch_done);
	init_waitqueue_head(&hw_priv->wsm_cmd_wq);
	init_waitqueue_head(&hw_priv->wsm_startup_done);
	init_waitqueue_head(&hw_priv->offchannel_wq);
	hw_priv->wsm_caps.firmwareReady = 0;
	hw_priv->driver_ready = 0;
	hw_priv->offchannel_done = 0;
	wsm_buf_init(&hw_priv->wsm_cmd_buf);
	spin_lock_init(&hw_priv->wsm_cmd.lock);
	xradio_init_resv_skb(hw_priv);
	/* One station interface; foreground scanning. */
	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	hw->wiphy->n_addresses = 1;
	hw->wiphy->max_scan_ssids = 2;
	hw->wiphy->max_scan_ie_len = 512;
	hw->wiphy->flags &= ~(WIPHY_FLAG_AP_UAPSD |
		WIPHY_FLAG_AP_PROBE_RESP_OFFLOAD | WIPHY_FLAG_PS_ON_BY_DEFAULT);
	xradio_band_2ghz.ht_cap.ht_supported = allow_station && allow_ht;
	/* One stream, HT20 long GI and firmware-owned BA negotiation.
	 * mac80211 reorders received MPDUs; no STBC/GF/HT40/SGI.
	 */
	xradio_band_2ghz.ht_cap.cap = 0;
	xradio_band_2ghz.ht_cap.ampdu_factor = IEEE80211_HT_MAX_AMPDU_32K;
	xradio_band_2ghz.ht_cap.mcs.rx_highest = cpu_to_le16(65);
	if (allow_station) { hw->queues = 4; hw->max_rates=4; hw->max_rate_tries=4; } /* Dynamic policies cover negotiated legacy and optional HT20 rates. */
	xr829_scan_init(hw_priv);
	hw->wiphy->reg_notifier = xr829_scan_reg_notifier;

	return hw;
}

static void xradio_free_common(struct ieee80211_hw *dev)
{
	struct xradio_common *hw_priv = dev->priv;

	xrs_runtime_free(hw_priv);
	xr829_scan_quiesce(hw_priv);
	cancel_work_sync(&hw_priv->scan_only_work);
	mutex_destroy(&hw_priv->wsm_oper_lock);
	mutex_destroy(&hw_priv->conf_mutex);
	mutex_destroy(&hw_priv->wsm_cmd_mux);
	wsm_buf_deinit(&hw_priv->wsm_cmd_buf);
	flush_workqueue(hw_priv->workqueue);
	destroy_workqueue(hw_priv->workqueue);
	hw_priv->workqueue = NULL;

	xradio_deinit_resv_skb(hw_priv);
	if (hw_priv->skb_cache) {
		dev_kfree_skb(hw_priv->skb_cache);
		hw_priv->skb_cache = NULL;
	}

	ieee80211_free_hw(dev);
}

static int xradio_register_common(struct ieee80211_hw *dev)
{
	int err = 0;
	struct xradio_common *hw_priv = dev->priv;

	SET_IEEE80211_DEV(dev, hw_priv->pdev);
	err = ieee80211_register_hw(dev);
	if (err) {
		dev_dbg(hw_priv->pdev, "Cannot register device (%d).\n", err);
		return err;
	}
	dev_dbg(hw_priv->pdev, "is registered as '%s'\n",
	           wiphy_name(dev->wiphy));

	hw_priv->driver_ready = 1;
	wake_up(&hw_priv->wsm_startup_done);
	return 0;
}

static void xradio_unregister_common(struct ieee80211_hw *dev)
{
	struct xradio_common *hw_priv = dev->priv;

	if (hw_priv->driver_ready) {
	ieee80211_unregister_hw(dev);
		SET_IEEE80211_DEV(dev, NULL);
	}
	hw_priv->driver_ready = 0;
}

int xradio_core_init(struct sdio_func* func)
{
	int err = -ENOMEM;
	struct ieee80211_hw *dev;
	struct xradio_common *hw_priv;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
	unsigned char addr[ETH_ALEN];
#else
	unsigned char randomaddr[ETH_ALEN];
	const unsigned char *addr = NULL;
#endif

	//init xradio_common
	dev = xradio_init_common(sizeof(struct xradio_common));
	if (!dev) {
		dev_dbg(&func->dev, "xradio_init_common failed\n");
		return err;
	}
	hw_priv = dev->priv;
	hw_priv->pdev = &func->dev;
	hw_priv->sdio_func = func;
	hw_priv->function_enabled = true;
	xrs_recovery_init(hw_priv);
	sdio_set_drvdata(func, hw_priv);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
	// fill in mac addresses
	if (hw_priv->pdev->of_node) {
		err = of_get_mac_address(hw_priv->pdev->of_node, addr);
	}
	if (err < 0) {
		dev_warn(hw_priv->pdev, "no mac address provided, using random\n");
		eth_random_addr(addr);
	}
#else
	// fill in mac addresses
	if (hw_priv->pdev->of_node) {
		addr = of_get_mac_address(hw_priv->pdev->of_node);
	}
	if (!addr) {
		dev_warn(hw_priv->pdev, "no mac address provided, using random\n");
		eth_random_addr(randomaddr);
		addr = randomaddr;
	}
#endif

	memcpy(hw_priv->addresses[0].addr, addr, ETH_ALEN);
	memcpy(hw_priv->addresses[1].addr, addr, ETH_ALEN);
	hw_priv->addresses[1].addr[5] += 0x01;

	err = xrs_runtime_init(hw_priv);
	if (err)
		goto err2;
	err = xrs_transport_start(hw_priv);
	if (err)
		goto err2;

	dev_info(hw_priv->pdev, "XR829 firmware ready; registering station interface\n");
	err = xradio_register_common(dev);
	if (err)
		goto err4;
	xrs_recovery_monitor(hw_priv);
	return 0;


err4:
	xrs_transport_stop(hw_priv);
err2:
	sdio_claim_host(func);
	sdio_set_drvdata(func, NULL);
	sdio_release_host(func);
	xradio_free_common(dev);
	return err;
}

void xradio_core_deinit(struct sdio_func* func)
{
	struct xradio_common* hw_priv = sdio_get_drvdata(func);
	if (hw_priv) {
		xrs_recovery_cancel(hw_priv);
		xradio_unregister_common(hw_priv->hw);
		xrs_transport_stop(hw_priv);
		sdio_claim_host(func);
		if (hw_priv->function_enabled)
			sdio_disable_func(func);
		hw_priv->function_enabled = false;
		sdio_release_host(func);
		xradio_free_common(hw_priv->hw);
		sdio_set_drvdata(func, NULL);
	}
	return;
}

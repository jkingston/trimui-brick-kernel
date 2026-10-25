#include "station-command.h"
#include "key-command.h"
#include "station-runtime.h"
#include "scan-only.h"
#include "xr829-wire.h"
/*
 * WSM host interfaces for XRadio drivers
 *
 * Copyright (c) 2013, XRadio
 * Author: XRadio
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/skbuff.h>
#include <linux/wait.h>
#include <linux/skbuff.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/random.h>
#include <linux/overflow.h>

#include "xradio.h"
#include "wsm.h"
#include "bh.h"

#define WSM_CMD_TIMEOUT		(2 * HZ) /* With respect to interrupt loss */
#define WSM_CMD_JOIN_TIMEOUT	(7 * HZ) /* Join timeout is 5 sec. in FW   */
#define WSM_CMD_START_TIMEOUT	(7 * HZ)
#define WSM_CMD_RESET_TIMEOUT	(3 * HZ) /* 2 sec. timeout was observed.   */
#define WSM_CMD_DEFAULT_TIMEOUT	(3 * HZ)
#define WSM_SKIP(buf, size)						\
	do {								\
		if (unlikely((buf)->data + size > (buf)->end))		\
			goto underflow;					\
		(buf)->data += size;					\
	} while (0)

#define WSM_GET(buf, ptr, size)						\
	do {								\
		if (unlikely((buf)->data + size > (buf)->end))		\
			goto underflow;					\
		memcpy(ptr, (buf)->data, size);				\
		(buf)->data += size;					\
	} while (0)

#define __WSM_GET(buf, type, cvt)					\
	({								\
		type val;						\
		if (unlikely((buf)->data + sizeof(type) > (buf)->end))	\
			goto underflow;					\
		val = cvt(*(type *)(buf)->data);			\
		(buf)->data += sizeof(type);				\
		val;							\
	})

#define WSM_GET8(buf)  __WSM_GET(buf, u8, (u8))
#define WSM_GET16(buf) __WSM_GET(buf, u16, __le16_to_cpu)
#define WSM_GET32(buf) __WSM_GET(buf, u32, __le32_to_cpu)

/* Command storage is independent of the SDIO transfer block size. */
#define WSM_CMD_BUFFER_INITIAL 1024
#define WSM_CMD_BUFFER_MAX 4096

#define WSM_PUT(buf, ptr, size)                                     \
 do {                                                              \
  size_t put_size = (size);                                         \
  ret = wsm_buf_reserve((buf), put_size);                            \
  if (unlikely(ret))                                               \
   goto nomem;                                                     \
  memcpy((buf)->data, (ptr), put_size);                              \
  (buf)->data += put_size;                                          \
 } while (0)

#define __WSM_PUT(buf, val, type, cvt)                               \
 do {                                                              \
  ret = wsm_buf_reserve((buf), sizeof(type));                        \
  if (unlikely(ret))                                               \
   goto nomem;                                                     \
  *(type *)(buf)->data = cvt(val);                                   \
  (buf)->data += sizeof(type);                                      \
 } while (0)

#define WSM_PUT8(buf, val)  __WSM_PUT(buf, val, u8, (u8))
#define WSM_PUT16(buf, val) __WSM_PUT(buf, val, u16, __cpu_to_le16)
#define WSM_PUT32(buf, val) __WSM_PUT(buf, val, u32, __cpu_to_le32)

static void wsm_buf_reset(struct wsm_buf *buf);
static int wsm_buf_reserve(struct wsm_buf *buf, size_t extra_size);

static int wsm_cmd_send(struct xradio_common *hw_priv,
			struct wsm_buf *buf,
			void *arg, u16 cmd, long tmo, int if_id);

static inline void wsm_cmd_lock(struct xradio_common *hw_priv)
{
	mutex_lock(&hw_priv->wsm_cmd_mux);
}

static inline void wsm_cmd_unlock(struct xradio_common *hw_priv)
{
	mutex_unlock(&hw_priv->wsm_cmd_mux);
}

static inline void wsm_oper_lock(struct xradio_common *hw_priv)
{
	mutex_lock(&hw_priv->wsm_oper_lock);
}

static inline void wsm_oper_unlock(struct xradio_common *hw_priv)
{
	mutex_unlock(&hw_priv->wsm_oper_lock);
}

/* ******************************************************************** */
/* WSM API implementation						*/

static int wsm_generic_confirm(struct xradio_common *hw_priv,
			     void *arg,
			     struct wsm_buf *buf)
{
	u32 status = WSM_GET32(buf);
	if (status != WSM_STATUS_SUCCESS)
		return -EINVAL;
	return 0;

underflow:
	WARN_ON(1);
	return -EINVAL;
}

int wsm_configuration(struct xradio_common *hw_priv,
		      struct wsm_configuration *arg,
		      int if_id)
{
	int ret;
	struct wsm_buf *buf = &hw_priv->wsm_cmd_buf;

	wsm_cmd_lock(hw_priv);

	WSM_PUT32(buf, arg->dot11MaxTransmitMsduLifeTime);
	WSM_PUT32(buf, arg->dot11MaxReceiveLifeTime);
	WSM_PUT32(buf, arg->dot11RtsThreshold);

	/* DPD block. */
	WSM_PUT16(buf, arg->dpdData_size + 12);
	WSM_PUT16(buf, 1); /* DPD version */
	WSM_PUT(buf, arg->dot11StationId, ETH_ALEN);
	WSM_PUT16(buf, 5); /* DPD flags */
	WSM_PUT(buf, arg->dpdData, arg->dpdData_size);

	ret = wsm_cmd_send(hw_priv, buf, arg, 0x0009, WSM_CMD_TIMEOUT, if_id);

	wsm_cmd_unlock(hw_priv);
	return ret;

nomem:
	wsm_buf_reset(buf);
	wsm_cmd_unlock(hw_priv);
	return ret;
}

static int wsm_configuration_confirm(struct xradio_common *hw_priv,
				     struct wsm_configuration *arg,
				     struct wsm_buf *buf)
{
	int i;
	int status;

	status = WSM_GET32(buf);
	if (WARN_ON(status != WSM_STATUS_SUCCESS))
		return -EINVAL;

	WSM_GET(buf, arg->dot11StationId, ETH_ALEN);
	arg->dot11FrequencyBandsSupported = WSM_GET8(buf);
	WSM_SKIP(buf, 1);
	arg->supportedRateMask = WSM_GET32(buf);
	for (i = 0; i < 2; ++i) {
		arg->txPowerRange[i].min_power_level = WSM_GET32(buf);
		arg->txPowerRange[i].max_power_level = WSM_GET32(buf);
		arg->txPowerRange[i].stepping = WSM_GET32(buf);
	}
	return 0;

underflow:
	WARN_ON(1);
	return -EINVAL;
}

/* ******************************************************************** */

int wsm_reset(struct xradio_common *hw_priv, const struct wsm_reset *arg,
		int if_id)
{
	int ret;
	struct wsm_buf *buf = &hw_priv->wsm_cmd_buf;
	u16 cmd = 0x000A | WSM_TX_LINK_ID(arg->link_id);

	wsm_cmd_lock(hw_priv);

	WSM_PUT32(buf, arg->reset_statistics ? 0 : 1);
	ret = wsm_cmd_send(hw_priv, buf, NULL, cmd, WSM_CMD_RESET_TIMEOUT,
				if_id);
	wsm_cmd_unlock(hw_priv);
	return ret;

nomem:
	wsm_buf_reset(buf);
	wsm_cmd_unlock(hw_priv);
	return ret;
}

/* ******************************************************************** */

struct wsm_mib {
	u16 mibId;
	void *buf;
	size_t buf_size;
};

/* ******************************************************************** */

int wsm_write_mib(struct xradio_common *hw_priv, u16 mibId, void *_buf,
			size_t buf_size, int if_id)
{
	int ret = 0;
	struct wsm_buf *buf = &hw_priv->wsm_cmd_buf;
	struct wsm_mib mib_buf = {
		.mibId = mibId,
		.buf = _buf,
		.buf_size = buf_size,
	};

	wsm_cmd_lock(hw_priv);

	WSM_PUT16(buf, mibId);
	WSM_PUT16(buf, buf_size);
	WSM_PUT(buf, _buf, buf_size);

	ret = wsm_cmd_send(hw_priv, buf, &mib_buf, 0x0006, WSM_CMD_TIMEOUT,
			if_id);
	wsm_cmd_unlock(hw_priv);
	return ret;

nomem:
	wsm_buf_reset(buf);
	wsm_cmd_unlock(hw_priv);
	return ret;
}

static int wsm_write_mib_confirm(struct xradio_common *hw_priv,
				struct wsm_mib *arg,
				struct wsm_buf *buf,
				int interface_link_id)
{
	int ret;
	int i;
	struct xradio_vif *priv;
	ret = wsm_generic_confirm(hw_priv, arg, buf);
	if (ret)
		return ret;

	/*wsm_set_operational_mode confirm.*/
	if (arg->mibId == 0x1006) {
		const char *p = arg->buf;
		bool powersave_enabled = (p[0] & 0x0F) ? true : false;

		/* update vif PM status. */
		priv = xrwl_hwpriv_to_vifpriv(hw_priv, interface_link_id);
		if (priv) {
			xradio_enable_powersave(priv, powersave_enabled);
			spin_unlock(&priv->vif_lock);
		}

		/* HW powersave base on vif except for generic vif. */
		spin_lock(&hw_priv->vif_list_lock);
		xradio_for_each_vif(hw_priv, priv, i) {
			if (!priv)
				continue;
			powersave_enabled &= !!priv->powersave_enabled;
		}
		hw_priv->powersave_enabled = powersave_enabled;
		spin_unlock(&hw_priv->vif_list_lock);

	}
	return 0;
}

/* ******************************************************************** */

int wsm_scan(struct xradio_common *hw_priv, const struct wsm_scan *arg,
		int if_id)
{
	int i;
	int ret;
	struct wsm_buf *buf = &hw_priv->wsm_cmd_buf;

 if (if_id || arg->scanFlags || arg->scanType || arg->band ||
     arg->autoScanInterval || !arg->ch || !arg->numOfChannels ||
     arg->numOfChannels>14 || arg->numOfSSIDs>2 ||
     (arg->maxTransmitRate!=0 && arg->maxTransmitRate!=6) ||
     (arg->numOfProbeRequests!=0 && arg->numOfProbeRequests!=2) ||
     (!arg->numOfProbeRequests && arg->numOfSSIDs) ||
     (arg->numOfProbeRequests && !arg->numOfSSIDs) ||
     (arg->numOfSSIDs && !arg->ssids) || arg->probeDelay>255)
  return -EOPNOTSUPP;
 for(i=0;i<arg->numOfSSIDs;i++)
  if(arg->ssids[i].length>32)return -EINVAL;

	wsm_cmd_lock(hw_priv);

 {
  u8 wire[XR829_SCAN_PREFIX_SIZE];
  struct xrw_scan scan = {
   .band=arg->band, .type=arg->scanType, .flags=arg->scanFlags,
   .rate=(u16)arg->maxTransmitRate << 4,
   .interval=arg->autoScanInterval, .probes=arg->numOfProbeRequests,
   .channels=arg->numOfChannels, .ssids=arg->numOfSSIDs,
   .probe_delay=arg->probeDelay,
  };
  ret = xrw_scan_encode(wire, sizeof(wire), &scan);
  if (ret < 0) {
   wsm_cmd_unlock(hw_priv); return ret;
  }
  WSM_PUT(buf, wire, sizeof(wire));
 }

	for (i = 0; i < arg->numOfChannels; ++i) {
		u8 channel[16];
		ret = xrw_scan_channel_encode(channel, sizeof(channel),
			arg->ch[i].number, arg->ch[i].minChannelTime,
			arg->ch[i].maxChannelTime);
		if (ret < 0) {
			wsm_buf_reset(buf);
			wsm_cmd_unlock(hw_priv);
			return ret;
		}
		WSM_PUT(buf, channel, sizeof(channel));
	}

	for (i = 0; i < arg->numOfSSIDs; ++i) {
		WSM_PUT32(buf, arg->ssids[i].length);
        {
         u8 ssid[32]={0};
         memcpy(ssid,arg->ssids[i].ssid,arg->ssids[i].length);
         WSM_PUT(buf,ssid,sizeof(ssid));
        }
	}

	ret = wsm_cmd_send(hw_priv, buf, NULL, 0x0007, WSM_CMD_TIMEOUT,
			   if_id);
	wsm_cmd_unlock(hw_priv);
	return ret;

nomem:
	wsm_buf_reset(buf);
	wsm_cmd_unlock(hw_priv);
	return ret;
}

/* ******************************************************************** */

static int wsm_join_confirm(struct xradio_common *hw_priv,
			    struct wsm_join *arg,
			    struct wsm_buf *buf)
{
	u32 status = WSM_GET32(buf);

	if (status != WSM_STATUS_SUCCESS) {
		dev_warn_ratelimited(hw_priv->pdev,
				     "JOIN rejected by firmware: status=%u\n",
				     status);
		return -EINVAL;
	}
	arg->minPowerLevel = WSM_GET32(buf);
	arg->maxPowerLevel = WSM_GET32(buf);

	return 0;

underflow:
	WARN_ON(1);
	return -EINVAL;
}

int wsm_join(struct xradio_common *hw_priv, struct wsm_join *arg,
	     int if_id)
/*TODO: combo: make it work per vif.*/
{
	int ret;
	struct wsm_buf *buf = &hw_priv->wsm_cmd_buf;

	wsm_oper_lock(hw_priv);
	wsm_cmd_lock(hw_priv);

 {
  u8 wire[XR829_JOIN_SIZE];
  struct xrw_join join = {
   .channel=arg->channelNumber, .atim=arg->atimWindow,
   .mode=arg->mode, .probe=arg->probeForJoin, .dtim=arg->dtimPeriod,
   .flags=arg->flags, .ssid_len=arg->ssidLength,
   .beacon_interval=arg->beaconInterval, .basic_rates=arg->basicRateSet,
  };
  /* Deliberately legacy20MHz station path until HT rate/TX codecs exist. */
  ret = xrw_phy20(arg->band, arg->preambleType, 0, &join.phy);
  if (!ret) {
   memcpy(join.bssid, arg->bssid, 6); memcpy(join.ssid, arg->ssid, 32);
   ret = xrw_join_encode(wire, sizeof(wire), &join);
  }
  if (ret < 0) {
   wsm_cmd_unlock(hw_priv); wsm_oper_unlock(hw_priv); return ret;
  }
  WSM_PUT(buf, wire, sizeof(wire));
 }

	hw_priv->tx_burst_idx = -1;
	ret = wsm_cmd_send(hw_priv, buf, arg, 0x000B, WSM_CMD_JOIN_TIMEOUT,
			   if_id);
	wsm_cmd_unlock(hw_priv);
	wsm_oper_unlock(hw_priv); /*confirm, not indcation.*/
	return ret;

nomem:
	wsm_buf_reset(buf);
	wsm_cmd_unlock(hw_priv);
	wsm_oper_unlock(hw_priv);
	return ret;
}

/* ******************************************************************** */

int wsm_set_bss_params(struct xradio_common *hw_priv,
			const struct wsm_set_bss_params *arg,
			int if_id)
{
	int ret;
	struct wsm_buf *buf = &hw_priv->wsm_cmd_buf;

	wsm_cmd_lock(hw_priv);

	WSM_PUT8(buf, 0);
	WSM_PUT8(buf, arg->beaconLostCount);
	WSM_PUT16(buf, arg->aid);
	WSM_PUT32(buf, arg->operationalRateSet);

	ret = wsm_cmd_send(hw_priv, buf, NULL, 0x0011, WSM_CMD_TIMEOUT,
			if_id);

	wsm_cmd_unlock(hw_priv);
	return ret;

nomem:
	wsm_buf_reset(buf);
	wsm_cmd_unlock(hw_priv);
	return ret;
}

/* ******************************************************************** */

int wsm_set_tx_queue_params(struct xradio_common *hw_priv,
				const struct wsm_set_tx_queue_params *arg,
				u8 id, int if_id)
{
	int ret;
	struct wsm_buf *buf = &hw_priv->wsm_cmd_buf;
	u8 queue_id_to_wmm_aci[] = {3, 2, 0, 1};

	wsm_cmd_lock(hw_priv);

	WSM_PUT8(buf, queue_id_to_wmm_aci[id]);
	WSM_PUT8(buf, 0);
	WSM_PUT8(buf, arg->ackPolicy);
	WSM_PUT8(buf, 0);
	WSM_PUT32(buf, arg->maxTransmitLifetime);
	WSM_PUT16(buf, arg->allowedMediumTime);
	WSM_PUT16(buf, 0);

	ret = wsm_cmd_send(hw_priv, buf, NULL, 0x0012, WSM_CMD_TIMEOUT, if_id);

	wsm_cmd_unlock(hw_priv);
	return ret;

nomem:
	wsm_buf_reset(buf);
	wsm_cmd_unlock(hw_priv);
	return ret;
}

/* ******************************************************************** */

int wsm_set_edca_params(struct xradio_common *hw_priv,
				const struct wsm_edca_params *arg,
				int if_id)
{
	int ret;
	struct wsm_buf *buf = &hw_priv->wsm_cmd_buf;

	wsm_cmd_lock(hw_priv);

	/* Implemented according to specification. */

	WSM_PUT16(buf, arg->params[3].cwMin);
	WSM_PUT16(buf, arg->params[2].cwMin);
	WSM_PUT16(buf, arg->params[1].cwMin);
	WSM_PUT16(buf, arg->params[0].cwMin);

	WSM_PUT16(buf, arg->params[3].cwMax);
	WSM_PUT16(buf, arg->params[2].cwMax);
	WSM_PUT16(buf, arg->params[1].cwMax);
	WSM_PUT16(buf, arg->params[0].cwMax);

	WSM_PUT8(buf, arg->params[3].aifns);
	WSM_PUT8(buf, arg->params[2].aifns);
	WSM_PUT8(buf, arg->params[1].aifns);
	WSM_PUT8(buf, arg->params[0].aifns);

	WSM_PUT16(buf, arg->params[3].txOpLimit);
	WSM_PUT16(buf, arg->params[2].txOpLimit);
	WSM_PUT16(buf, arg->params[1].txOpLimit);
	WSM_PUT16(buf, arg->params[0].txOpLimit);

	WSM_PUT32(buf, arg->params[3].maxReceiveLifetime);
	WSM_PUT32(buf, arg->params[2].maxReceiveLifetime);
	WSM_PUT32(buf, arg->params[1].maxReceiveLifetime);
	WSM_PUT32(buf, arg->params[0].maxReceiveLifetime);

	ret = wsm_cmd_send(hw_priv, buf, NULL, 0x0013, WSM_CMD_TIMEOUT, if_id);
	wsm_cmd_unlock(hw_priv);
	return ret;

nomem:
	wsm_buf_reset(buf);
	wsm_cmd_unlock(hw_priv);
	return ret;
}

/* ******************************************************************** */

/* ******************************************************************** */
/* WSM indication events implementation					*/

static int wsm_startup_indication(struct xradio_common *hw_priv,
					struct wsm_buf *buf)
{
	u16 status;
	static const char * const fw_types[] = {
			"ETF",
			"WFM",
			"WSM",
			"HI test",
			"Platform test"
	};

	hw_priv->wsm_caps.numInpChBufs	= WSM_GET16(buf);
	hw_priv->wsm_caps.sizeInpChBuf	= WSM_GET16(buf);
	hw_priv->wsm_caps.hardwareId	= WSM_GET16(buf);
	hw_priv->wsm_caps.hardwareSubId	= WSM_GET16(buf);
	status				= WSM_GET16(buf);
	hw_priv->wsm_caps.firmwareCap	= WSM_GET16(buf);
	hw_priv->wsm_caps.firmwareType	= WSM_GET16(buf);
	hw_priv->wsm_caps.firmwareApiVer	= WSM_GET16(buf);
	hw_priv->wsm_caps.firmwareBuildNumber = WSM_GET16(buf);
	hw_priv->wsm_caps.firmwareVersion	= WSM_GET16(buf);
	WSM_GET(buf, &hw_priv->wsm_caps.fw_label[0], WSM_FW_LABEL);
	hw_priv->wsm_caps.fw_label[WSM_FW_LABEL] = 0;
 hw_priv->wsm_caps.firmwareConfig[0] = WSM_GET32(buf);
 hw_priv->wsm_caps.firmwareConfig[1] = WSM_GET32(buf);
 hw_priv->wsm_caps.firmwareConfig[2] = WSM_GET32(buf);

	if (WARN_ON(status))
		return -EINVAL;

	if (WARN_ON(hw_priv->wsm_caps.firmwareType > 4))
		return -EINVAL;

	dev_info(hw_priv->pdev,
		"   Input buffers: %d x %d bytes\n"
		"   Hardware: %d.%d\n"
		"   %s firmware ver: %d, build: %d,"
		    " api: %d, cap: 0x%.4X\n",
		hw_priv->wsm_caps.numInpChBufs,
		hw_priv->wsm_caps.sizeInpChBuf,
		hw_priv->wsm_caps.hardwareId,
		hw_priv->wsm_caps.hardwareSubId,
		fw_types[hw_priv->wsm_caps.firmwareType],
		hw_priv->wsm_caps.firmwareVersion,
		hw_priv->wsm_caps.firmwareBuildNumber,
		hw_priv->wsm_caps.firmwareApiVer,
		hw_priv->wsm_caps.firmwareCap);

	dev_info(hw_priv->pdev, "Firmware Label:%s\n", &hw_priv->wsm_caps.fw_label[0]);

	hw_priv->wsm_caps.firmwareReady = 1;

	wake_up(&hw_priv->wsm_startup_done);
	return 0;

underflow:
	WARN_ON(1);
	return -EINVAL;
}

/* ******************************************************************** */
/* WSM TX								*/

int wsm_cmd_send(struct xradio_common *hw_priv,
		 struct wsm_buf *buf,
		 void *arg, u16 cmd, long tmo, int if_id)
{
	size_t buf_len = buf->data - buf->begin;
	int ret;

	/* CHANGE_MAC addresses only the sole station interface. */
	if (cmd == 6 && buf_len >= 8 && xrw_get16(buf->begin+4) == 0x1046 &&
	    if_id != 0) {
		wsm_buf_reset(buf); return -EOPNOTSUPP;
	}

	if (!xrs_command_allowed(cmd,buf->begin,buf_len,allow_station) &&
	    !(allow_station && xrs_key_command_allowed(cmd,buf->begin,buf_len))) {
		wsm_buf_reset(buf); return -EOPNOTSUPP;
	}

	if (allow_station)
		dev_info(hw_priv->pdev,"XR829 command%04x mib%04x len%zu\n",cmd,
		 cmd == 6 && buf_len >= 8 ? xrw_get16(buf->begin+4) : 0,buf_len);

	if (cmd == 0x0006 || cmd == 0x0005) { /* Write/Read MIB */
		wsm_printk(XRADIO_DBG_MSG, ">>> 0x%.4X [MIB: 0x%.4X] (%d)\n",
			cmd, __le16_to_cpu(((__le16 *)buf->begin)[2]),
			buf_len);
	} else {
		wsm_printk(XRADIO_DBG_MSG, ">>> 0x%.4X (%d)\n", cmd, buf_len);
	}

	if (unlikely(hw_priv->bh_error)) {
		wsm_buf_reset(buf);
		wsm_printk(XRADIO_DBG_ERROR, "bh error!>>> 0x%.4X (%d)\n", cmd, buf_len);
		return -ETIMEDOUT;
	}

	/* Fill HI message header */
	/* BH will add sequence number */

	/* TODO:COMBO: Add if_id from  to the WSM header */
	/* if_id == -1 indicates that command is HW specific,
	 * eg. wsm_configuration which is called during driver initialzation
	 *  (mac80211 .start callback called when first ifce is created. )*/

	/* send hw specific commands on if 0 */
	if (if_id == -1)
		if_id = 0;

	((__le16 *)buf->begin)[0] = __cpu_to_le16(buf_len);
	((__le16 *)buf->begin)[1] = __cpu_to_le16(cmd |
					((is_hardware_xradio(hw_priv)) ? (if_id << 6) : 0));

	spin_lock(&hw_priv->wsm_cmd.lock);
	BUG_ON(hw_priv->wsm_cmd.ptr);
	hw_priv->wsm_cmd.done = 0;
	hw_priv->wsm_cmd.generation++;
	hw_priv->wsm_cmd.ptr = buf->begin;
	hw_priv->wsm_cmd.len = buf_len;
	hw_priv->wsm_cmd.arg = arg;
	hw_priv->wsm_cmd.cmd = cmd;
	spin_unlock(&hw_priv->wsm_cmd.lock);

	xradio_bh_wakeup(hw_priv);

	if (unlikely(hw_priv->bh_error)) {
		/* Do not wait for timeout if BH is dead. Exit immediately. */
		ret = 0;
	} else {
		unsigned long wsm_cmd_max_tmo;

		/* Give start cmd a little more time */
		if (unlikely(tmo == WSM_CMD_START_TIMEOUT))
			wsm_cmd_max_tmo = WSM_CMD_START_TIMEOUT;
		else
			wsm_cmd_max_tmo = WSM_CMD_DEFAULT_TIMEOUT;

		/*Set max timeout.*/
		wsm_cmd_max_tmo = jiffies + wsm_cmd_max_tmo;

		/* Firmware prioritizes data traffic over control confirm.
		 * Loop below checks if data was RXed and increases timeout
		 * accordingly. */
		do {
			/* It's safe to use unprotected access to wsm_cmd.done here */
			ret = wait_event_timeout(hw_priv->wsm_cmd_wq, hw_priv->wsm_cmd.done, tmo);

			/* check time since last rxed and max timeout.*/
		} while (!ret &&
		         time_before_eq(jiffies, hw_priv->rx_timestamp+tmo) &&
		         time_before(jiffies, wsm_cmd_max_tmo));

	}

	if (unlikely(ret == 0)) {
		u16 raceCheck;

		spin_lock(&hw_priv->wsm_cmd.lock);
		raceCheck = hw_priv->wsm_cmd.cmd;
		hw_priv->wsm_cmd.arg = NULL;
		hw_priv->wsm_cmd.ptr = NULL;
		spin_unlock(&hw_priv->wsm_cmd.lock);

		dev_err(hw_priv->pdev, "***CMD timeout!>>> 0x%.4X (%zu), buf_use=%d, bh_state=%d\n",
			   cmd, buf_len, hw_priv->hw_bufs_used, hw_priv->bh_error);
		/* Race condition check to make sure _confirm is not called
		 * after exit of _send */
		if (raceCheck == 0xFFFF) {
			/* If wsm_handle_rx got stuck in _confirm we will hang
			 * system there. It's better than silently currupt
			 * stack or heap, isn't it? */
			BUG_ON(wait_event_timeout(
					hw_priv->wsm_cmd_wq,
					hw_priv->wsm_cmd.done,
					WSM_CMD_LAST_CHANCE_TIMEOUT) <= 0);
		}

		/* Kill BH thread to report the error to the top layer. */
		hw_priv->bh_error = 1;
#ifdef BH_USE_SEMAPHORE
		up(&hw_priv->bh_sem);
#else
		wake_up(&hw_priv->bh_wq);
#endif
		ret = -ETIMEDOUT;
	} else {
		spin_lock(&hw_priv->wsm_cmd.lock);
		BUG_ON(!hw_priv->wsm_cmd.done);
		ret = hw_priv->wsm_cmd.ret;
		spin_unlock(&hw_priv->wsm_cmd.lock);
	}
	wsm_buf_reset(buf);
	return ret;
}

/* ******************************************************************** */
/* WSM TX port control							*/

/* ******************************************************************** */
/* WSM RX								*/

int wsm_handle_exception(struct xradio_common *hw_priv, u8 *data, size_t len)
{
	struct wsm_buf buf;
	u32 reason;
	u32 reg[18];
	char fname[48];
	int i = 0;

	static const char * const reason_str[] = {
		"undefined instruction",
		"prefetch abort",
		"data abort",
		"unknown error",
	};

	buf.begin = buf.data = data;
	buf.end = &buf.begin[len];

	reason = WSM_GET32(&buf);
	for (i = 0; i < ARRAY_SIZE(reg); ++i)
		reg[i] = WSM_GET32(&buf);
	WSM_GET(&buf, fname, sizeof(fname));

	if (reason < 4) {
		dev_err(hw_priv->pdev, "Firmware exception: %s.\n",
		           reason_str[reason]);
	} else {
		dev_err(hw_priv->pdev, "Firmware assert at %.*s, line %d, reason=0x%x\n",
			       (int) sizeof(fname), fname, reg[1], reg[2]);
	}

	for (i = 0; i < 12; i += 4) {
		dev_err(hw_priv->pdev, "Firmware:" \
		           "R%d: 0x%.8X, R%d: 0x%.8X, R%d: 0x%.8X, R%d: 0x%.8X,\n",
		           i + 0, reg[i + 0], i + 1, reg[i + 1],
		           i + 2, reg[i + 2], i + 3, reg[i + 3]);
	}
	dev_err(hw_priv->pdev, "Firmware:" \
	           "R12: 0x%.8X, SP: 0x%.8X, LR: 0x%.8X, PC: 0x%.8X,\n",
	           reg[i + 0], reg[i + 1], reg[i + 2], reg[i + 3]);
	i += 4;
	dev_err(hw_priv->pdev, "Firmware:CPSR: 0x%.8X, SPSR: 0x%.8X\n",
	           reg[i + 0], reg[i + 1]);

	return 0;

underflow:
	dev_err(hw_priv->pdev, "Firmware exception.\n");
#if defined(CONFIG_XRADIO_DEBUG)
	print_hex_dump_bytes("Exception: ", DUMP_PREFIX_NONE, data, len);
#endif /* CONFIG_XRADIO_DEBUG */
	return -EINVAL;
}

int wsm_handle_rx(struct xradio_common *hw, int id,
                  struct wsm_hdr *wsm, struct sk_buff **skb_p)
{
 struct wsm_buf buf;
 struct wsm_cmd *cmd = &hw->wsm_cmd;
 struct xrw_scan_complete complete;
 void *arg;
 u16 pending;
 int ret = 0;
 int interface_link_id = (id >> 6) & 0x0f;

 id &= ~WSM_TX_LINK_ID(WSM_TX_LINK_ID_MAX);
 if (le16_to_cpu(wsm->len) < sizeof(*wsm) ||
     le16_to_cpu(wsm->len) > (*skb_p)->len)
  return -EINVAL;
 buf.begin = (u8 *)wsm;
 buf.data = (u8 *)&wsm[1];
 buf.end = buf.begin + le16_to_cpu(wsm->len);

 switch (id) {
 case 0x0801:
  return wsm_startup_indication(hw, &buf);
 case 0x0804:
  if (xrs_runtime_joined(hw))
   return xrs_runtime_rx(hw, buf.data, buf.end - buf.data);
  return xr829_scan_rx(hw, buf.data, buf.end - buf.data);
	case 0x0809:
		if (!interface_link_id)
			xrs_runtime_pm_complete(hw);
		return 0;
 case 0x0806:
  if (xrw_scan_complete_decode(&complete, buf.data, buf.end - buf.data) < 0)
   return -EINVAL;
  xr829_scan_done(hw, complete.status, complete.channels);
  return 0;
 case 0x0406: /* write MIB */
 case 0x0407: /* scan */
 case 0x0409: /* configuration */
 case 0x040a: /* reset */
 case 0x040b: /* join */
 case 0x040c: /* add key */
 case 0x040d: /* remove key */
	case 0x0410: /* power-management request */
 case 0x0411: /* BSS parameters */
 case 0x0412: /* TX queue parameters */
 case 0x0413: /* EDCA parameters */
  break;
 default:
  dev_dbg_ratelimited(hw->pdev, "ignoring WSM id=%#x len=%u\n",
                      id, le16_to_cpu(wsm->len));
  return 0;
 }

 /* TX confirmations are handled by BH's station decoder. Claim command
  * arguments only after matching the outstanding synchronous request. */
 spin_lock(&cmd->lock);
 arg = cmd->arg;
 pending = cmd->cmd & ~WSM_TX_LINK_ID(WSM_TX_LINK_ID_MAX);
 if ((id & ~0x0400) != pending) {
  spin_unlock(&cmd->lock);
  dev_warn_ratelimited(hw->pdev,
       "ignoring unmatched WSM confirmation %#x (pending %#x)\n", id, pending);
  return 0;
 }
 cmd->cmd = 0xffff;
 spin_unlock(&cmd->lock);

 switch (id) {
 case 0x0409:
  if (arg) ret = wsm_configuration_confirm(hw, arg, &buf);
  break;
 case 0x0406:
  if (arg) ret = wsm_write_mib_confirm(hw, arg, &buf, interface_link_id);
  break;
 case 0x040b:
  if (arg) ret = wsm_join_confirm(hw, arg, &buf);
  break;
 default:
  WARN_ON(arg != NULL);
  ret = wsm_generic_confirm(hw, arg, &buf);
  break;
 }
 spin_lock(&cmd->lock);
 cmd->ret = ret;
 cmd->done = 1;
 spin_unlock(&cmd->lock);
 wake_up(&hw->wsm_cmd_wq);
 /* Firmware rejection completes the caller; it need not terminate BH. */
 return 0;
}

/* Commands use a BH-owned copy so a producer timeout cannot overwrite an
 * in-flight SDIO write. Data ownership belongs exclusively to the station
 * runtime; the obsolete vendor AP/P2P queues are never selected. */
int wsm_get_tx(struct xradio_common *hw, u8 **data, size_t *tx_len,
               int *burst, int *vif_selected)
{
 struct wsm_cmd *cmd = &hw->wsm_cmd;
 u8 *copy;

 *burst = 1;
 *vif_selected = -1;
 spin_lock(&cmd->lock);
 if (!cmd->ptr) {
  spin_unlock(&cmd->lock);
  return xrs_runtime_get_tx(hw, data, tx_len);
 }
 copy = xrs_runtime_command_buffer(hw);
 if (!copy || cmd->len < 4 || cmd->len > 4096) {
  spin_unlock(&cmd->lock);
  return -EMSGSIZE;
 }
 memset(copy, 0, 4096);
 memcpy(copy, cmd->ptr, cmd->len);
 *data = copy;
 *tx_len = cmd->len;
 cmd->copied_generation = cmd->generation;
 spin_unlock(&cmd->lock);
 return 1;
}

void wsm_txed(struct xradio_common *hw, u8 *data)
{
 struct wsm_cmd *cmd = &hw->wsm_cmd;
 spin_lock(&cmd->lock);
 /* A timed-out command may have been replaced during its SDIO write. */
 if (data == xrs_runtime_command_buffer(hw) &&
     xrs_command_copy_current(cmd->generation, cmd->copied_generation))
  cmd->ptr = NULL;
 spin_unlock(&cmd->lock);
}

/* ******************************************************************** */
/* WSM buffer								*/

void wsm_buf_init(struct wsm_buf *buf)
{
	size_t size = WSM_CMD_BUFFER_INITIAL;
	BUG_ON(buf->begin);
	buf->begin = kmalloc(size, GFP_KERNEL);
	buf->end = buf->begin ? &buf->begin[size] : buf->begin;
	wsm_buf_reset(buf);
}

void wsm_buf_deinit(struct wsm_buf *buf)
{
	if(buf->begin)
		kfree(buf->begin);
	buf->begin = buf->data = buf->end = NULL;
}

static void wsm_buf_reset(struct wsm_buf *buf)
{
	if (buf->begin) {
		buf->data = &buf->begin[4];
		*(u32 *)buf->begin = 0;
	} else
		buf->data = buf->begin;
}

static int wsm_buf_reserve(struct wsm_buf *buf, size_t extra_size)
{
 size_t pos, size;
 u8 *begin;

 if (!buf->begin)
  return -ENOMEM;
 pos = buf->data - buf->begin;
 if (check_add_overflow(pos, extra_size, &size))
  return -EOVERFLOW;
 if (size > WSM_CMD_BUFFER_MAX)
  return -EMSGSIZE;
 if (size <= (size_t)(buf->end - buf->begin))
  return 0;
 begin = krealloc(buf->begin, size, GFP_KERNEL);
 if (!begin)
  return -ENOMEM;
 buf->begin = begin;
 buf->data = begin + pos;
 buf->end = begin + size;
 return 0;
}

/* Synchronous key transport. The BH owns its own copy before SDIO submission. */
int wsm_station_key_command(struct xradio_common *hw,u16 command,
                            const u8 *payload,size_t size)
{
 struct wsm_buf *buf=&hw->wsm_cmd_buf;int ret;
 if(!allow_station || (command!=12 && command!=13) ||
    size!=(command==12?44:4))return -EINVAL;
 wsm_cmd_lock(hw);
 WSM_PUT(buf,payload,size);
 ret=wsm_cmd_send(hw,buf,NULL,command,WSM_CMD_TIMEOUT,0);
 if (buf->begin)
  memzero_explicit(buf->begin,buf->end-buf->begin);
 wsm_buf_reset(buf);wsm_cmd_unlock(hw);return ret;
nomem:
 if (buf->begin)
  memzero_explicit(buf->begin,buf->end-buf->begin);
 wsm_buf_reset(buf);wsm_cmd_unlock(hw);return ret;
}

/* Caller owns the station transaction through the separate PM indication. */
int wsm_set_pm(struct xradio_common *hw, const struct wsm_set_pm *arg, int if_id)
{
	struct wsm_buf *buf = &hw->wsm_cmd_buf;
	int ret;

	wsm_cmd_lock(hw);
	WSM_PUT8(buf, arg->pmMode);
	WSM_PUT8(buf, arg->fastPsmIdlePeriod);
	WSM_PUT8(buf, arg->apPsmChangePeriod);
	WSM_PUT8(buf, arg->minAutoPsPollPeriod);
	ret = wsm_cmd_send(hw, buf, NULL, 0x0010, WSM_CMD_TIMEOUT, if_id);
	wsm_cmd_unlock(hw);
	return ret;
nomem:
	wsm_buf_reset(buf);
	wsm_cmd_unlock(hw);
	return ret;
}

// SPDX-License-Identifier: GPL-2.0-only
#include <linux/firmware.h>
#include <linux/mmc/sdio_func.h>
#include "xradio.h"
#include "bh.h"
#include "sdio.h"
#include "fwio.h"
#include "recovery.h"
#include "sdio-reset.h"
#include "scan-only.h"
#include "station-runtime.h"

/* Recovery is required for transport errors and bounded TX generations. */
const bool allow_recovery = true;
const unsigned int recovery_test_limit = 0;

/* All command callers and runtime workers must already be serialized before
 * stop. Joining BH is the host callback boundary; an empty slot is not one. */
void xrs_transport_stop(struct xradio_common *hw)
{
 xrs_sdio_irq_release(hw);
 if(hw->bh_running) {
  xradio_unregister_bh(hw);
  hw->bh_running=false;
 }
 xradio_dev_deinit(hw);
 hw->firmware_loaded=false;
}

int xrs_transport_start(struct xradio_common *hw)
{
 int ret;
 if(hw->bh_running || hw->irq_claimed || !hw->function_enabled)return -EINVAL;
 ret=xradio_register_bh(hw);if(ret)return ret;
 hw->bh_running=true;
 ret=xrs_sdio_irq_claim(hw);if(ret)goto fail;
 ret=xradio_load_firmware(hw);if(ret)goto fail;
 hw->firmware_loaded=true;
 sdio_lock(hw);
 ret=sdio_set_blk_size(hw,SDIO_BLOCK_SIZE);
 sdio_unlock(hw);
 if(ret)goto fail;
 if(wait_event_interruptible_timeout(hw->wsm_startup_done,
       hw->wsm_caps.firmwareReady,3*HZ)<=0) {
  ret=-ETIMEDOUT;goto fail;
 }
 dev_info(hw->pdev,"XR829 fresh STARTUP epoch=%llu\n",hw->transport_epoch);
 return 0;
fail:
 xrs_transport_stop(hw);
 return ret;
}

/* Only called with IRQ released, BH joined, all command callers serialized,
 * old scans notified and runtime work canceled, AFTER successful MMC reset. */
static void new_transport_epoch(struct xradio_common *hw)
{
 hw->buf_id_tx=hw->buf_id_rx=0;
 hw->wsm_tx_seq=hw->wsm_rx_seq=0;
 hw->hw_bufs_used=0;
 memset(hw->hw_bufs_used_vif,0,sizeof(hw->hw_bufs_used_vif));
 memset(&hw->wsm_caps,0,sizeof(hw->wsm_caps));
 hw->wsm_cmd.ptr=NULL;hw->wsm_cmd.arg=NULL;hw->wsm_cmd.len=0;
 hw->wsm_cmd.cmd=0xffff;hw->wsm_cmd.done=0;hw->wsm_cmd.ret=0;
 /* Keep command generation monotonic; no saved host callback can be reused. */
 if(hw->wsm_cmd_buf.begin) {
  hw->wsm_cmd_buf.data=hw->wsm_cmd_buf.begin+4;
  memset(hw->wsm_cmd_buf.begin,0,4);
 }
 atomic_set(&hw->tx_lock,0);
 hw->pending_frame_id=0;
 hw->device_can_sleep=false;
 hw->rx_timestamp=jiffies;
 dev_kfree_skb(hw->skb_cache);hw->skb_cache=NULL;
 xradio_deinit_resv_skb(hw);
 xradio_init_resv_skb(hw);
 xr829_scan_new_epoch(hw);
 xrs_runtime_new_epoch(hw);
 hw->transport_epoch++;
 WRITE_ONCE(hw->bh_error,0);
}

int xrs_recovery_sync_locked(struct xradio_common *hw)
{
 struct ieee80211_vif *vif;
 int ret = 0;
 bool reset_attempted = false;
 bool restart_attempted = false;
 /* One critical section covers disconnect through complete hardware restore.
  * No queued mac80211 restart token can race an intervening down/up operation.
  * Keep mac80211's registered/started state; restore a disconnected radio. */
 lockdep_assert_wiphy(hw->hw->wiphy);
 if (hw->recovery_in_progress) return -EINPROGRESS;
	if (hw->pm_stopped)
		return -EBUSY;
 if (READ_ONCE(hw->removing)) return -ENODEV;
 if (READ_ONCE(hw->recovery_state) == XRS_DEAD) return -EIO;
 hw->recovery_in_progress = true;
 WRITE_ONCE(hw->recovery_state, XRS_QUIESCING);
 ieee80211_stop_queues(hw->hw);
 vif=hw->recovery_vif;
 xrs_runtime_quiesce(hw);
 /* Publish every accepted scan completion before flushing stack work. BH may
  * concurrently finish a scan, so quiesce waits for that notifier to return. */
 ret=xr829_scan_quiesce(hw);if(ret)goto dead;
 if(vif && vif->cfg.assoc)ieee80211_connection_loss(vif);
 wiphy_work_flush(hw->hw->wiphy,NULL);
 if(vif && vif->cfg.assoc) {ret=-EBUSY;goto dead;}
 /* Runtime stop drains accepted packets with its bounded deadline; failure
  * remains quarantined. No synthetic command completion can race a BH arg. */
 xrs_runtime_stop(hw);
 xrs_transport_stop(hw);
 ret=xrs_runtime_detached(hw);if(ret)goto dead;
 WRITE_ONCE(hw->recovery_state,XRS_RESETTING);
 reset_attempted = true;
 ret=xrs_sdio_power_reset(hw->sdio_func,&hw->function_enabled);
 if(ret)goto dead;
 new_transport_epoch(hw);
 WRITE_ONCE(hw->recovery_state,XRS_RESTORING);
 restart_attempted = true;
 ret=xrs_transport_start(hw);if(ret)goto dead;
 /* Same configuration/address paths as normal ->start and ->add_interface.
  * Existing VIF pointer remains valid under wiphy mutex. No saved key state
  * is replayed; a fresh association installs all subsequent keys. */
 if(vif) {
  ret=xr829_restore_interface(hw,vif);
  if(ret)goto dead;
 }
 WRITE_ONCE(hw->recovery_state,XRS_RUNNING);
 ieee80211_wake_queues(hw->hw);
 dev_info(hw->pdev,"XR829 synchronous recovery complete epoch=%llu; fresh association required\n",hw->transport_epoch);
 goto out;
dead:
 /* Stop the host transport, including a partially restored transport.
  * The first reset no longer fences RF once firmware restart was attempted.
  * A failed physical reset is not retried here: remain terminal and report it. */
 xrs_transport_stop(hw);
 if (!reset_attempted || restart_attempted) {
  int reset_ret = xrs_sdio_power_reset(hw->sdio_func, &hw->function_enabled);
  if (reset_ret)
   dev_err(hw->pdev, "XR829 mandatory RF reset failed: %d\n", reset_ret);
  else
   dev_info(hw->pdev, "XR829 recovery failure fenced by WLAN reset\n");
 }
 WRITE_ONCE(hw->recovery_state,XRS_DEAD);
 WRITE_ONCE(hw->bh_error,1);
 ieee80211_stop_queues(hw->hw);
 /* Leave owned resources for normal removal; never reopen uncertain state. */
 dev_err(hw->pdev,"XR829 recovery stopped at epoch=%llu error=%d\n",hw->transport_epoch,ret);
out:
 hw->recovery_in_progress = false;
 return ret;
}

static void recovery_work(struct work_struct *work)
{
 struct xradio_common *hw = container_of(work, struct xradio_common, recovery_work);
 wiphy_lock(hw->hw->wiphy);
 /* A synchronous enforcement callback may have satisfied this queued work. */
 if (READ_ONCE(hw->recovery_state) == XRS_QUIESCING)
  xrs_recovery_sync_locked(hw);
 wiphy_unlock(hw->hw->wiphy);
}

void xrs_recovery_request(struct xradio_common *hw)
{
	unsigned long flags;

	if (!allow_recovery)
		return;
	spin_lock_irqsave(&hw->recovery_lock, flags);
	if (!hw->removing && !hw->pm_prepared &&
	    hw->recovery_state == XRS_RUNNING) {
		hw->recovery_state = XRS_QUIESCING;
		ieee80211_stop_queues(hw->hw);
		/* Independent from the runtime workqueue that recovery joins. */
		queue_work(system_long_wq, &hw->recovery_work);
	}
	spin_unlock_irqrestore(&hw->recovery_lock, flags);
}
static void recovery_health(struct work_struct *work)
{
 struct xradio_common *hw=container_of(to_delayed_work(work),struct xradio_common,recovery_health);
 /* Scan cancellation/timeout also leaves firmware ownership uncertain.
  * Only RUNNING may request recovery: our own quiesce poisons active scans. */
 if(READ_ONCE(hw->recovery_state)==XRS_RUNNING &&
    (READ_ONCE(hw->bh_error) || xr829_scan_poisoned(hw)))
  xrs_recovery_request(hw);
 xrs_recovery_monitor(hw);
}
void xrs_recovery_monitor(struct xradio_common *hw)
{
	unsigned long flags;

	spin_lock_irqsave(&hw->recovery_lock, flags);
	if (allow_recovery && !hw->removing && !hw->pm_prepared &&
	    hw->recovery_state != XRS_DEAD)
		queue_delayed_work(system_long_wq, &hw->recovery_health, HZ);
	spin_unlock_irqrestore(&hw->recovery_lock, flags);
}
void xrs_recovery_init(struct xradio_common *hw)
{
 spin_lock_init(&hw->recovery_lock);
 INIT_WORK(&hw->recovery_work,recovery_work);
 INIT_DELAYED_WORK(&hw->recovery_health,recovery_health);
 hw->recovery_state=XRS_RUNNING;
}
void xrs_recovery_cancel(struct xradio_common *hw)
{
 unsigned long flags;
 spin_lock_irqsave(&hw->recovery_lock,flags);hw->removing=true;
 spin_unlock_irqrestore(&hw->recovery_lock,flags);
 cancel_delayed_work_sync(&hw->recovery_health);
 cancel_work_sync(&hw->recovery_work);
}

/* Prepare runs before the wiphy child suspends, without its mutex held.
 * Drain an already queued recovery; canceling it could strand QUIESCING.
 */
int xrs_pm_prepare(struct xradio_common *hw)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&hw->recovery_lock, flags);
	if (hw->removing || hw->pm_prepared) {
		spin_unlock_irqrestore(&hw->recovery_lock, flags);
		return -EBUSY;
	}
	hw->pm_prepared = true;
	spin_unlock_irqrestore(&hw->recovery_lock, flags);
	cancel_delayed_work_sync(&hw->recovery_health);
	flush_work(&hw->recovery_work);
	wiphy_lock(hw->hw->wiphy);
	if (hw->recovery_state != XRS_RUNNING || hw->pm_stopped || hw->pm_error)
		ret = -EIO;
	wiphy_unlock(hw->hw->wiphy);
	if (ret)
		xrs_pm_complete(hw);
	return ret;
}

/* cfg80211 has disconnected and mac80211 removed all interfaces. This is
 * also used by the SDIO parent when no interface was open (no ->stop call).
 */
int xrs_pm_stop_locked(struct xradio_common *hw)
{
	int ret, detached;

	lockdep_assert_wiphy(hw->hw->wiphy);
	if (hw->pm_stopped)
		return hw->pm_error;
	if (!hw->pm_prepared || hw->recovery_vif || hw->scan_only_vif)
		return -EBUSY;
	xrs_runtime_quiesce(hw);
	ret = xr829_scan_quiesce(hw);
	xrs_runtime_stop(hw);
	xrs_transport_stop(hw);
	detached = xrs_runtime_detached(hw);
	if (!ret)
		ret = detached;
	if (!ret && (hw->irq_claimed || hw->bh_running))
		ret = -EBUSY;
	hw->pm_stopped = true;
	if (ret && !hw->irq_claimed && !hw->bh_running) {
		int reset_ret = xrs_sdio_power_reset(hw->sdio_func,
						   &hw->function_enabled);

		if (reset_ret)
			dev_err(hw->pdev, "WLAN stop failure reset failed: %d\n", reset_ret);
	}
	hw->pm_error = ret;
	if (ret)
		WRITE_ONCE(hw->bh_error, 1);
	WRITE_ONCE(hw->recovery_state, ret ? XRS_DEAD : XRS_QUIESCING);
	return ret;
}

/* Called before the wiphy child resumes; ->start also handles an abort
 * after child ->stop but before the SDIO parent reached ->suspend.
 */
int xrs_pm_restore_locked(struct xradio_common *hw)
{
	int ret;
	bool restarted = false;

	lockdep_assert_wiphy(hw->hw->wiphy);
	if (hw->pm_error)
		return hw->pm_error;
	if (!hw->pm_stopped)
		return 0;
	/* A failed MMC parent resume may still be followed by child resume.
	 * Do not reset/reload while MMC retains suspended or removed state.
	 */
	sdio_claim_host(hw->sdio_func);
	ret = sdio_check_card_state(hw->sdio_func);
	sdio_release_host(hw->sdio_func);
	if (ret)
		goto dead;
	WRITE_ONCE(hw->recovery_state, XRS_RESETTING);
	ret = xrs_sdio_power_reset(hw->sdio_func, &hw->function_enabled);
	if (ret)
		goto dead;
	new_transport_epoch(hw);
	WRITE_ONCE(hw->recovery_state, XRS_RESTORING);
	restarted = true;
	ret = xrs_transport_start(hw);
	if (ret)
		goto dead;
	hw->pm_stopped = false;
	WRITE_ONCE(hw->recovery_state, XRS_RUNNING);
	/* Driver STOP reason is independent from mac80211's SUSPEND reason. */
	ieee80211_wake_queues(hw->hw);
	return 0;
dead:
	xrs_transport_stop(hw);
	if (restarted) {
		int reset_ret = xrs_sdio_power_reset(hw->sdio_func,
						   &hw->function_enabled);

		if (reset_ret)
			dev_err(hw->pdev, "WLAN restore failure reset failed: %d\n", reset_ret);
	}
	hw->pm_error = ret;
	WRITE_ONCE(hw->bh_error, 1);
	WRITE_ONCE(hw->recovery_state, XRS_DEAD);
	ieee80211_stop_queues(hw->hw);
	dev_err(hw->pdev, "WLAN suspend restore failed: %d; interface remains stopped\n", ret);
	return ret;
}

void xrs_pm_complete(struct xradio_common *hw)
{
	unsigned long flags;

	/* Also restore a closed interface after an aborted device suspend. */
	wiphy_lock(hw->hw->wiphy);
	xrs_pm_restore_locked(hw);
	wiphy_unlock(hw->hw->wiphy);
	spin_lock_irqsave(&hw->recovery_lock, flags);
	hw->pm_prepared = false;
	spin_unlock_irqrestore(&hw->recovery_lock, flags);
	xrs_recovery_monitor(hw);
}

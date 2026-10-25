/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XRS_RECOVERY_H
#define XRS_RECOVERY_H
struct xradio_common;
struct ieee80211_hw;
enum xrs_recovery_state {
 XRS_RUNNING, XRS_QUIESCING, XRS_RESETTING, XRS_RESTORING, XRS_DEAD,
};
extern const bool allow_recovery;
extern const unsigned int recovery_test_limit;
void xrs_recovery_init(struct xradio_common *hw);
int xrs_recovery_sync_locked(struct xradio_common *hw);
void xrs_recovery_request(struct xradio_common *hw);
void xrs_recovery_cancel(struct xradio_common *hw);
void xrs_recovery_monitor(struct xradio_common *hw);
int xrs_transport_start(struct xradio_common *hw);
void xrs_transport_stop(struct xradio_common *hw);
int xrs_sdio_irq_claim(struct xradio_common *hw);
void xrs_sdio_irq_release(struct xradio_common *hw);
int xrs_pm_prepare(struct xradio_common *hw);
void xrs_pm_complete(struct xradio_common *hw);
int xrs_pm_stop_locked(struct xradio_common *hw);
int xrs_pm_restore_locked(struct xradio_common *hw);
#endif

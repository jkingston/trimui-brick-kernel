#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
source_file="$root/drivers/net/wireless/xradio/station-runtime.c"
recovery_file="$root/drivers/net/wireless/xradio/recovery.c"

grep -q 'station_ps_update(s, vif->cfg.ps' "$source_file"
if grep -Eq 'station_ps_update\([^;]*IEEE80211_CONF_PS' "$source_file"; then
	echo 'FAIL: dynamic-PS driver consumes global IEEE80211_CONF_PS' >&2
	exit 1
fi
grep -q 'static unsigned int join_timeout_ms;' "$source_file"
grep -q 'xrs_runtime_join_stalled(hw)' "$recovery_file"

echo 'PASS: XR829 dynamic PS and opt-in JOIN-recovery source contracts'

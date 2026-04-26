#!/bin/bash
# Write-staging drain-active tier mask smoke test.
#
# The drain mask is distinct from metadata_active_tier_mask: SmoothNAS uses it
# to tell future staged-data drain code which tiers are already active due to
# external activity. The kernel always forces the fastest-tier bit on.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"

UUID=00000000-0000-0000-0000-00000000f207
SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-drain-mask}
export SPILL_ROOT SPILL_UUID=$UUID
trap spill_cleanup EXIT

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool drainmask "$UUID"

SYSFS="/sys/fs/smoothfs/$UUID"
if [ ! -d "$SYSFS" ]; then
	echo "  FAIL  missing smoothfs sysfs pool $SYSFS"
	exit 1
fi

echo "=== default drain mask permits fastest tier only ==="
spill_assert test "$(cat "$SYSFS/write_staging_drain_active_tier_mask")" = "0x1"

echo "=== slow tier can be marked externally active for drains ==="
echo 0x3 > "$SYSFS/write_staging_drain_active_tier_mask"
spill_assert test "$(cat "$SYSFS/write_staging_drain_active_tier_mask")" = "0x3"

echo "=== fastest tier bit is forced back on ==="
echo 0x2 > "$SYSFS/write_staging_drain_active_tier_mask"
spill_assert test "$(cat "$SYSFS/write_staging_drain_active_tier_mask")" = "0x3"

echo "=== out-of-range bits are masked off ==="
echo 0xff > "$SYSFS/write_staging_drain_active_tier_mask"
spill_assert test "$(cat "$SYSFS/write_staging_drain_active_tier_mask")" = "0x3"

spill_finish "write_staging_drain_mask"

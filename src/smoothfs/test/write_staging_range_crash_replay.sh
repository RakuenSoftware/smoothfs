#!/bin/bash
# Range-staging crash/replay smoke harness (Phase 6O).
#
# Stages a range write while the source tier is NOT drain-active, then
# simulates a crash by unmounting + reloading the module, remounts the
# pool, and verifies that:
#
#   1. The read-merge view is restored (smoothfs reports the staged
#      bytes overlaid on the original lower file) WITHOUT touching the
#      source tier (no drain has happened yet).
#   2. range_staging_recovered_bytes / _writes report the replay.
#   3. range_staging_recovery_pending equals the staged bytes.
#   4. recovered_range_tier_mask names the source tier.
#   5. After SmoothNAS marks the source tier drain-active, the recovered
#      range drains to the source tier and recovery_pending returns to 0.
#
# A second crash mid-drain is exercised by re-staging a fresh range and
# unmounting again before drain-active completes — the new mount must
# also recover that range without data loss.
#
# This is a pragmatic harness rather than a true crash injector. The
# unmount + rmmod cycle still exercises the mount-time replay path that
# is the load-bearing piece of the 6O acceptance criteria.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"

UUID=00000000-0000-0000-0000-00000000f207
SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-write-staging-range-replay}
export SPILL_ROOT SPILL_UUID=$UUID
trap spill_cleanup EXIT

remount_pool() {
	umount "$SPILL_ROOT/server"
	rmmod smoothfs
	modprobe smoothfs
	mount -t smoothfs \
		-o "pool=writestagerangereplay,uuid=$UUID,tiers=$SPILL_ROOT/fast:$SPILL_ROOT/slow" \
		none "$SPILL_ROOT/server"
	# write_staging_enabled is per-sb and does not survive remount —
	# re-enable so the next stage write actually goes through the
	# range-staging admission path instead of writing direct to the
	# (slow) lower.
	echo 1 > "/sys/fs/smoothfs/$UUID/write_staging_enabled"
	echo 98 > "/sys/fs/smoothfs/$UUID/write_staging_full_pct"
}

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool writestagerangereplay "$UUID"

SYSFS="/sys/fs/smoothfs/$UUID"
if [ ! -d "$SYSFS" ]; then
	echo "  FAIL  missing smoothfs sysfs pool $SYSFS"
	exit 1
fi
echo 1 > "$SYSFS/write_staging_enabled"
echo 98 > "$SYSFS/write_staging_full_pct"
spill_assert test "$(cat "$SYSFS/range_staging_recovery_supported")" = "1"

printf 'abcdefghij\n' > "$SPILL_ROOT/slow/range.txt"
spill_assert test ! -e "$SPILL_ROOT/fast/range.txt"

echo "=== buffered range-stage write through smoothfs ==="
# bs=3 seek=1 count=1 issues a single 3-byte pwrite at offset 3.
# Using bs=1 here would split the overlay into three 1-byte writes
# which the kernel correctly counts as recovered_writes=3 rather
# than the 1 the post-replay assertion below asserts.
printf 'XYZ' | dd of="$SPILL_ROOT/server/range.txt" bs=3 seek=1 count=1 conv=notrunc status=none
spill_assert grep -qx 'abcdefghij' "$SPILL_ROOT/slow/range.txt"
spill_assert grep -qx 'abcXYZghij' "$SPILL_ROOT/server/range.txt"
spill_assert test "$(cat "$SYSFS/range_staged_bytes")" = "3"

echo "=== crash before drain: unmount + rmmod + remount ==="
remount_pool

SYSFS="/sys/fs/smoothfs/$UUID"
spill_assert test -d "$SYSFS"
spill_assert test "$(cat "$SYSFS/range_staging_recovered_bytes")" = "3"
spill_assert test "$(cat "$SYSFS/range_staging_recovered_writes")" = "1"
spill_assert test "$(cat "$SYSFS/range_staging_recovery_pending")" = "3"
recovery_reason=$(cat "$SYSFS/last_recovery_reason")
spill_assert test "$recovery_reason" = "remount-replay"
mask=$(cat "$SYSFS/recovered_range_tier_mask")
if [ "$mask" != "0x2" ]; then
	echo "  FAIL  recovered_range_tier_mask=$mask, want 0x2"
	spill_rc=1
fi

echo "=== read-merge view available pre-drain (no source-tier wake) ==="
spill_assert grep -qx 'abcXYZghij' "$SPILL_ROOT/server/range.txt"
spill_assert grep -qx 'abcdefghij' "$SPILL_ROOT/slow/range.txt"

echo "=== drain after source tier is marked active ==="
echo 0x3 > "$SYSFS/write_staging_drain_active_tier_mask"
spill_assert grep -qx 'abcXYZghij' "$SPILL_ROOT/slow/range.txt"
spill_assert test "$(cat "$SYSFS/range_staged_bytes")" = "0"
spill_assert test "$(cat "$SYSFS/range_staging_recovery_pending")" = "0"
spill_assert test "$(cat "$SYSFS/last_drain_reason")" = "range-staged-drain"

echo "=== crash during drain: re-stage + unmount mid-cycle ==="
echo 0x1 > "$SYSFS/write_staging_drain_active_tier_mask"
# Single 3-byte pwrite at offset 6 (bs=3 seek=2 count=1) overlays
# positions 6,7,8 (g,h,i) with 1,2,3 -> "abcXYZ123j". Older revisions
# of this harness used bs=1 (3 separate writes) and asserted on
# "abcXYZ123ij", but that 11-char string is unreachable: 3 bytes
# written at offset 6 of "abcXYZghij" cannot leave both 'i' and 'j'.
printf '123' | dd of="$SPILL_ROOT/server/range.txt" bs=3 seek=2 count=1 conv=notrunc status=none
spill_assert test "$(cat "$SYSFS/range_staged_bytes")" = "3"
remount_pool

SYSFS="/sys/fs/smoothfs/$UUID"
spill_assert test "$(cat "$SYSFS/range_staging_recovered_bytes")" = "3"
spill_assert grep -qx 'abcXYZ123j' "$SPILL_ROOT/server/range.txt"

echo 0x3 > "$SYSFS/write_staging_drain_active_tier_mask"
spill_assert grep -qx 'abcXYZ123j' "$SPILL_ROOT/slow/range.txt"
spill_assert test "$(cat "$SYSFS/range_staging_recovery_pending")" = "0"

spill_finish "write_staging_range_crash_replay"

#!/bin/bash
# Write-staging truncate smoke test.
#
# Seeds a file directly on the colder tier, enables write staging, then
# performs a replace-style write through smoothfs. The cold-tier file must not
# be truncated; the replacement lands on the fastest tier and is visible
# through the smoothfs mount.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"

UUID=00000000-0000-0000-0000-00000000f205
SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-write-staging}
export SPILL_ROOT SPILL_UUID=$UUID
trap spill_cleanup EXIT

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool writestage "$UUID"

SYSFS="/sys/fs/smoothfs/$UUID"
if [ ! -d "$SYSFS" ]; then
	echo "  FAIL  missing smoothfs sysfs pool $SYSFS"
	exit 1
fi
echo 1 > "$SYSFS/write_staging_enabled"
echo 98 > "$SYSFS/write_staging_full_pct"

printf 'cold-original\n' > "$SPILL_ROOT/slow/cold.txt"
spill_assert test ! -e "$SPILL_ROOT/fast/cold.txt"

echo "=== replace cold-tier file through smoothfs ==="
printf 'fast-replacement\n' > "$SPILL_ROOT/server/cold.txt"

spill_assert test -e "$SPILL_ROOT/fast/cold.txt"
spill_assert grep -qx 'fast-replacement' "$SPILL_ROOT/fast/cold.txt"
spill_assert grep -qx 'fast-replacement' "$SPILL_ROOT/server/cold.txt"
spill_assert grep -qx 'cold-original' "$SPILL_ROOT/slow/cold.txt"

staged_bytes=$(cat "$SYSFS/staged_bytes")
full_pct=$(cat "$SYSFS/write_staging_full_pct")
oldest=$(cat "$SYSFS/oldest_staged_write_at")
reason=$(cat "$SYSFS/last_drain_reason")

if [ "$staged_bytes" -le 0 ]; then
	echo "  FAIL  staged_bytes=$staged_bytes"
	spill_rc=1
else
	echo "  ok    staged_bytes=$staged_bytes"
fi
if [ "$oldest" -le 0 ]; then
	echo "  FAIL  oldest_staged_write_at=$oldest"
	spill_rc=1
else
	echo "  ok    oldest_staged_write_at=$oldest"
fi
spill_assert test "$reason" = "staged-write"
spill_assert test "$full_pct" = "98"

spill_finish "write_staging_truncate"

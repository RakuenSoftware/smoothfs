#!/bin/bash
# Write-staging range smoke test.
#
# Seeds a file directly on the colder tier, enables write staging, then
# performs a non-truncating buffered write through smoothfs. The original lower
# file remains on the colder tier, the changed range lands in the fastest tier's
# staging area, and reads through smoothfs merge old and staged bytes.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"

UUID=00000000-0000-0000-0000-00000000f206
SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-write-staging-range}
export SPILL_ROOT SPILL_UUID=$UUID
trap spill_cleanup EXIT

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool writestagerange "$UUID"

SYSFS="/sys/fs/smoothfs/$UUID"
if [ ! -d "$SYSFS" ]; then
	echo "  FAIL  missing smoothfs sysfs pool $SYSFS"
	exit 1
fi
echo 1 > "$SYSFS/write_staging_enabled"
echo 98 > "$SYSFS/write_staging_full_pct"

printf 'abcdefghij\n' > "$SPILL_ROOT/slow/range.txt"
spill_assert test ! -e "$SPILL_ROOT/fast/range.txt"

echo "=== overlay non-truncating write through smoothfs ==="
printf 'XYZ' | dd of="$SPILL_ROOT/server/range.txt" bs=1 seek=3 conv=notrunc status=none

spill_assert test ! -e "$SPILL_ROOT/fast/range.txt"
spill_assert grep -qx 'abcdefghij' "$SPILL_ROOT/slow/range.txt"
spill_assert grep -qx 'abcXYZghij' "$SPILL_ROOT/server/range.txt"

range_bytes=$(cat "$SYSFS/range_staged_bytes")
range_writes=$(cat "$SYSFS/range_staged_writes")
staged_bytes=$(cat "$SYSFS/staged_bytes")
reason=$(cat "$SYSFS/last_drain_reason")

spill_assert test "$range_bytes" = "3"
spill_assert test "$range_writes" = "1"
spill_assert test "$staged_bytes" = "3"
spill_assert test "$reason" = "range-staged-write"

echo "=== direct I/O refuses to bypass staged ranges ==="
if dd if="$SPILL_ROOT/server/range.txt" of=/dev/null bs=4096 iflag=direct count=1 status=none 2>/dev/null; then
	echo "  FAIL  direct read unexpectedly bypassed staged range"
	spill_rc=1
else
	echo "  ok    direct read refused while range-staged"
fi

spill_finish "write_staging_range"

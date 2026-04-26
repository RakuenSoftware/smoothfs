#!/bin/bash
# Metadata-tier activity gate smoke test.
#
# A tier that SmoothNAS marks inactive must not be touched for metadata-only
# fallback lookup or union readdir. The fastest tier remains visible.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"

UUID=00000000-0000-0000-0000-00000000f206
SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-metadata-gate}
export SPILL_ROOT SPILL_UUID=$UUID
trap spill_cleanup EXIT

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool metagate "$UUID"

SYSFS="/sys/fs/smoothfs/$UUID"
if [ ! -d "$SYSFS" ]; then
	echo "  FAIL  missing smoothfs sysfs pool $SYSFS"
	exit 1
fi

printf 'fast\n' > "$SPILL_ROOT/fast/fast.txt"
printf 'slow\n' > "$SPILL_ROOT/slow/slow.txt"

echo "=== default active mask sees both tiers ==="
if ls "$SPILL_ROOT/server" | grep -qx 'slow.txt'; then
	echo "  ok    slow-tier file visible while tier is active"
else
	echo "  FAIL  slow-tier file missing before gate"
	spill_rc=1
fi

before=$(cat "$SYSFS/metadata_tier_skips")
echo 0x1 > "$SYSFS/metadata_active_tier_mask"

echo "=== inactive slow tier is skipped for metadata-only browse ==="
spill_assert grep -qx 'fast' "$SPILL_ROOT/server/fast.txt"
if ls "$SPILL_ROOT/server" | grep -qx 'slow.txt'; then
	echo "  FAIL  slow-tier file visible while tier is inactive"
	spill_rc=1
else
	echo "  ok    slow-tier file hidden while tier is inactive"
fi
if cat "$SPILL_ROOT/server/slow.txt" >/dev/null 2>&1; then
	echo "  FAIL  fallback lookup touched inactive slow tier"
	spill_rc=1
else
	echo "  ok    fallback lookup skipped inactive slow tier"
fi

after=$(cat "$SYSFS/metadata_tier_skips")
if [ "$after" -le "$before" ]; then
	echo "  FAIL  metadata_tier_skips did not increase"
	spill_rc=1
else
	echo "  ok    metadata_tier_skips increased to $after"
fi

spill_finish "metadata_tier_activity_gate"

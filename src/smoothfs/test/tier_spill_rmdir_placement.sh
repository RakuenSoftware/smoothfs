#!/bin/bash
# Regression: rmdir/unlink must drop the placement identity and purge spill
# tiers, so a removed directory does not survive as a zombie or a stale
# spill-tier copy.
#
# A spilled child file forces smoothfs to replicate its parent chain onto the
# slow tier (same shape as a plugin's tier-bound volume dir landing on a
# non-canonical tier). Before the fix, `rm -rf` of that subtree:
#   * left the parent dir on the slow tier (smoothfs_rmdir only removed the
#     canonical tier), and
#   * left si->rel_path + the replay pin set on the cached inode,
# so the path resurrected on the next lookup (dual-resolution), and recreating
# the exact same path failed with ENOENT under a zombie (nlink 0) parent.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/tier_spill_lib.sh"

trap spill_cleanup EXIT

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool rmdirplc 00000000-0000-0000-0000-00000000f108

echo "=== filling fast tier to spill threshold ==="
spill_fill_fast_tier

# Build /plug/state with a payload that spills to the slow tier, replicating
# the /plug and /plug/state parent dirs onto the slow tier.
mkdir -p "$SPILL_ROOT/server/plug/state"
SRC=/tmp/tier-spill-rmdir.src
spill_make_payload "$SRC" 8
cp "$SRC" "$SPILL_ROOT/server/plug/state/data.bin"

echo "=== precondition: parent chain replicated onto the slow tier ==="
spill_assert test -d "$SPILL_ROOT/slow/plug"
spill_assert test -d "$SPILL_ROOT/slow/plug/state"

echo "=== rm -rf the subtree through smoothfs ==="
rm -rf "$SPILL_ROOT/server/plug"

echo "=== removed path must be fully gone (no zombie resurrection) ==="
spill_assert test ! -e "$SPILL_ROOT/server/plug"
# stat() of the removed path must miss, not resurrect the cached inode.
spill_assert bash -c "! stat '$SPILL_ROOT/server/plug' >/dev/null 2>&1"

echo "=== spill-tier copies must be purged, not orphaned ==="
spill_assert test ! -e "$SPILL_ROOT/slow/plug"
spill_assert test ! -e "$SPILL_ROOT/fast/plug"

echo "=== the exact same path must be recreatable (the reinstall case) ==="
spill_assert mkdir -p "$SPILL_ROOT/server/plug/state"
spill_assert cp "$SRC" "$SPILL_ROOT/server/plug/state/data2.bin"
spill_assert test "$(spill_sha "$SRC")" = "$(spill_sha "$SPILL_ROOT/server/plug/state/data2.bin")"

rm -f "$SRC"
spill_finish "tier_spill_rmdir_placement"

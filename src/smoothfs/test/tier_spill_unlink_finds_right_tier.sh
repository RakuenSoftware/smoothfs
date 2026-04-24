#!/bin/bash
# Unlink of a spilled file should remove the backing object from the
# colder tier rather than looking only at the canonical parent tier.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/tier_spill_lib.sh"

trap spill_cleanup EXIT

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool unlinkspill 00000000-0000-0000-0000-00000000f104

echo "=== fill fast tier, then create spilled file ==="
spill_fill_fast_tier
echo payload > "$SPILL_ROOT/server/unlink-me.txt"
spill_assert test -f "$SPILL_ROOT/slow/unlink-me.txt"

echo "=== unlink through smoothfs ==="
rm -f "$SPILL_ROOT/server/unlink-me.txt"

spill_assert test ! -e "$SPILL_ROOT/server/unlink-me.txt"
spill_assert test ! -e "$SPILL_ROOT/slow/unlink-me.txt"
spill_assert test ! -e "$SPILL_ROOT/fast/unlink-me.txt"

spill_finish "tier_spill_unlink_finds_right_tier"

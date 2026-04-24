#!/bin/bash
# Tier-spill basic create: fill tier 0 near full, then create a file
# through smoothfs and verify it lands on tier 1 and is readable by name.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/tier_spill_lib.sh"

trap spill_cleanup EXIT

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool basic 00000000-0000-0000-0000-00000000f101

echo "=== filling fast tier to spill threshold ==="
spill_fill_fast_tier

SRC=/tmp/tier-spill-basic.src
spill_make_payload "$SRC" 16

echo "=== create through smoothfs; expect spill to tier 1 ==="
cp "$SRC" "$SPILL_ROOT/server/spilled.bin"

spill_assert test -f "$SPILL_ROOT/slow/spilled.bin"
spill_assert test ! -f "$SPILL_ROOT/fast/spilled.bin"
spill_assert test "$(spill_sha "$SRC")" = "$(spill_sha "$SPILL_ROOT/server/spilled.bin")"
spill_assert test "$(spill_sha "$SRC")" = "$(spill_sha "$SPILL_ROOT/slow/spilled.bin")"

rm -f "$SRC"
spill_finish "tier_spill_basic_create"

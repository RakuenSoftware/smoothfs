#!/bin/bash
# Tier-spill nested parent materialization: spilled create under
# /a/b/c must mkdir-p the parent chain on the colder tier.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/tier_spill_lib.sh"

trap spill_cleanup EXIT

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool nested 00000000-0000-0000-0000-00000000f102

echo "=== filling fast tier to spill threshold ==="
spill_fill_fast_tier

mkdir -p "$SPILL_ROOT/server/a/b/c"
SRC=/tmp/tier-spill-nested.src
spill_make_payload "$SRC" 8

echo "=== nested create through smoothfs; expect parent chain on tier 1 ==="
cp "$SRC" "$SPILL_ROOT/server/a/b/c/d.bin"

spill_assert test -d "$SPILL_ROOT/slow/a"
spill_assert test -d "$SPILL_ROOT/slow/a/b"
spill_assert test -d "$SPILL_ROOT/slow/a/b/c"
spill_assert test -f "$SPILL_ROOT/slow/a/b/c/d.bin"
spill_assert test "$(spill_sha "$SRC")" = "$(spill_sha "$SPILL_ROOT/server/a/b/c/d.bin")"

rm -f "$SRC"
spill_finish "tier_spill_nested_parent"

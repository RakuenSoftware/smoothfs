#!/bin/bash
# Cross-directory cross-tier rename, exhaustive cases. The fix completes these
# on the source's own tier with no data copy (mirrors the same-directory path),
# so an in-share "move" never leaks EXDEV to an SMB/NFS client.
#
# Covers:
#   1. file move into a sibling dir whose backing is on the other tier,
#      with byte-exact payload integrity,
#   2. file move into a NEW nested destination chain (parent materialized on
#      the source tier),
#   3. directory move across directories and tiers (spill_is_dir path).

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/tier_spill_lib.sh"

trap spill_cleanup EXIT

echo "=== laying down 2-tier smoothfs ==="
spill_setup_pool renamecrossdir 00000000-0000-0000-0000-00000000f104

echo "=== destination dirs on canonical (fast) tier 0 ==="
mkdir -p "$SPILL_ROOT/server/archive"
spill_assert test -d "$SPILL_ROOT/fast/archive"

echo "=== fill fast tier so fresh writes spill to tier 1 (slow) ==="
spill_fill_fast_tier

echo "=== case 1: spilled file -> dir on the other tier, payload intact ==="
spill_make_payload "$SPILL_ROOT/server/incoming.bin" 16
spill_assert test -f "$SPILL_ROOT/slow/incoming.bin"
want=$(spill_sha "$SPILL_ROOT/server/incoming.bin")
python3 - <<'PY' "$SPILL_ROOT/server/incoming.bin" "$SPILL_ROOT/server/archive/keep.bin"
import os, sys
os.rename(sys.argv[1], sys.argv[2])
PY
spill_rc=$(( spill_rc + $? ))
spill_assert test ! -e "$SPILL_ROOT/server/incoming.bin"
spill_assert test -f "$SPILL_ROOT/server/archive/keep.bin"
spill_assert test -f "$SPILL_ROOT/slow/archive/keep.bin"      # stayed on slow
spill_assert test ! -f "$SPILL_ROOT/fast/archive/keep.bin"    # no copy to fast
got=$(spill_sha "$SPILL_ROOT/server/archive/keep.bin")
spill_assert test "$got" = "$want"

echo "=== case 2: spilled file -> brand-new nested destination chain ==="
echo nested > "$SPILL_ROOT/server/loose.txt"
spill_assert test -f "$SPILL_ROOT/slow/loose.txt"
python3 - <<'PY' "$SPILL_ROOT/server/loose.txt" "$SPILL_ROOT/server/a/b/c/deep.txt"
import os, sys
dst = sys.argv[2]
os.makedirs(os.path.dirname(dst), exist_ok=True)
os.rename(sys.argv[1], dst)
PY
spill_rc=$(( spill_rc + $? ))
spill_assert test ! -e "$SPILL_ROOT/server/loose.txt"
spill_assert test -f "$SPILL_ROOT/server/a/b/c/deep.txt"
spill_assert test "$(cat "$SPILL_ROOT/server/a/b/c/deep.txt")" = nested

echo "=== case 3: directory move across dirs and tiers ==="
mkdir "$SPILL_ROOT/server/album"          # spills to slow (fast is full)
spill_assert test -d "$SPILL_ROOT/slow/album"
echo track > "$SPILL_ROOT/server/album/01.flac"
python3 - <<'PY' "$SPILL_ROOT/server/album" "$SPILL_ROOT/server/archive/album"
import os, sys
os.rename(sys.argv[1], sys.argv[2])
PY
spill_rc=$(( spill_rc + $? ))
spill_assert test ! -e "$SPILL_ROOT/server/album"
spill_assert test -d "$SPILL_ROOT/server/archive/album"
spill_assert test -f "$SPILL_ROOT/server/archive/album/01.flac"
spill_assert test "$(cat "$SPILL_ROOT/server/archive/album/01.flac")" = track

spill_finish "tier_spill_rename_cross_dir"

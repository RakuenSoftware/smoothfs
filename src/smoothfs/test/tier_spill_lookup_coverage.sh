#!/bin/bash
# Invariant: lookup tier-coverage must be a superset of readdir's, so a name
# that `ls` lists is always resolvable (open/stat/rename) -- never a "phantom".
#
# smoothfs_lookup's across-tiers fallback now scans EVERY tier (parent_tier
# included) via fresh rel_path resolution, matching readdir's union. This guard
# exercises cross-tier directories under churn and asserts no listed entry is
# unresolvable, and that the parent_tier_lookup_recoveries counter is exposed.

set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/tier_spill_lib.sh"
trap spill_cleanup EXIT

echo "=== 2-tier smoothfs ==="
spill_setup_pool lookupcov 00000000-0000-0000-0000-00000000f106

echo "=== a dir whose files span BOTH tiers ==="
mkdir "$SPILL_ROOT/server/d"
for i in 1 2 3 4; do echo "fast-$i" > "$SPILL_ROOT/server/d/f0$i"; done   # fast tier
spill_fill_fast_tier
for i in 5 6 7 8; do echo "slow-$i" > "$SPILL_ROOT/server/d/f0$i"; done   # spill -> slow tier
spill_assert test -f "$SPILL_ROOT/slow/d/f05"
mkdir "$SPILL_ROOT/server/e"

echo "=== move the fast-tier files out cross-dir cross-tier, leaving a mixed dir ==="
python3 - <<'PY' "$SPILL_ROOT/server/d" "$SPILL_ROOT/server/e"
import os, sys
d, e = sys.argv[1], sys.argv[2]
for i in (1, 2, 3, 4):
    os.rename(f"{d}/f0{i}", f"{e}/f0{i}")
PY
spill_rc=$(( spill_rc + $? ))

echo "=== INVARIANT: every entry readdir lists in d/ and e/ MUST be openable ==="
phantom=""
for dir in d e; do
  for name in $(ls -1 "$SPILL_ROOT/server/$dir" 2>/dev/null); do
    cat "$SPILL_ROOT/server/$dir/$name" >/dev/null 2>&1 || phantom="$phantom $dir/$name"
  done
done
echo "  phantom (listed-but-unopenable):${phantom:- none}"
spill_assert test -z "$phantom"
# remaining slow-tier siblings resolve with correct content
for i in 5 6 7 8; do
  spill_assert test "$(cat "$SPILL_ROOT/server/d/f0$i" 2>/dev/null)" = "slow-$i"
done
# moved files resolve in their new home
for i in 1 2 3 4; do
  spill_assert test "$(cat "$SPILL_ROOT/server/e/f0$i" 2>/dev/null)" = "fast-$i"
done

echo "=== the parent_tier_lookup_recoveries counter is exposed in /sys ==="
uuid=$(ls /sys/fs/smoothfs/ 2>/dev/null | head -1)
spill_assert test -r "/sys/fs/smoothfs/$uuid/parent_tier_lookup_recoveries"
echo "  parent_tier_lookup_recoveries = $(cat "/sys/fs/smoothfs/$uuid/parent_tier_lookup_recoveries" 2>/dev/null)"

spill_finish "tier_spill_lookup_coverage"

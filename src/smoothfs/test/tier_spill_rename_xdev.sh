#!/bin/bash
# Cross-tier rename should fail with EXDEV: spilled file on tier 1,
# destination parent on canonical tier 0.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/tier_spill_lib.sh"

trap spill_cleanup EXIT

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool renamexdev 00000000-0000-0000-0000-00000000f103

echo "=== create a destination dir on canonical tier 0 ==="
mkdir -p "$SPILL_ROOT/server/fastdir"
echo fast > "$SPILL_ROOT/server/fastdir/anchor.txt"
spill_assert test -f "$SPILL_ROOT/fast/fastdir/anchor.txt"

echo "=== fill fast tier, then create spilled source on tier 1 ==="
spill_fill_fast_tier
echo spill > "$SPILL_ROOT/server/spilled.txt"
spill_assert test -f "$SPILL_ROOT/slow/spilled.txt"

echo "=== raw rename syscall should fail with EXDEV ==="
python3 - <<'PY' "$SPILL_ROOT/server/spilled.txt" "$SPILL_ROOT/server/fastdir/moved.txt"
import errno, os, sys
src, dst = sys.argv[1], sys.argv[2]
try:
    os.rename(src, dst)
except OSError as e:
    if e.errno == errno.EXDEV:
        print("  ok    rename rejected with EXDEV")
        sys.exit(0)
    print(f"  FAIL  rename returned unexpected errno {e.errno}: {e}", file=sys.stderr)
    sys.exit(1)
print("  FAIL  cross-tier rename unexpectedly succeeded", file=sys.stderr)
sys.exit(1)
PY
spill_rc=$(( spill_rc + $? ))

spill_assert test -f "$SPILL_ROOT/server/spilled.txt"
spill_assert test ! -f "$SPILL_ROOT/server/fastdir/moved.txt"

spill_finish "tier_spill_rename_xdev"

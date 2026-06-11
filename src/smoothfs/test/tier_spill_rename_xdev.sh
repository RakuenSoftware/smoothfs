#!/bin/bash
# Cross-directory cross-tier rename must COMPLETE on the source's own tier,
# never EXDEV: a spilled file on tier 1 moved into a directory whose canonical
# backing is on tier 0 lands on tier 1 under that directory and stays visible.
# (SMB/NFS servers and raw rename(2) callers do not fall back to copy+delete,
# so leaking EXDEV here breaks an in-share move.)

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
mkdir "$SPILL_ROOT/server/.smb-tmp-dir"
spill_assert test -d "$SPILL_ROOT/slow/.smb-tmp-dir"

echo "=== same-parent rename of spilled directory should stay on tier 1 ==="
python3 - <<'PY' "$SPILL_ROOT/server/.smb-tmp-dir" "$SPILL_ROOT/server/smb-final-dir"
import os, sys
os.rename(sys.argv[1], sys.argv[2])
PY
spill_rc=$(( spill_rc + $? ))
spill_assert test ! -e "$SPILL_ROOT/server/.smb-tmp-dir"
spill_assert test -d "$SPILL_ROOT/server/smb-final-dir"
spill_assert test -d "$SPILL_ROOT/slow/smb-final-dir"
rmdir "$SPILL_ROOT/server/smb-final-dir"

echo "=== create spilled source file on tier 1 ==="
echo spill > "$SPILL_ROOT/server/spilled.txt"
spill_assert test -f "$SPILL_ROOT/slow/spilled.txt"

echo "=== cross-directory cross-tier rename must complete (no EXDEV) ==="
python3 - <<'PY' "$SPILL_ROOT/server/spilled.txt" "$SPILL_ROOT/server/fastdir/moved.txt"
import os, sys
src, dst = sys.argv[1], sys.argv[2]
try:
    os.rename(src, dst)
except OSError as e:
    print(f"  FAIL  cross-dir cross-tier rename failed: errno {e.errno}: {e}",
          file=sys.stderr)
    sys.exit(1)
print("  ok    rename completed")
sys.exit(0)
PY
spill_rc=$(( spill_rc + $? ))

# Source name is gone; the moved name is visible under the destination dir.
spill_assert test ! -e "$SPILL_ROOT/server/spilled.txt"
spill_assert test -f "$SPILL_ROOT/server/fastdir/moved.txt"
# No byte copied: the file stays on its source tier (slow), now under fastdir;
# it did NOT materialize on the fast tier where fastdir is canonical.
spill_assert test -f "$SPILL_ROOT/slow/fastdir/moved.txt"
spill_assert test ! -f "$SPILL_ROOT/fast/fastdir/moved.txt"
# Content preserved, and the pre-existing fast-tier sibling is untouched.
spill_assert test "$(cat "$SPILL_ROOT/server/fastdir/moved.txt")" = spill
spill_assert test "$(cat "$SPILL_ROOT/server/fastdir/anchor.txt")" = fast

spill_finish "tier_spill_rename_xdev"

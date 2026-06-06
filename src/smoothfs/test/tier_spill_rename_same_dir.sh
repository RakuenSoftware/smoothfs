#!/bin/bash
# Same-directory rename across tiers must SUCCEED (no EXDEV), so that
# atomic tmp+rename writers (Steam appmanifests, SQLite, dpkg, git,
# editors) keep working after the fast tier fills and forces a fresh tmp
# onto a slower tier than the file it replaces. The renamed file lands on
# the source's tier and the stale target copy is dropped from its old tier.
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/tier_spill_lib.sh"
trap spill_cleanup EXIT

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool renamesamedir 00000000-0000-0000-0000-00000000f104

mkdir -p "$SPILL_ROOT/server/lib"

# Case 1: target on the fast (canonical) tier, tmp spills to slow.
echo "=== create target on fast tier, then fill fast so tmp spills ==="
printf 'old-content\n' > "$SPILL_ROOT/server/lib/manifest"
spill_assert test -f "$SPILL_ROOT/fast/lib/manifest"
spill_fill_fast_tier
printf 'new-content-A\n' > "$SPILL_ROOT/server/lib/manifest.tmp"
spill_assert test -f "$SPILL_ROOT/slow/lib/manifest.tmp"

echo "=== same-dir cross-tier rename (slow tmp -> fast target) must succeed ==="
python3 - "$SPILL_ROOT/server/lib/manifest.tmp" "$SPILL_ROOT/server/lib/manifest" <<'PY'
import errno, os, sys
try:
    os.rename(sys.argv[1], sys.argv[2])
    print("  ok    rename succeeded")
except OSError as e:
    name = errno.errorcode.get(e.errno, e.errno)
    print(f"  FAIL  rename failed with {name}: {e}", file=sys.stderr)
    sys.exit(1)
PY
spill_rc=$(( spill_rc + $? ))

spill_assert test "$(cat "$SPILL_ROOT/server/lib/manifest")" = "new-content-A"
spill_assert test ! -e "$SPILL_ROOT/server/lib/manifest.tmp"
# Stale copy on the canonical (fast) tier must be gone, else it shadows the
# renamed file; the renamed file lives on the source's (slow) tier.
spill_assert test ! -e "$SPILL_ROOT/fast/lib/manifest"
spill_assert test -f "$SPILL_ROOT/slow/lib/manifest"

# Case 2: repeat the atomic write — now both target and tmp are on slow
# (same spill tier). The same-directory rename must still succeed.
echo "=== same-dir same-spill-tier rename (slow tmp -> slow target) ==="
printf 'new-content-B\n' > "$SPILL_ROOT/server/lib/manifest.tmp"
spill_assert test -f "$SPILL_ROOT/slow/lib/manifest.tmp"
python3 - "$SPILL_ROOT/server/lib/manifest.tmp" "$SPILL_ROOT/server/lib/manifest" <<'PY'
import errno, os, sys
try:
    os.rename(sys.argv[1], sys.argv[2])
    print("  ok    rename succeeded")
except OSError as e:
    name = errno.errorcode.get(e.errno, e.errno)
    print(f"  FAIL  rename failed with {name}: {e}", file=sys.stderr)
    sys.exit(1)
PY
spill_rc=$(( spill_rc + $? ))
spill_assert test "$(cat "$SPILL_ROOT/server/lib/manifest")" = "new-content-B"
spill_assert test ! -e "$SPILL_ROOT/server/lib/manifest.tmp"

# The old path must not resolve any more (no dual-resolution); a fresh
# listing shows exactly one manifest.
spill_assert test "$(ls "$SPILL_ROOT/server/lib" | grep -c '^manifest$')" = "1"

spill_finish "tier_spill_rename_same_dir"

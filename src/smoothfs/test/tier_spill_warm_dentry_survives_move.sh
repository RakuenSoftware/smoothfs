#!/bin/bash
# Regression: a warm (cached) dentry must survive tierd moving its file to
# another tier out-of-band.
#
# tierd's placement mover relocates a file by copying it to another tier and
# then removing the source directly on the backing, bypassing the smoothfs VFS,
# and finally poking forget_lower on the old (tier, lower_ino). The smoothfs
# inode is OID-identified, so it is unchanged across the move — but any warm
# dentry looked up before the move still has smoothfs_inode_info.lower_path
# pointing at the now-unlinked old-tier lower. ->getattr / open then dereference
# that dead lower and return ENOENT, while readdir re-lists the name and
# d_revalidate never re-checks (XFS/ext4/... install no ->d_revalidate). A cold
# lookup would recover via smoothfs_lookup_rel_across_tiers, but nothing forces
# the warm dentry cold, so `stat`/`cat` fail until drop_caches evicts it.
#
# The fix teaches forget_lower (smoothfs_forget_tier_copy -> smoothfs_relower_
# after_forget) to re-resolve the object on a surviving tier and re-point the
# warm inode + dentry, mirroring smoothfs_movement_cutover. This test warms the
# dentry, emulates the out-of-band move, pokes forget_lower, and asserts the
# path resolves again with the SAME st_ino and NO drop_caches. It FAILS on an
# unpatched module (stat returns ENOENT).

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"

UUID=00000000-0000-0000-0000-00000000f10d
POOL=warmmove
SYSFS="/sys/fs/smoothfs/$UUID/forget_lower"
FAST_TIER=0
export SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-warm-dentry-move}
trap spill_cleanup EXIT

MERGED="$SPILL_ROOT/server/obj.bin"
FASTF="$SPILL_ROOT/fast/obj.bin"
SLOWF="$SPILL_ROOT/slow/obj.bin"

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool "$POOL" "$UUID"

# The fix's control surface must exist (fails loudly on an unpatched module).
spill_assert test -w "$SYSFS"
[ -w "$SYSFS" ] || spill_finish "tier_spill_warm_dentry_survives_move"

echo "=== create a 16MB object on the fast (canonical) tier ==="
SRC=/tmp/warm-dentry-move.src
spill_make_payload "$SRC" 16
cp "$SRC" "$MERGED"
sync
spill_assert test -f "$FASTF"
LINO=$(stat -c %i "$FASTF")
echo "  fast lower inode=$LINO"

# Wait for the async OID writeback to persist the identity xattr onto the
# backing — that is what the moved copy must carry to keep the same inode.
echo "=== wait for the backing OID xattr to persist ==="
python3 - "$FASTF" <<'PY'
import os, sys, time
deadline = time.time() + 15
while True:
    try:
        os.getxattr(sys.argv[1], b"trusted.smoothfs.oid"); break
    except OSError:
        if time.time() > deadline:
            sys.stderr.write("oid xattr never persisted\n"); sys.exit(1)
        time.sleep(0.2)
PY
[ $? -eq 0 ] || { spill_assert false "oid not persisted"; spill_finish "tier_spill_warm_dentry_survives_move"; }

echo "=== warm the dentry: stat + read through the merged view ==="
# This instantiates the smoothfs inode and a hashed dentry whose lower_path
# points at the fast tier — the warm state the bug strands.
spill_assert test -f "$MERGED"
ST_INO_BEFORE=$(stat -c %i "$MERGED")
SHA_BEFORE=$(spill_sha "$MERGED")
echo "  smoothfs st_ino=$ST_INO_BEFORE"

echo "=== emulate tierd's out-of-band move: copy fast->slow, then unlink fast ==="
# Copy the bytes, then propagate every trusted.smoothfs.* xattr (identity/gen)
# exactly as tierd's mover does, so the slow copy is the same object.
cp "$FASTF" "$SLOWF"
python3 - "$FASTF" "$SLOWF" <<'PY'
import os, sys
src, dst = sys.argv[1], sys.argv[2]
for name in os.listxattr(src, follow_symlinks=False):
    if name.startswith("trusted.smoothfs."):
        os.setxattr(dst, name,
                    os.getxattr(src, name, follow_symlinks=False),
                    follow_symlinks=False)
PY
sync
rm -f "$FASTF"
sync
# NB: deliberately NO drop_caches — the warm dentry/inode stay resident, which
# is the whole point of the regression.

echo "=== forget_lower must re-point the warm dentry, not strand it ==="
echo "$FAST_TIER $LINO" > "$SYSFS"
sync
sleep 1

echo "=== assert the path resolves again, same identity, no drop_caches ==="
# On an unpatched module these fail: getattr/open dereference the unlinked fast
# lower and return ENOENT.
spill_assert test -f "$MERGED"
spill_assert cat "$MERGED" >/dev/null
ST_INO_AFTER=$(stat -c %i "$MERGED" 2>/dev/null || echo "MISSING")
spill_assert test "$ST_INO_AFTER" = "$ST_INO_BEFORE"
spill_assert test "$(spill_sha "$MERGED" 2>/dev/null)" = "$SHA_BEFORE"
# readdir parity should hold both before and after the fix.
spill_assert bash -c "ls '$SPILL_ROOT/server' | grep -qx obj.bin"

rm -f "$SRC"
spill_finish "tier_spill_warm_dentry_survives_move"

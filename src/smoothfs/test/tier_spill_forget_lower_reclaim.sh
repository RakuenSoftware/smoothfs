#!/bin/bash
# Regression: the forget_lower sysfs control must reclaim backing space for a
# lower copy that was removed out-of-band (bypassing the smoothfs VFS).
#
# tierd's placement mover relocates a file by copying it to the destination tier
# and then os.Remove()-ing the source directly on the backing directory — it
# never goes through smoothfs. smoothfs still holds each lower inode via
# smoothfs_inode_info.lower_path, and placement/spill/replay inodes are
# additionally replay-pinned (an extra iget ref). So the source inode's VFS
# refcount never hits zero, smoothfs_evict_inode never runs, and the backing fs
# keeps the freed blocks allocated (df >> du) until unmount. tierd's old
# drop_caches=2 workaround cannot reclaim these: drop_caches only evicts
# cache-idle inodes, and the replay pin keeps a reference.
#
# The fix: tierd writes "<tier> <lower_ino>" to the pool's forget_lower sysfs
# after each out-of-band removal; smoothfs drops that inode's pin and evicts it,
# freeing the blocks immediately. This harness reproduces the leak (drop_caches
# does NOT reclaim) and then asserts forget_lower does.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/tier_spill_lib.sh"

trap spill_cleanup EXIT

POOL_UUID=00000000-0000-0000-0000-00000000f10a
SYSFS="/sys/fs/smoothfs/$POOL_UUID/forget_lower"
SLOW_TIER=1   # tiers=fast:slow -> fast=0, slow=1

back_used_inodes() { df -P -i "$1" | awk 'NR==2 {print $3}'; }

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool forgetlwr "$POOL_UUID"

# The fix's control surface must exist (fails loudly on an unpatched module).
spill_assert test -w "$SYSFS"
[ -w "$SYSFS" ] || spill_finish "tier_spill_forget_lower_reclaim"

echo "=== filling fast tier to spill threshold ==="
spill_fill_fast_tier

SLOW="$SPILL_ROOT/slow"
base_ino=$(back_used_inodes "$SLOW")
echo "  slow-tier baseline: ${base_ino} inodes"

# Spill a payload under a fresh parent chain: smoothfs replicates /leak/a/b onto
# the slow tier (each replica dir is a distinct replay-pinned shadow inode) and
# spills the file there too.
echo "=== create a spilling subtree (replicates + pins parent dirs on slow) ==="
mkdir -p "$SPILL_ROOT/server/leak/a/b"
SRC=/tmp/tier-spill-forget.src
spill_make_payload "$SRC" 24
cp "$SRC" "$SPILL_ROOT/server/leak/a/b/data.bin"
sync
spill_assert test -d "$SLOW/leak/a/b"

# Capture every backing inode under the spilled subtree, then remove them
# OUT-OF-BAND on the slow backing (exactly what tierd's os.Remove does).
mapfile -t INOS < <(find "$SLOW/leak" -printf '%i\n' | sort -u)
echo "  captured ${#INOS[@]} slow-backing inodes under the subtree"
rm -rf "$SLOW/leak"
sync

echo "=== drop_caches must NOT reclaim (pinned inodes defeat it: the bug) ==="
echo 2 > /proc/sys/vm/drop_caches
sleep 1
leaked_ino=$(back_used_inodes "$SLOW")
echo "  after out-of-band rm + drop_caches: ${leaked_ino} inodes (baseline ${base_ino})"
spill_assert test "$leaked_ino" -gt "$base_ino"

echo "=== forget_lower must reclaim every removed inode ==="
# Safe to forget all of them: forget_lower is a no-op for inodes that are not
# pinned / already evicted (ilookup misses).
for ino in "${INOS[@]}"; do
	echo "$SLOW_TIER $ino" > "$SYSFS" 2>/dev/null || spill_assert false "write forget_lower $ino"
done
sync
sleep 1
done_ino=$(back_used_inodes "$SLOW")
echo "  after forget_lower: ${done_ino} inodes"
spill_assert test "$done_ino" -le "$base_ino"

rm -f "$SRC"
spill_finish "tier_spill_forget_lower_reclaim"

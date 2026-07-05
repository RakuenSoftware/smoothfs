#!/bin/bash
# Regression: forget_lower must free the backing space of a lower copy that
# tierd removed out-of-band, while smoothfs still holds that inode via a replay
# pin.
#
# tierd's placement mover relocates a file by copying it to another tier and
# then os.Remove()-ing the source directly on the backing, bypassing the
# smoothfs VFS. smoothfs holds each lower inode via smoothfs_inode_info.lower_
# path, and placement/replay inodes carry an extra replay-pin iget ref, so the
# removed inode's refcount never reaches zero, ->evict_inode never runs, and the
# backing keeps the freed blocks allocated (df >> du) until unmount. drop_caches
# cannot reclaim it — a pinned inode is never cache-idle. forget_lower drops
# that inode's pin so the backing frees the blocks at once.
#
# Deterministic pinned orphan: create an object, let the mount's background
# path-index rebuild persist it, then reload the module and remount. The object
# is now recovery-only: a netlink Inspect resolves it via smoothfs_recovery_
# resolve_oid (from the persisted path index) which replay-pins the inode
# WITHOUT a merged-view lookup that would hand the pin off to a dentry. That is
# the reliable pin the leak needs.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"

UUID=00000000-0000-0000-0000-00000000f10c
POOL=forgetlower
SYSFS="/sys/fs/smoothfs/$UUID/forget_lower"
FAST_TIER=0
export SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-forget-lower}
trap spill_cleanup EXIT

back_used_kb() { df -P -k "$1" | awk 'NR==2 {print $3}'; }

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool "$POOL" "$UUID"

# The fix's control surface must exist (fails loudly on an unpatched module).
spill_assert test -w "$SYSFS"
[ -w "$SYSFS" ] || spill_finish "tier_spill_forget_lower_reclaim"

echo "=== create a 16MB object on the fast (canonical) tier ==="
SRC=/tmp/forget-lower.src
spill_make_payload "$SRC" 16
cp "$SRC" "$SPILL_ROOT/server/obj.bin"
sync
FASTF="$SPILL_ROOT/fast/obj.bin"
spill_assert test -f "$FASTF"
LINO=$(stat -c %i "$FASTF")
# Read the OID from the BACKING xattr (smoothfs serves it from memory), polling
# until the async oid writeback has persisted it — the value recovery indexes.
OID=$(python3 - "$FASTF" <<'PY'
import os, sys, time
deadline = time.time() + 15
while True:
    try:
        print(os.getxattr(sys.argv[1], b"trusted.smoothfs.oid").hex()); break
    except OSError:
        if time.time() > deadline:
            sys.stderr.write("oid xattr never persisted\n"); sys.exit(1)
        time.sleep(0.2)
PY
)
[ -n "$OID" ] || { spill_assert false "oid not persisted"; spill_finish "tier_spill_forget_lower_reclaim"; }
echo "  fast lower inode=$LINO"

echo "=== wait for the background path-index rebuild to persist ==="
# Recovery is fed by the persisted path index (.smoothfs/path.idx), rebuilt in
# the background ~5s after mount. Inspect can only resolve the OID after it lands.
for _ in $(seq 1 40); do
	dmesg 2>/dev/null | grep -q "path index background rebuild complete for pool '$POOL'" && break
	sleep 1
done
spill_assert test -s "$SPILL_ROOT/fast/.smoothfs/path.idx"

echo "=== unmount, reload module, remount (object now recovery-only) ==="
umount "$SPILL_ROOT/server"
rmmod smoothfs
modprobe smoothfs
mount -t smoothfs -o "pool=$POOL,uuid=$UUID,tiers=$SPILL_ROOT/fast:$SPILL_ROOT/slow" \
	none "$SPILL_ROOT/server"
# NB: no merged-view stat/read of obj.bin here — that would instantiate the
# inode and hand its replay pin off to a dentry, defeating the Inspect pin.

echo "=== Inspect pins the recovered inode via recovery (no merged lookup) ==="
HELPER="$SPILL_ROOT/forget_inspect_helper.go"
cat > "$HELPER" <<'GO'
package main

import (
	"encoding/hex"
	"fmt"
	"os"
	"time"

	smoothfs "github.com/RakuenSoftware/smoothfs"
	"github.com/google/uuid"
)

func fatal(format string, a ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", a...)
	os.Exit(1)
}

func main() {
	if len(os.Args) != 3 {
		fatal("usage: %s pool-uuid oid-hex", os.Args[0])
	}
	poolUUID, err := uuid.Parse(os.Args[1])
	if err != nil {
		fatal("parse uuid: %v", err)
	}
	raw, err := hex.DecodeString(os.Args[2])
	if err != nil {
		fatal("decode oid: %v", err)
	}
	if len(raw) != smoothfs.OIDLen {
		fatal("oid length %d, want %d", len(raw), smoothfs.OIDLen)
	}
	var oid [smoothfs.OIDLen]byte
	copy(oid[:], raw)

	client, err := smoothfs.Open()
	if err != nil {
		fatal("open smoothfs client: %v", err)
	}
	defer client.Close()

	// Retry Inspect until recovery resolves the OID; the resolving Inspect
	// replay-pins the inode. A miss returns an error and does not pin.
	deadline := time.Now().Add(20 * time.Second)
	for {
		ins, err := client.Inspect(poolUUID, oid)
		if err == nil && ins != nil {
			fmt.Printf("pinned tier=%d rel=%s\n", ins.CurrentTier, ins.RelPath)
			return
		}
		if time.Now().After(deadline) {
			fatal("inspect did not resolve within deadline: %v", err)
		}
		time.Sleep(100 * time.Millisecond)
	}
}
GO
if (cd "$REPO_ROOT" && go run "$HELPER" "$UUID" "$OID"); then
	echo "  ok    inspect pinned the recovered inode"
else
	spill_assert false "inspect helper failed to pin"
	spill_finish "tier_spill_forget_lower_reclaim"
fi

base_kb=$(back_used_kb "$SPILL_ROOT/fast")
echo "  fast backing used with object present: ${base_kb}KB"

echo "=== remove the backing out-of-band (as tierd's mover does) ==="
rm -f "$FASTF"
sync
echo 2 > /proc/sys/vm/drop_caches
sleep 1
held_kb=$(back_used_kb "$SPILL_ROOT/fast")
echo "  after out-of-band rm + drop_caches: ${held_kb}KB"
# Pinned: drop_caches cannot evict, so the 16MB is still allocated — the leak.
spill_assert test "$((base_kb - held_kb))" -lt 4096

echo "=== forget_lower must drop the pin and free the blocks ==="
echo "$FAST_TIER $LINO" > "$SYSFS"
sync
sleep 1
freed_kb=$(back_used_kb "$SPILL_ROOT/fast")
echo "  after forget_lower (no drop_caches): ${freed_kb}KB"
# DIAGNOSTIC: does a subsequent drop_caches free it? (tells us whether the fix
# merely makes the inode reclaimable vs actually evicts it)
echo 2 > /proc/sys/vm/drop_caches
sync
sleep 1
freed2_kb=$(back_used_kb "$SPILL_ROOT/fast")
echo "  after forget_lower + drop_caches: ${freed2_kb}KB"
spill_assert test "$((base_kb - freed_kb))" -ge 12288

rm -f "$SRC"
spill_finish "tier_spill_forget_lower_reclaim"

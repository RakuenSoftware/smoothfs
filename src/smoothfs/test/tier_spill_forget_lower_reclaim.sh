#!/bin/bash
# Regression: forget_lower must free the backing space of a lower copy that
# tierd removed out-of-band, while smoothfs still holds that inode via a replay
# pin.
#
# tierd's placement mover relocates a file by copying it to another tier and
# then os.Remove()-ing the source directly on the backing, bypassing the
# smoothfs VFS. smoothfs holds each lower inode via smoothfs_inode_info.lower_
# path; placement/spill/replay inodes are additionally replay-pinned (an extra
# iget ref). So the removed source inode's refcount never reaches zero,
# smoothfs_evict_inode never runs, and the backing fs keeps the freed blocks
# allocated (df >> du) until unmount. drop_caches cannot reclaim it — a pinned
# inode is never cache-idle. forget_lower is the precise reclaim: it drops that
# inode's pin so the backing frees the blocks at once.
#
# Deterministic pinned orphan: after a remount an object is recovery-only. A
# netlink Inspect resolves it via smoothfs_recovery_resolve_oid, which replay-
# pins the inode (placement.c) WITHOUT a merged-view lookup that would hand the
# pin off to a dentry alias. That is the reliable pin the leak needs; an earlier
# spill-and-rmdir approach depended on a materialize pin surviving, which is
# lookup/timing sensitive and did not reproduce under virtme.

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
# Read the OID from the BACKING xattr, NOT the merged view: smoothfs serves
# trusted.smoothfs.oid from the in-memory si->oid (xattr.c), which may not yet
# be persisted to the lower. The mount-time recovery scan reads the *backing*
# xattr and mints a fresh OID if it is absent, so we must Inspect the OID that
# is actually on disk. Poll until the async oid writeback has landed it.
OID=$(python3 - "$FASTF" <<'PY'
import os, sys, time
p = sys.argv[1]
deadline = time.time() + 15
while True:
    try:
        print(os.getxattr(p, b"trusted.smoothfs.oid").hex())
        break
    except OSError:
        if time.time() > deadline:
            sys.stderr.write("oid xattr never persisted to backing\n")
            sys.exit(1)
        time.sleep(0.2)
PY
)
if [ -z "$OID" ]; then
	spill_assert false "oid not persisted to backing"
	spill_finish "tier_spill_forget_lower_reclaim"
fi
echo "  fast lower inode=$LINO oid=${OID:0:12}..."

echo "=== unmount, reload module, remount (object now recovery-only) ==="
umount "$SPILL_ROOT/server"
rmmod smoothfs
modprobe smoothfs
mount -t smoothfs -o "pool=$POOL,uuid=$UUID,tiers=$SPILL_ROOT/fast:$SPILL_ROOT/slow" \
	none "$SPILL_ROOT/server"
# NB: do NOT stat/read obj.bin through $SPILL_ROOT/server here — a merged-view
# lookup would instantiate the inode and hand its replay pin off to the dentry,
# defeating the deterministic pin the Inspect below relies on.

echo "=== Inspect pins the recovered inode (no merged-view lookup) ==="
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

	// The mount-time recovery scan indexes the backing asynchronously. Retry
	// Inspect until it resolves — the resolving Inspect runs recovery_resolve_
	// oid, which replay-pins the inode. A miss returns an error and does not
	// pin, so retrying is harmless.
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
echo "  after forget_lower: ${freed_kb}KB"
spill_assert test "$((base_kb - freed_kb))" -ge 12288

rm -f "$SRC"
spill_finish "tier_spill_forget_lower_reclaim"

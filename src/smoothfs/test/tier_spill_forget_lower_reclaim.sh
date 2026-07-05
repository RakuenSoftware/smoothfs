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
# Deterministic pinned orphan: move the object between tiers via the netlink
# client (which writes a placement-log record), then reload the module and
# remount. The object is now recovery-only: a netlink Inspect resolves it via
# smoothfs_recovery_resolve_oid from the log-backed recovery index, which replay-
# pins the inode WITHOUT a merged-view lookup that would hand the pin off. That
# is the reliable pin the leak needs — recovery is fed by the placement log, so
# a normal (never-logged) file would not be Inspect-findable after remount.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"

UUID=00000000-0000-0000-0000-00000000f10c
POOL=forgetlower
SYSFS="/sys/fs/smoothfs/$UUID/forget_lower"
SLOW_TIER=1
SEQ=7101
export SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-forget-lower}
trap spill_cleanup EXIT

back_used_kb() { df -P -k "$1" | awk 'NR==2 {print $3}'; }

# Read the OID from a *backing* file (bypassing smoothfs, which serves
# trusted.smoothfs.oid from memory), polling until the async oid writeback has
# persisted it so the value matches what the mount scan / recovery index use.
read_backing_oid() {
	python3 - "$1" <<'PY'
import os, sys, time
p = sys.argv[1]
deadline = time.time() + 15
while True:
    try:
        print(os.getxattr(p, b"trusted.smoothfs.oid").hex()); break
    except OSError:
        if time.time() > deadline:
            sys.stderr.write("oid xattr never persisted to backing\n"); sys.exit(1)
        time.sleep(0.2)
PY
}

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool "$POOL" "$UUID"

# The fix's control surface must exist (fails loudly on an unpatched module).
spill_assert test -w "$SYSFS"
[ -w "$SYSFS" ] || spill_finish "tier_spill_forget_lower_reclaim"

echo "=== create a 16MB object on the fast (canonical) tier ==="
SRC=/tmp/forget-lower.src
spill_make_payload "$SRC" 16
mkdir -p "$SPILL_ROOT/server"
cp "$SRC" "$SPILL_ROOT/server/obj.bin"
sync
FASTF="$SPILL_ROOT/fast/obj.bin"
SLOWF="$SPILL_ROOT/slow/obj.bin"
spill_assert test -f "$FASTF"
OID=$(read_backing_oid "$FASTF")
[ -n "$OID" ] || { spill_assert false "oid not persisted"; spill_finish "tier_spill_forget_lower_reclaim"; }

HELPER="$SPILL_ROOT/forget_helper.go"
cat > "$HELPER" <<'GO'
package main

import (
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"time"

	smoothfs "github.com/RakuenSoftware/smoothfs"
	"github.com/google/uuid"
)

func fatal(format string, a ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", a...)
	os.Exit(1)
}

func parse(u, o string) (uuid.UUID, [smoothfs.OIDLen]byte) {
	pu, err := uuid.Parse(u)
	if err != nil {
		fatal("parse uuid: %v", err)
	}
	raw, err := hex.DecodeString(o)
	if err != nil {
		fatal("decode oid: %v", err)
	}
	if len(raw) != smoothfs.OIDLen {
		fatal("oid length %d, want %d", len(raw), smoothfs.OIDLen)
	}
	var oid [smoothfs.OIDLen]byte
	copy(oid[:], raw)
	return pu, oid
}

func copyFile(srcPath, dstPath string) error {
	src, err := os.Open(srcPath)
	if err != nil {
		return err
	}
	defer src.Close()
	info, err := src.Stat()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(dstPath), 0o755); err != nil {
		return err
	}
	dst, err := os.OpenFile(dstPath, os.O_CREATE|os.O_TRUNC|os.O_RDWR, info.Mode().Perm())
	if err != nil {
		return err
	}
	if _, err := io.Copy(dst, src); err != nil {
		_ = dst.Close()
		return err
	}
	if err := dst.Sync(); err != nil {
		_ = dst.Close()
		return err
	}
	return dst.Close()
}

func main() {
	if len(os.Args) < 2 {
		fatal("usage: %s move|pin ...", os.Args[0])
	}
	client, err := smoothfs.Open()
	if err != nil {
		fatal("open smoothfs client: %v", err)
	}
	defer client.Close()

	switch os.Args[1] {
	case "move": // move uuid oid destTier seq src dst
		if len(os.Args) != 8 {
			fatal("usage: %s move uuid oid destTier seq src dst", os.Args[0])
		}
		pu, oid := parse(os.Args[2], os.Args[3])
		destTier, err := strconv.ParseUint(os.Args[4], 10, 8)
		if err != nil {
			fatal("parse destTier: %v", err)
		}
		seq, err := strconv.ParseUint(os.Args[5], 10, 64)
		if err != nil {
			fatal("parse seq: %v", err)
		}
		ins, err := client.Inspect(pu, oid)
		if err != nil || ins == nil {
			fatal("inspect before move: %v", err)
		}
		if err := client.MovePlan(pu, oid, uint8(destTier), seq); err != nil {
			fatal("move plan: %v", err)
		}
		if err := copyFile(os.Args[6], os.Args[7]); err != nil {
			fatal("copy to dest: %v", err)
		}
		if ins.HasWriteSeq {
			err = client.MoveCutoverVerifyWriteSeq(pu, oid, seq, ins.WriteSeq)
		} else {
			err = client.MoveCutover(pu, oid, seq)
		}
		if err != nil {
			fatal("move cutover: %v", err)
		}
		after, err := client.Inspect(pu, oid)
		if err != nil || after == nil {
			fatal("inspect after move: %v", err)
		}
		fmt.Printf("moved to tier=%d\n", after.CurrentTier)

	case "pin": // pin uuid oid  — retry Inspect until recovery resolves + pins
		if len(os.Args) != 4 {
			fatal("usage: %s pin uuid oid", os.Args[0])
		}
		pu, oid := parse(os.Args[2], os.Args[3])
		deadline := time.Now().Add(20 * time.Second)
		for {
			ins, err := client.Inspect(pu, oid)
			if err == nil && ins != nil {
				fmt.Printf("pinned tier=%d rel=%s\n", ins.CurrentTier, ins.RelPath)
				return
			}
			if time.Now().After(deadline) {
				fatal("inspect did not resolve within deadline: %v", err)
			}
			time.Sleep(100 * time.Millisecond)
		}

	default:
		fatal("unknown subcommand %q", os.Args[1])
	}
}
GO

echo "=== move the object fast -> slow (writes a placement-log record) ==="
if (cd "$REPO_ROOT" && go run "$HELPER" move "$UUID" "$OID" "$SLOW_TIER" "$SEQ" "$FASTF" "$SLOWF"); then
	echo "  ok    move cutover completed"
else
	spill_assert false "move helper failed"
	spill_finish "tier_spill_forget_lower_reclaim"
fi
spill_assert test -f "$SLOWF"
# The SWITCHED placement record is written by a ~1s-interval writeback worker;
# sync only kicks it. Wait for it to land on .smoothfs/placement.log so the
# post-remount recovery index (which is log-backed) can resolve the OID.
sync
sleep 2
sync
echo "  placement.log: $(ls -l "$SPILL_ROOT/fast/.smoothfs/placement.log" 2>&1 | awk '{print $5, $9}')"
echo "  captured OID:  $OID"
python3 - "$SPILL_ROOT/fast/.smoothfs/placement.log" <<'PY'
import sys, struct
MAGIC = 0x534D46504C4F470A
data = open(sys.argv[1], "rb").read()
# scan for records: magic(8 le) + seq(8) + oid(16 at +16) + state(+32)
i = 0
seen = []
while i + 33 <= len(data):
    if struct.unpack_from("<Q", data, i)[0] == MAGIC:
        oid = data[i+16:i+32].hex(); state = data[i+32]
        seen.append((oid, state)); i += 48  # assume 48-byte rec; realign on next magic if wrong
    else:
        i += 1
for oid, state in seen:
    print(f"  log record: oid={oid} state={state}")
PY
# tierd would os.Remove the stale source; do the same so only the slow copy is live.
rm -f "$FASTF"
SLINO=$(stat -c %i "$SLOWF")
echo "  slow lower inode=$SLINO"

echo "=== unmount, reload module, remount (object now recovery-only) ==="
umount "$SPILL_ROOT/server"
rmmod smoothfs
modprobe smoothfs
mount -t smoothfs -o "pool=$POOL,uuid=$UUID,tiers=$SPILL_ROOT/fast:$SPILL_ROOT/slow" \
	none "$SPILL_ROOT/server"
# NB: no merged-view stat/read of obj.bin here — that would instantiate the
# inode and hand its replay pin off to a dentry, defeating the Inspect pin.

echo "=== Inspect pins the recovered inode via recovery (no merged lookup) ==="
if (cd "$REPO_ROOT" && go run "$HELPER" pin "$UUID" "$OID"); then
	echo "  ok    inspect pinned the recovered inode"
else
	spill_assert false "inspect helper failed to pin"
	spill_finish "tier_spill_forget_lower_reclaim"
fi

base_kb=$(back_used_kb "$SPILL_ROOT/slow")
echo "  slow backing used with object present: ${base_kb}KB"

echo "=== remove the backing out-of-band (as tierd's mover does) ==="
rm -f "$SLOWF"
sync
echo 2 > /proc/sys/vm/drop_caches
sleep 1
held_kb=$(back_used_kb "$SPILL_ROOT/slow")
echo "  after out-of-band rm + drop_caches: ${held_kb}KB"
# Pinned: drop_caches cannot evict, so the 16MB is still allocated — the leak.
spill_assert test "$((base_kb - held_kb))" -lt 4096

echo "=== forget_lower must drop the pin and free the blocks ==="
echo "$SLOW_TIER $SLINO" > "$SYSFS"
sync
sleep 1
freed_kb=$(back_used_kb "$SPILL_ROOT/slow")
echo "  after forget_lower: ${freed_kb}KB"
spill_assert test "$((base_kb - freed_kb))" -ge 12288

rm -f "$SRC"
spill_finish "tier_spill_forget_lower_reclaim"

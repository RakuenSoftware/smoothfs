#!/bin/bash
# stat(2) must keep working on a file while a placement cutover moves it.
#
# smoothfs_getattr used to snapshot si->lower_path with no lock and no
# reference, then call vfs_getattr_nosec on that snapshot. A cutover replaces
# lower_path and, after dropping inode_lock, dputs the old lower dentry and
# mntputs its vfsmount -- so a stat that sampled the path just before the swap
# ran against a lower that had been unlinked on the source tier (-ESTALE for a
# file that plainly exists) or, on the losing interleaving, against a dentry
# already freed underneath it.
#
# The regression is timing-dependent by nature: the window is the few
# microseconds between the snapshot and the vfs_getattr. This drives many
# concurrent statters across repeated cutovers to open it, and treats ANY
# ESTALE as failure -- one is a bug, not noise.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"

UUID=00000000-0000-0000-0000-00000000f30a
SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-movement-getattr}
export SPILL_ROOT SPILL_UUID=$UUID

STAT_PIDS=()
cleanup() {
	local pid
	for pid in "${STAT_PIDS[@]:-}"; do
		[ -n "$pid" ] && kill "$pid" 2>/dev/null
	done
	spill_cleanup
}
trap cleanup EXIT

oid_for_path() {
	python3 - <<'PY' "$1"
import os
import sys

print(os.getxattr(sys.argv[1], b"trusted.smoothfs.oid").hex())
PY
}

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool movementgetattr "$UUID"

MOVE_HELPER="$SPILL_ROOT/move_getattr_helper.go"
cat > "$MOVE_HELPER" <<'GO'
package main

import (
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"

	"github.com/google/uuid"
	smoothfs "github.com/RakuenSoftware/smoothfs"
)

func main() {
	if len(os.Args) != 7 {
		fmt.Fprintf(os.Stderr, "usage: %s pool-uuid oid-hex dest-tier seq src dst\n", os.Args[0])
		os.Exit(2)
	}
	poolUUID, err := uuid.Parse(os.Args[1])
	must("parse uuid", err)
	rawOID, err := hex.DecodeString(os.Args[2])
	must("decode oid", err)
	if len(rawOID) != smoothfs.OIDLen {
		fatal("oid length = %d, want %d", len(rawOID), smoothfs.OIDLen)
	}
	destTier, err := strconv.ParseUint(os.Args[3], 10, 8)
	must("parse dest tier", err)
	seq, err := strconv.ParseUint(os.Args[4], 10, 64)
	must("parse seq", err)

	var oid [smoothfs.OIDLen]byte
	copy(oid[:], rawOID)

	client, err := smoothfs.Open()
	must("open smoothfs client", err)
	defer client.Close()

	ins, err := client.Inspect(poolUUID, oid)
	must("inspect before move", err)
	if ins == nil {
		fatal("inspect before move returned nil")
	}
	must("move plan", client.MovePlan(poolUUID, oid, uint8(destTier), seq))
	must("copy destination", copyFile(os.Args[5], os.Args[6]))
	if ins.HasWriteSeq {
		must("move cutover", client.MoveCutoverVerifyWriteSeq(poolUUID, oid, seq, ins.WriteSeq))
	} else {
		must("move cutover", client.MoveCutover(poolUUID, oid, seq))
	}
}

func copyFile(src, dst string) error {
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()
	if _, err := io.Copy(out, in); err != nil {
		return err
	}
	return out.Sync()
}

func must(what string, err error) {
	if err != nil {
		fatal("%s: %v", what, err)
	}
}

func fatal(format string, args ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", args...)
	os.Exit(1)
}
GO

# A statter reports every errno it saw, so a failure names the syscall result
# rather than only that "something went wrong".
STAT_HELPER="$SPILL_ROOT/stat_loop.py"
cat > "$STAT_HELPER" <<'PY'
import collections
import errno
import os
import signal
import sys

target, result = sys.argv[1], sys.argv[2]
seen = collections.Counter()
calls = 0
running = True


def stop(_signum, _frame):
    global running
    running = False


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)

while running:
    try:
        os.stat(target)
        calls += 1
    except OSError as exc:
        seen[exc.errno] += 1
        calls += 1

with open(result, "w") as handle:
    handle.write(f"{calls}\n")
    for code, count in sorted(seen.items()):
        handle.write(f"{code} {errno.errorcode.get(code, '?')} {count}\n")
PY

REL=movement/getattr-target.bin
SERVER="$SPILL_ROOT/server/$REL"
SRC="$SPILL_ROOT/fast/$REL"
DST="$SPILL_ROOT/slow/$REL"

mkdir -p "$(dirname "$SERVER")"
spill_make_payload "$SERVER" 4
sync
OID=$(oid_for_path "$SERVER")

echo "=== hammering stat(2) across repeated cutovers ==="
STATTERS=8
for i in $(seq 1 "$STATTERS"); do
	python3 "$STAT_HELPER" "$SERVER" "$SPILL_ROOT/stat-$i.txt" &
	STAT_PIDS+=("$!")
done

# Alternate tiers so each round is a real cutover rather than a no-op.
seq_no=9100
for round in $(seq 1 6); do
	if [ $((round % 2)) -eq 1 ]; then
		dest=1; from="$SRC"; to="$DST"
	else
		dest=0; from="$DST"; to="$SRC"
	fi
	if (cd "$REPO_ROOT" && go run "$MOVE_HELPER" "$UUID" "$OID" "$dest" \
		"$seq_no" "$from" "$to" >/dev/null); then
		echo "  ok    cutover round $round to tier $dest"
	else
		echo "  FAIL  cutover round $round to tier $dest"
		spill_rc=1
	fi
	seq_no=$((seq_no + 1))
	sleep 0.2
done

for pid in "${STAT_PIDS[@]}"; do
	kill -TERM "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
done
STAT_PIDS=()

total_calls=0
stale=0
other=0
for i in $(seq 1 "$STATTERS"); do
	file="$SPILL_ROOT/stat-$i.txt"
	[ -s "$file" ] || { echo "  FAIL  statter $i produced no result"; spill_rc=1; continue; }
	total_calls=$((total_calls + $(head -1 "$file")))
	while read -r code name count; do
		[ -z "${code:-}" ] && continue
		if [ "$name" = "ESTALE" ]; then
			stale=$((stale + count))
		else
			other=$((other + count))
			echo "  note  statter $i saw $count x $name"
		fi
	done < <(tail -n +2 "$file")
done

echo "  info  $total_calls stat calls across $STATTERS statters and 6 cutovers"
spill_assert test "$stale" -eq 0
spill_assert test "$other" -eq 0
if [ "$stale" -ne 0 ]; then
	echo "  FAIL  $stale ESTALE results: getattr raced a cutover on an unreferenced lower path"
fi

spill_finish "movement_getattr_cutover"

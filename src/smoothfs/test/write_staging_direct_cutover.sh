#!/bin/bash
# A refused O_DIRECT write after range staging must not wedge cutover.
#
# Regression target: smoothfs_write_iter used to return -EBUSY for direct
# writes after acquiring the cutover SRCU read lock, without releasing it.
# A later MOVE_CUTOVER then blocked forever in synchronize_srcu(). This harness
# creates a range-staged object, attempts a refused direct write, and then
# drives cutover under timeout.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"

UUID=00000000-0000-0000-0000-00000000f308
SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-write-staging-direct-cutover}
export SPILL_ROOT SPILL_UUID=$UUID
trap spill_cleanup EXIT

REL_PATH="range/direct-cutover.bin"
SERVER_PATH="$SPILL_ROOT/server/$REL_PATH"
SRC_PATH="$SPILL_ROOT/slow/$REL_PATH"
DST_PATH="$SPILL_ROOT/fast/$REL_PATH"
EXPECTED_PATH="$SPILL_ROOT/expected.bin"
SEQ=8001

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool writestagedirect "$UUID"

SYSFS="/sys/fs/smoothfs/$UUID"
if [ ! -d "$SYSFS" ]; then
	echo "  FAIL  missing smoothfs sysfs pool $SYSFS"
	exit 1
fi
echo 1 > "$SYSFS/write_staging_enabled"
echo 98 > "$SYSFS/write_staging_full_pct"

echo "=== seeding source object on colder tier ==="
mkdir -p "$(dirname "$SRC_PATH")"
python3 - <<'PY' "$SRC_PATH"
import pathlib
import sys

pathlib.Path(sys.argv[1]).write_bytes(b"A" * 8192)
PY
spill_assert test ! -e "$DST_PATH"

echo "=== range-stage a buffered write through smoothfs ==="
printf 'XYZ' | dd of="$SERVER_PATH" bs=1 seek=1024 conv=notrunc status=none
python3 - <<'PY' "$EXPECTED_PATH"
import pathlib
import sys

data = bytearray(b"A" * 8192)
data[1024:1027] = b"XYZ"
pathlib.Path(sys.argv[1]).write_bytes(data)
PY

spill_assert cmp -s "$EXPECTED_PATH" "$SERVER_PATH"
spill_assert test ! -e "$DST_PATH"
spill_assert test "$(cat "$SYSFS/range_staged_bytes")" = "3"

echo "=== refused direct write should release cutover SRCU ==="
if dd if=/dev/zero of="$SERVER_PATH" bs=4096 count=1 oflag=direct conv=notrunc \
	status=none 2>"$SPILL_ROOT/direct-write.err"; then
	echo "  FAIL  direct write unexpectedly bypassed staged range"
	spill_rc=1
else
	echo "  ok    direct write refused while range-staged"
fi
spill_assert cmp -s "$EXPECTED_PATH" "$SERVER_PATH"

OID_HEX=$(python3 - <<'PY' "$SERVER_PATH"
import os
import sys

print(os.getxattr(sys.argv[1], b"trusted.smoothfs.oid").hex())
PY
)

HELPER="$SPILL_ROOT/direct_cutover_helper.go"
cat > "$HELPER" <<'GO'
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
	if len(os.Args) != 8 {
		fmt.Fprintf(os.Stderr, "usage: %s pool-uuid oid-hex dest-tier seq rel-path src dst\n", os.Args[0])
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
	relPath := os.Args[5]

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
	if ins.CurrentTier != 1 {
		fatal("current tier before move = %d, want 1", ins.CurrentTier)
	}
	if ins.RelPath != relPath {
		fatal("rel_path = %q, want %q", ins.RelPath, relPath)
	}

	must("move plan", client.MovePlan(poolUUID, oid, uint8(destTier), seq))
	must("copy merged destination", copyFile(os.Args[6], os.Args[7]))
	if ins.HasWriteSeq {
		must("move cutover", client.MoveCutoverVerifyWriteSeq(poolUUID, oid, seq, ins.WriteSeq))
	} else {
		must("move cutover", client.MoveCutover(poolUUID, oid, seq))
	}
	after, err := client.Inspect(poolUUID, oid)
	must("inspect after move", err)
	if after == nil {
		fatal("inspect after move returned nil")
	}
	if after.CurrentTier != uint8(destTier) {
		fatal("current tier after move = %d, want %d", after.CurrentTier, destTier)
	}
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

func must(label string, err error) {
	if err != nil {
		fatal("%s: %v", label, err)
	}
}

func fatal(format string, args ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", args...)
	os.Exit(1)
}
GO

echo "=== cutover must complete after refused direct write ==="
if (cd "$REPO_ROOT" && timeout 15s go run "$HELPER" "$UUID" "$OID_HEX" 0 "$SEQ" \
	"$REL_PATH" "$SERVER_PATH" "$DST_PATH"); then
	echo "  ok    cutover completed after refused direct write"
else
	echo "  FAIL  cutover after refused direct write"
	spill_rc=1
fi

spill_assert test -f "$DST_PATH"
spill_assert cmp -s "$EXPECTED_PATH" "$DST_PATH"
spill_assert cmp -s "$EXPECTED_PATH" "$SERVER_PATH"

echo "=== stale source tier no longer drives reads ==="
python3 - <<'PY' "$SRC_PATH"
import pathlib
import sys

pathlib.Path(sys.argv[1]).write_bytes(b"B" * 8192)
PY
spill_assert cmp -s "$EXPECTED_PATH" "$SERVER_PATH"

spill_finish "write_staging_direct_cutover"

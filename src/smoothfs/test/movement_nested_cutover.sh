#!/bin/bash
# Movement cutover must resolve the destination by namespace-relative path.
#
# This catches regressions where kernel cutover looks up only the source
# basename at the destination tier root. A nested object should cut over to
# <dest-tier>/<relative-parent>/<basename>, not <dest-tier>/<basename>.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"

UUID=00000000-0000-0000-0000-00000000f307
SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-movement-nested}
export SPILL_ROOT SPILL_UUID=$UUID
trap spill_cleanup EXIT

REL_PATH="deep/tree/object.txt"
SERVER_PATH="$SPILL_ROOT/server/$REL_PATH"
SRC_PATH="$SPILL_ROOT/fast/$REL_PATH"
DST_PATH="$SPILL_ROOT/slow/$REL_PATH"
PAYLOAD="nested movement payload"
SEQ=7001

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool movementnested "$UUID"

echo "=== creating nested source object on fastest tier ==="
mkdir -p "$(dirname "$SERVER_PATH")"
printf '%s\n' "$PAYLOAD" > "$SERVER_PATH"
sync

spill_assert test -f "$SRC_PATH"
spill_assert test ! -e "$DST_PATH"

OID_HEX=$(python3 - <<'PY' "$SERVER_PATH"
import os
import sys

print(os.getxattr(sys.argv[1], b"trusted.smoothfs.oid").hex())
PY
)

HELPER="$SPILL_ROOT/move_nested_helper.go"
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
	if len(os.Args) != 7 {
		fmt.Fprintf(os.Stderr, "usage: %s pool-uuid oid-hex dest-tier seq src dst\n", os.Args[0])
		os.Exit(2)
	}
	poolUUID, err := uuid.Parse(os.Args[1])
	must("parse uuid", err)
	rawOID, err := hex.DecodeString(os.Args[2])
	must("decode oid", err)
	if len(rawOID) != smoothfs.OIDLen {
		fmt.Fprintf(os.Stderr, "oid length = %d, want %d\n", len(rawOID), smoothfs.OIDLen)
		os.Exit(2)
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
	if ins.CurrentTier != 0 {
		fatal("current tier before move = %d, want 0", ins.CurrentTier)
	}
	if ins.RelPath != "deep/tree/object.txt" {
		fatal("rel_path = %q, want deep/tree/object.txt", ins.RelPath)
	}

	must("move plan", client.MovePlan(poolUUID, oid, uint8(destTier), seq))
	must("copy destination", copyFile(os.Args[5], os.Args[6]))
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
	if after.RelPath != "deep/tree/object.txt" {
		fatal("post-cutover rel_path = %q, want deep/tree/object.txt", after.RelPath)
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

echo "=== moving nested object fast -> slow ==="
if (cd "$REPO_ROOT" && go run "$HELPER" "$UUID" "$OID_HEX" 1 "$SEQ" "$SRC_PATH" "$DST_PATH"); then
	echo "  ok    nested move cutover completed"
else
	echo "  FAIL  nested move cutover"
	spill_rc=1
fi

spill_assert test -f "$DST_PATH"
spill_assert grep -qx "$PAYLOAD" "$SERVER_PATH"

echo "=== source lower is stale after cutover ==="
printf 'stale source bytes\n' > "$SRC_PATH"
spill_assert grep -qx "$PAYLOAD" "$SERVER_PATH"

spill_finish "movement_nested_cutover"

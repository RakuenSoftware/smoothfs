#!/bin/bash
# Existing smoothfs file descriptors must reissue to the destination after
# cutover, and must fail closed if the destination lower cannot be reopened.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"

UUID=00000000-0000-0000-0000-00000000f309
SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-movement-open-fd}
export SPILL_ROOT SPILL_UUID=$UUID

success_pid=
failure_pid=
cleanup() {
	if [ -n "${success_pid:-}" ]; then
		kill "$success_pid" 2>/dev/null || true
	fi
	if [ -n "${failure_pid:-}" ]; then
		kill "$failure_pid" 2>/dev/null || true
	fi
	spill_cleanup
}
trap cleanup EXIT

wait_for_marker() {
	local marker=$1
	local label=$2
	local i

	for i in $(seq 1 300); do
		if [ -e "$marker" ]; then
			return 0
		fi
		sleep 0.1
	done
	echo "  FAIL  timed out waiting for $label"
	spill_rc=1
	return 1
}

oid_for_path() {
	python3 - <<'PY' "$1"
import os
import sys

print(os.getxattr(sys.argv[1], b"trusted.smoothfs.oid").hex())
PY
}

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool movementopenfd "$UUID"

MOVE_HELPER="$SPILL_ROOT/move_open_fd_helper.go"
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
	if ins.CurrentTier != 0 {
		fatal("current tier before move = %d, want 0", ins.CurrentTier)
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

echo "=== existing writable fd follows destination after cutover ==="
SUCCESS_REL="openfd/success.txt"
SUCCESS_SERVER="$SPILL_ROOT/server/$SUCCESS_REL"
SUCCESS_SRC="$SPILL_ROOT/fast/$SUCCESS_REL"
SUCCESS_DST="$SPILL_ROOT/slow/$SUCCESS_REL"
SUCCESS_READY="$SPILL_ROOT/success.ready"
SUCCESS_GO="$SPILL_ROOT/success.go"
SUCCESS_RESULT="$SPILL_ROOT/success.result"
SUCCESS_INITIAL="open fd initial payload"
SUCCESS_UPDATED="open fd post-cutover write"
SUCCESS_STALE="stale source bytes"
SUCCESS_SEQ=9001

mkdir -p "$(dirname "$SUCCESS_SERVER")"
printf '%s\n' "$SUCCESS_INITIAL" > "$SUCCESS_SERVER"
sync
spill_assert test -f "$SUCCESS_SRC"
spill_assert test ! -e "$SUCCESS_DST"
SUCCESS_OID=$(oid_for_path "$SUCCESS_SERVER")

SUCCESS_HELPER="$SPILL_ROOT/open_fd_success.py"
cat > "$SUCCESS_HELPER" <<'PY'
import os
import pathlib
import sys
import time

path, ready, go, result, expected, updated = sys.argv[1:]

def fail(message):
    pathlib.Path(result).write_text(message + "\n")
    print(message, file=sys.stderr)
    sys.exit(1)

with open(path, "r+b", buffering=0) as handle:
    before = handle.read().decode()
    if before != expected + "\n":
        fail(f"pre-cutover read = {before!r}, want {expected!r}")
    pathlib.Path(ready).write_text("ready\n")

    deadline = time.time() + 30
    while not os.path.exists(go):
        if time.time() > deadline:
            fail("timed out waiting for cutover")
        time.sleep(0.1)

    handle.seek(0)
    after = handle.read().decode()
    if after != expected + "\n":
        fail(f"post-cutover read through existing fd = {after!r}, want {expected!r}")

    handle.seek(0)
    handle.write((updated + "\n").encode())
    handle.truncate()
    os.fsync(handle.fileno())

pathlib.Path(result).write_text("ok\n")
PY

python3 "$SUCCESS_HELPER" "$SUCCESS_SERVER" "$SUCCESS_READY" "$SUCCESS_GO" \
	"$SUCCESS_RESULT" "$SUCCESS_INITIAL" "$SUCCESS_UPDATED" &
success_pid=$!
if wait_for_marker "$SUCCESS_READY" "writable fd opener"; then
	if (cd "$REPO_ROOT" && go run "$MOVE_HELPER" "$UUID" "$SUCCESS_OID" 1 \
		"$SUCCESS_SEQ" "$SUCCESS_SRC" "$SUCCESS_DST"); then
		echo "  ok    cutover completed while writable fd was open"
	else
		echo "  FAIL  cutover with open writable fd"
		spill_rc=1
	fi
	printf '%s\n' "$SUCCESS_STALE" > "$SUCCESS_SRC"
	touch "$SUCCESS_GO"
fi
if wait "$success_pid"; then
	echo "  ok    existing writable fd reissued to destination"
else
	echo "  FAIL  existing writable fd reissue"
	cat "$SUCCESS_RESULT" 2>/dev/null || true
	spill_rc=1
fi
success_pid=

spill_assert grep -qx "$SUCCESS_UPDATED" "$SUCCESS_SERVER"
spill_assert grep -qx "$SUCCESS_UPDATED" "$SUCCESS_DST"
spill_assert grep -qx "$SUCCESS_STALE" "$SUCCESS_SRC"

echo "=== failed destination reopen does not fall back to stale source ==="
FAIL_REL="openfd/reopen-denied.txt"
FAIL_SERVER="$SPILL_ROOT/server/$FAIL_REL"
FAIL_SRC="$SPILL_ROOT/fast/$FAIL_REL"
FAIL_DST="$SPILL_ROOT/slow/$FAIL_REL"
FAIL_IPC="$SPILL_ROOT/reopen-denied-ipc"
FAIL_READY="$FAIL_IPC/ready"
FAIL_GO="$FAIL_IPC/go"
FAIL_RESULT="$FAIL_IPC/result"
FAIL_INITIAL="reopen denied initial payload"
FAIL_STALE="reopen denied stale source"
FAIL_SEQ=9002

mkdir -p "$(dirname "$FAIL_SERVER")" "$FAIL_IPC"
chmod 0777 "$FAIL_IPC"
printf '%s\n' "$FAIL_INITIAL" > "$FAIL_SERVER"
chmod 0666 "$FAIL_SERVER"
sync
spill_assert test -f "$FAIL_SRC"
spill_assert test ! -e "$FAIL_DST"
FAIL_OID=$(oid_for_path "$FAIL_SERVER")

FAIL_HELPER="$SPILL_ROOT/open_fd_reopen_denied.py"
cat > "$FAIL_HELPER" <<'PY'
import errno
import grp
import os
import pathlib
import pwd
import sys
import time

path, ready, go, result, expected = sys.argv[1:]

def fail(message):
    pathlib.Path(result).write_text(message + "\n")
    print(message, file=sys.stderr)
    sys.exit(1)

def drop_to_nobody():
    user = pwd.getpwnam("nobody")
    try:
        group = grp.getgrnam("nogroup")
        gid = group.gr_gid
    except KeyError:
        gid = user.pw_gid
    os.setgroups([])
    os.setgid(gid)
    os.setuid(user.pw_uid)

drop_to_nobody()

with open(path, "rb", buffering=0) as handle:
    before = handle.read().decode()
    if before != expected + "\n":
        fail(f"pre-cutover read = {before!r}, want {expected!r}")
    pathlib.Path(ready).write_text("ready\n")

    deadline = time.time() + 30
    while not os.path.exists(go):
        if time.time() > deadline:
            fail("timed out waiting for cutover")
        time.sleep(0.1)

    # The reissue perm-check can surface EACCES on either the lseek or
    # the read of the new lower (smoothfs_llseek goes through
    # smoothfs_lower_file -> reissue too). Either is a valid signal that
    # the lazy reopen against the destination tier was denied.
    try:
        handle.seek(0)
        after = handle.read()
    except OSError as err:
        if err.errno in (errno.EACCES, errno.EPERM):
            pathlib.Path(result).write_text("ok\n")
            sys.exit(0)
        fail(f"post-cutover access failed with unexpected errno {err.errno}: {err}")

fail(f"post-cutover read unexpectedly returned {after!r}")
PY

python3 "$FAIL_HELPER" "$FAIL_SERVER" "$FAIL_READY" "$FAIL_GO" \
	"$FAIL_RESULT" "$FAIL_INITIAL" &
failure_pid=$!
if wait_for_marker "$FAIL_READY" "unprivileged fd opener"; then
	if (cd "$REPO_ROOT" && go run "$MOVE_HELPER" "$UUID" "$FAIL_OID" 1 \
		"$FAIL_SEQ" "$FAIL_SRC" "$FAIL_DST"); then
		echo "  ok    cutover completed while unprivileged fd was open"
	else
		echo "  FAIL  cutover with open unprivileged fd"
		spill_rc=1
	fi
	chmod 0000 "$FAIL_DST"
	printf '%s\n' "$FAIL_STALE" > "$FAIL_SRC"
	touch "$FAIL_GO"
fi
if wait "$failure_pid"; then
	echo "  ok    destination reopen failure returned an error"
else
	echo "  FAIL  destination reopen failure handling"
	cat "$FAIL_RESULT" 2>/dev/null || true
	spill_rc=1
fi
failure_pid=

spill_assert grep -qx "$FAIL_STALE" "$FAIL_SRC"

spill_finish "movement_open_fd_cutover"

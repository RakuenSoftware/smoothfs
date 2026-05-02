#!/bin/bash
# Phase 6.0 — O_DIRECT conformance on smoothfs. The iSCSI fileio
# backend in LIO opens its backing file with O_DIRECT by default, so
# any smoothfs mount that wants to host LUN backing files must
# accept O_DIRECT opens and pass aligned direct I/O through to the
# lower byte-for-byte.
#
# Covers:
#   1. open(O_DIRECT) succeeds on a smoothfs regular file (the
#      kernel's do_dentry_open gate requires FMODE_CAN_ODIRECT on the
#      upper file — propagated from the lower's capability in Phase
#      6.0's smoothfs_open).
#   2. Block-aligned O_DIRECT write lands byte-identical on the
#      lower tier's native file.
#   3. Block-aligned O_DIRECT read returns the same bytes.
#   4. Block-misaligned O_DIRECT access is rejected by the lower
#      (EINVAL) — confirms smoothfs doesn't silently downgrade to
#      buffered IO, which would break LIO's write-through contract.

set -u

. "$(dirname "$0")/lower_fs_lib.sh"

ROOT=/tmp/smoothfs-odirect
UUID=00000000-0000-0000-0000-000000000600

cleanup() {
    umount -l $ROOT/server $ROOT/fast $ROOT/slow 2>/dev/null || true
    rm -rf $ROOT
}
trap cleanup EXIT

rc=0
assert() {
    if "$@"; then
        echo "  ok    $*"
    else
        echo "  FAIL  $*"
        rc=1
    fi
}

echo "=== laying down 2-tier XFS smoothfs ==="
rm -rf $ROOT
mkdir -p $ROOT/{fast,slow,server}
truncate -s 1G $ROOT/fast.img $ROOT/slow.img
mkfs_lower $ROOT/fast.img
mkfs_lower $ROOT/slow.img
mount -o loop $ROOT/fast.img $ROOT/fast
mount -o loop $ROOT/slow.img $ROOT/slow
mount -t smoothfs -o pool=odirect,uuid=$UUID,tiers=$ROOT/fast:$ROOT/slow \
      none $ROOT/server
chmod 1777 $ROOT/server

echo "=== open(O_DIRECT) round-trip ==="
# 4 KiB-aligned, 1 MiB payload.
SRC=/tmp/odirect.src
DST=/tmp/odirect.dst
dd if=/dev/urandom of=$SRC bs=4K count=256 status=none
assert dd if=$SRC of=$ROOT/server/probe.bin bs=4K count=256 \
       oflag=direct conv=fsync status=none
assert dd if=$ROOT/server/probe.bin of=$DST bs=4K count=256 \
       iflag=direct status=none
assert cmp -s $SRC $DST

echo "=== lower-tier mirror is byte-identical ==="
# Written through smoothfs, stored on the fast tier (the placement
# default for fresh creates).
assert test -f $ROOT/fast/probe.bin
SHA_UPPER=$(sha256sum $ROOT/server/probe.bin | awk '{print $1}')
SHA_LOWER=$(sha256sum $ROOT/fast/probe.bin | awk '{print $1}')
assert test "$SHA_UPPER" = "$SHA_LOWER"

echo "=== misaligned O_DIRECT rejected ==="
# 513-byte write with bs=513 oflag=direct must fail — XFS (and every
# supported lower) rejects unaligned direct I/O with EINVAL. If
# smoothfs silently downgraded to buffered IO this would succeed
# and LIO's write-through ordering would be a lie.
#
# dd doesn't surface EINVAL in its exit code reliably, so use a
# tiny python helper that checks errno directly.
python3 - <<'PY'
import os, sys
path = "/tmp/smoothfs-odirect/server/misalign.bin"
try:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_DIRECT, 0o644)
except OSError as e:
    print(f"  FAIL  open O_DIRECT on smoothfs: {e}", file=sys.stderr)
    sys.exit(1)
try:
    # 513 bytes — not a multiple of the lower's logical block size.
    buf = b"A" * 513
    try:
        os.write(fd, buf)
    except OSError as e:
        if e.errno == 22:  # EINVAL
            print("  ok    misaligned O_DIRECT write rejected with EINVAL")
            sys.exit(0)
        print(f"  FAIL  misaligned write got unexpected errno: {e}", file=sys.stderr)
        sys.exit(1)
    print("  FAIL  misaligned O_DIRECT write was accepted (buffered fallback?)", file=sys.stderr)
    sys.exit(1)
finally:
    os.close(fd)
PY
rc=$(( rc + $? ))

echo
if [ $rc -eq 0 ]; then
    echo "odirect: PASS (O_DIRECT open + aligned rw + lower mirror + misalign rejection)"
else
    echo "odirect: FAIL"
fi
exit $rc

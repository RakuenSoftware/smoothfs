#!/bin/bash
# Phase 6.2 — LUN pin xattr. Exercises the trusted.smoothfs.lun
# handler in src/smoothfs/xattr.c: flipping the 1-byte xattr
# moves pin_state between PIN_NONE and PIN_LUN, and the pin is
# disjoint from trusted.smoothfs.lease — setting one while the
# other is held must return EBUSY.
#
# Assertions (12):
#   1.  Fresh file reads lun xattr as 0.
#   2.  setxattr lun=1 succeeds.
#   3.  lun xattr reads back as 1.
#   4.  While PIN_LUN, setxattr lease=1 fails with EBUSY.
#   5.  While PIN_LUN, lease xattr still reads 0 (pin unchanged).
#   6.  removexattr lun clears pin_state back to PIN_NONE
#       (lun xattr reads 0).
#   7.  After clear, setxattr lease=1 succeeds.
#   8.  While PIN_LEASE, setxattr lun=1 fails with EBUSY.
#   9.  While PIN_LEASE, lun xattr still reads 0 (pin unchanged).
#   10. removexattr lease clears back to PIN_NONE.
#   11. After clear, setxattr lun=1 again succeeds.
#   12. Setting lun to 0 via value (not removexattr) also clears.

set -u

. "$(dirname "$0")/lower_fs_lib.sh"

ROOT=/tmp/smoothfs-lunpin
UUID=00000000-0000-0000-0000-000000000620

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

read_pin() {
    getfattr -n "trusted.smoothfs.$1" --only-values -h "$2" 2>/dev/null \
        | od -An -tx1 | tr -d ' \n'
}

echo "=== laying down 2-tier XFS smoothfs ==="
rm -rf $ROOT
mkdir -p $ROOT/{fast,slow,server}
truncate -s 1G $ROOT/fast.img $ROOT/slow.img
mkfs_lower $ROOT/fast.img
mkfs_lower $ROOT/slow.img
mount -o loop $ROOT/fast.img $ROOT/fast
mount -o loop $ROOT/slow.img $ROOT/slow
mount -t smoothfs -o pool=lunpin,uuid=$UUID,tiers=$ROOT/fast:$ROOT/slow \
      none $ROOT/server

FILE=$ROOT/server/lun.img
printf "phase62\n" > $FILE

echo "=== starting state: no pin ==="
v=$(read_pin lun "$FILE")
assert test "$v" = "00"

echo "=== setxattr lun=1 -> PIN_LUN ==="
printf '\x01' | setfattr -n trusted.smoothfs.lun -v "$(printf '\x01')" "$FILE"
# setfattr with `-v x` does a raw byte; we need the literal 0x01 byte.
# Above form is awkward — use a tiny python helper instead for clarity
# and fall through to that for the remaining setxattr calls too.
python3 -c 'import os,sys; os.setxattr(sys.argv[1], b"trusted.smoothfs.lun", b"\x01")' "$FILE"
v=$(read_pin lun "$FILE")
assert test "$v" = "01"

echo "=== setxattr lease=1 while PIN_LUN held -> EBUSY ==="
python3 - <<'PY' "$FILE"
import os, sys, errno
try:
    os.setxattr(sys.argv[1], b"trusted.smoothfs.lease", b"\x01")
    print(f"  FAIL  lease setxattr under PIN_LUN was accepted", file=sys.stderr)
    sys.exit(1)
except OSError as e:
    if e.errno == errno.EBUSY:
        print("  ok    lease setxattr rejected with EBUSY under PIN_LUN")
        sys.exit(0)
    print(f"  FAIL  unexpected errno {e.errno}: {e}", file=sys.stderr)
    sys.exit(1)
PY
rc=$(( rc + $? ))

v=$(read_pin lease "$FILE")
assert test "$v" = "00"

echo "=== removexattr lun -> PIN_NONE ==="
python3 -c 'import os,sys; os.removexattr(sys.argv[1], b"trusted.smoothfs.lun")' "$FILE"
v=$(read_pin lun "$FILE")
assert test "$v" = "00"

echo "=== setxattr lease=1 -> PIN_LEASE ==="
python3 -c 'import os,sys; os.setxattr(sys.argv[1], b"trusted.smoothfs.lease", b"\x01")' "$FILE"
v=$(read_pin lease "$FILE")
assert test "$v" = "01"

echo "=== setxattr lun=1 while PIN_LEASE held -> EBUSY ==="
python3 - <<'PY' "$FILE"
import os, sys, errno
try:
    os.setxattr(sys.argv[1], b"trusted.smoothfs.lun", b"\x01")
    print(f"  FAIL  lun setxattr under PIN_LEASE was accepted", file=sys.stderr)
    sys.exit(1)
except OSError as e:
    if e.errno == errno.EBUSY:
        print("  ok    lun setxattr rejected with EBUSY under PIN_LEASE")
        sys.exit(0)
    print(f"  FAIL  unexpected errno {e.errno}: {e}", file=sys.stderr)
    sys.exit(1)
PY
rc=$(( rc + $? ))

v=$(read_pin lun "$FILE")
assert test "$v" = "00"

echo "=== removexattr lease -> PIN_NONE, then lun=1 again ==="
python3 -c 'import os,sys; os.removexattr(sys.argv[1], b"trusted.smoothfs.lease")' "$FILE"
python3 -c 'import os,sys; os.setxattr(sys.argv[1], b"trusted.smoothfs.lun", b"\x01")' "$FILE"
v=$(read_pin lun "$FILE")
assert test "$v" = "01"

echo "=== setxattr lun=0 value (not removexattr) also clears ==="
python3 -c 'import os,sys; os.setxattr(sys.argv[1], b"trusted.smoothfs.lun", b"\x00")' "$FILE"
v=$(read_pin lun "$FILE")
assert test "$v" = "00"

echo
if [ $rc -eq 0 ]; then
    echo "iscsi_pin: PASS (PIN_LUN toggle + PIN_LEASE exclusion)"
else
    echo "iscsi_pin: FAIL"
fi
exit $rc

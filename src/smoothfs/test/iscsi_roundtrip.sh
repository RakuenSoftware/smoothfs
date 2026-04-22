#!/bin/bash
# Phase 6.1 — LIO file-backed LUN over a smoothfs mount.
#
# Stands up an isolated iSCSI target on 127.0.0.1:3260 with a
# fileio backstore whose backing file lives on a 2-tier XFS
# smoothfs, logs in via open-iscsi as the local initiator, and
# verifies a block-level write/read round-trip against the smoothfs
# view and the fast-tier native file. No smoothfs-specific target
# config — stock targetcli-fb.  Phase 6.0's O_DIRECT conformance is
# the only kernel change this relies on.
#
# Assertions (8):
#   1.  fileio backstore creation over the smoothfs-backed file succeeds
#   2.  iSCSI target comes up on loopback:3260
#   3.  iscsiadm discovers the target
#   4.  iscsiadm login exposes a /dev/sdX device
#   5.  Block-level dd write succeeds through the block device
#   6.  Logout + relogin preserves content (durability through session
#       bounce)
#   7.  Block-level dd read returns byte-identical content
#   8.  The fast-tier backing file carries the same sha256 as what the
#       initiator wrote
#
# Prereq: `apt-get install targetcli-fb open-iscsi`, iscsid running.

set -u

ROOT=/tmp/iscsi-smoothfs
UUID=00000000-0000-0000-0000-000000000610
PORTAL=127.0.0.1
PORT=3260
TARGET_IQN="iqn.2026-04.com.smoothnas:phase6lun"
LUN_NAME=smoothfs-lun0
BACKFILE=$ROOT/server/lun0.img
LUN_SIZE=64M
INITIATOR_IQN=$(awk -F= '/^InitiatorName=/ {print $2}' /etc/iscsi/initiatorname.iscsi)

cleanup() {
    iscsiadm -m node -T $TARGET_IQN -p $PORTAL:$PORT --logout 2>/dev/null || true
    iscsiadm -m node -T $TARGET_IQN -p $PORTAL:$PORT -o delete 2>/dev/null || true
    targetcli /iscsi delete $TARGET_IQN 2>/dev/null || true
    targetcli /backstores/fileio delete $LUN_NAME 2>/dev/null || true
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

echo "=== preparing clean state ==="
iscsiadm -m node -T $TARGET_IQN -p $PORTAL:$PORT --logout 2>/dev/null || true
iscsiadm -m node -T $TARGET_IQN -p $PORTAL:$PORT -o delete 2>/dev/null || true
targetcli /iscsi delete $TARGET_IQN 2>/dev/null || true
targetcli /backstores/fileio delete $LUN_NAME 2>/dev/null || true
umount -l $ROOT/server $ROOT/fast $ROOT/slow 2>/dev/null || true
rm -rf $ROOT
systemctl is-active iscsid >/dev/null 2>&1 || systemctl start iscsid

echo "=== laying down 2-tier XFS smoothfs ==="
mkdir -p $ROOT/{fast,slow,server}
truncate -s 1G $ROOT/fast.img $ROOT/slow.img
mkfs.xfs -q -f $ROOT/fast.img
mkfs.xfs -q -f $ROOT/slow.img
mount -o loop $ROOT/fast.img $ROOT/fast
mount -o loop $ROOT/slow.img $ROOT/slow
mount -t smoothfs -o pool=iscsi,uuid=$UUID,tiers=$ROOT/fast:$ROOT/slow \
      none $ROOT/server

echo "=== creating LUN backing file on smoothfs ==="
truncate -s $LUN_SIZE $BACKFILE
assert test -f $BACKFILE

echo "=== fileio backstore (smoothfs-backed) ==="
targetcli /backstores/fileio create \
          name=$LUN_NAME file_or_dev=$BACKFILE size=$LUN_SIZE >/dev/null
targetcli /backstores/fileio ls >/tmp/iscsi.backstore 2>&1
assert grep -q "$LUN_NAME" /tmp/iscsi.backstore

echo "=== iSCSI target on $PORTAL:$PORT ==="
targetcli /iscsi create $TARGET_IQN >/dev/null
targetcli /iscsi/$TARGET_IQN/tpg1/luns create /backstores/fileio/$LUN_NAME >/dev/null
targetcli /iscsi/$TARGET_IQN/tpg1/acls create $INITIATOR_IQN >/dev/null
targetcli /iscsi/$TARGET_IQN/tpg1 set attribute authentication=0 >/dev/null
# Swap the default 0.0.0.0 portal for a loopback-only one.
targetcli /iscsi/$TARGET_IQN/tpg1/portals delete 0.0.0.0 3260 2>/dev/null || true
targetcli /iscsi/$TARGET_IQN/tpg1/portals create $PORTAL $PORT >/dev/null
ss -lnt >/tmp/iscsi.ports
assert grep -q ":$PORT " /tmp/iscsi.ports

echo "=== iscsiadm discover + login ==="
iscsiadm -m discovery -t st -p $PORTAL:$PORT >/tmp/iscsi.disco 2>&1
assert grep -q "$TARGET_IQN" /tmp/iscsi.disco

iscsiadm -m node -T $TARGET_IQN -p $PORTAL:$PORT --login >/tmp/iscsi.login 2>&1
assert grep -q "successful" /tmp/iscsi.login

# Wait for the kernel to expose the LUN as /dev/sdX.  Poll via
# /sys/class/iscsi_session/.../target*/.../block/ rather than
# scanning /proc/partitions, so we hit exactly this target's LUN.
SDEV=""
for i in $(seq 1 30); do
    SDEV=$(ls /sys/class/iscsi_session/*/device/target*/*/block/ 2>/dev/null \
           | head -1)
    [ -n "$SDEV" ] && break
    sleep 0.2
done
assert test -n "$SDEV"
if [ -z "$SDEV" ]; then
    echo "  (no /dev/sdX surfaced; aborting IO tests)"
    exit 1
fi
DEVPATH=/dev/$SDEV
echo "  LUN device: $DEVPATH"

echo "=== write / read round-trip through iSCSI block device ==="
# 8 MiB of random bytes — smaller than the LUN, bigger than any
# single request the initiator will coalesce.
PAYLOAD=/tmp/iscsi-payload.bin
dd if=/dev/urandom of=$PAYLOAD bs=4K count=2048 status=none
SHA_PAYLOAD=$(sha256sum $PAYLOAD | awk '{print $1}')

# The iSCSI-side LUN is seen as a whole disk.  Write from offset 0.
assert dd if=$PAYLOAD of=$DEVPATH bs=4K count=2048 oflag=direct \
       conv=fsync status=none

echo "=== logout + relogin (session-bounce durability) ==="
iscsiadm -m node -T $TARGET_IQN -p $PORTAL:$PORT --logout >/dev/null 2>&1
sleep 0.3
iscsiadm -m node -T $TARGET_IQN -p $PORTAL:$PORT --login >/dev/null 2>&1
SDEV=""
for i in $(seq 1 30); do
    SDEV=$(ls /sys/class/iscsi_session/*/device/target*/*/block/ 2>/dev/null \
           | head -1)
    [ -n "$SDEV" ] && break
    sleep 0.2
done
assert test -n "$SDEV"
DEVPATH=/dev/$SDEV

READBACK=/tmp/iscsi-readback.bin
assert dd if=$DEVPATH of=$READBACK bs=4K count=2048 iflag=direct status=none
SHA_READ=$(sha256sum $READBACK | awk '{print $1}')
assert test "$SHA_READ" = "$SHA_PAYLOAD"

echo "=== fast-tier mirror sha256 ==="
# LIO's fileio backend writes to the first $LUN_SIZE bytes of the
# backing file.  Our 8 MiB payload is at offset 0, so compare the
# first 8 MiB of the fast-tier native file to the payload.
FAST_BACKFILE=$ROOT/fast/lun0.img
LOWER_PREFIX=/tmp/iscsi-lower-prefix.bin
dd if=$FAST_BACKFILE of=$LOWER_PREFIX bs=4K count=2048 status=none
SHA_LOWER=$(sha256sum $LOWER_PREFIX | awk '{print $1}')
assert test "$SHA_LOWER" = "$SHA_PAYLOAD"

echo
if [ $rc -eq 0 ]; then
    echo "iscsi_roundtrip: PASS (LIO fileio over smoothfs, block-level round-trip OK)"
else
    echo "iscsi_roundtrip: FAIL"
fi
exit $rc

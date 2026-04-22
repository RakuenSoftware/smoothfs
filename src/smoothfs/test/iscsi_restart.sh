#!/bin/bash
# Phase 6.3 — target restart / reconnect correctness.
#
# Reuses the Phase 6.1 topology (fileio LUN backed by a smoothfs
# file, loopback portal, default initiator) and exercises two
# target-side churn scenarios that a production LIO deployment
# has to survive:
#
#   A. targetctl save → clear → restore.  This is what systemd's
#      /etc/rtslib-fb-target/saveconfig.json drives on every boot
#      of target.service; if it breaks, LIO doesn't come back after
#      a reboot.  We confirm that the backing file on smoothfs is
#      untouched across the tear-down and that the initiator can
#      log in and read the same bytes it wrote before the bounce.
#
#   B. iscsid bounce on the target host.  Stops iscsid, drops any
#      residual state the initiator had, restarts iscsid, logs
#      back in, and re-reads.  Doesn't touch smoothfs but proves
#      there's no state on the smoothfs side that the session
#      restart depends on.
#
# Assertions (12):
#   1. Initial target up; initiator logs in, sees /dev/sdX.
#   2. Write payload succeeds.
#   3. Logout before bounce.
#   4. targetctl save succeeds.
#   5. targetctl clear removes the target (IQN no longer in tree).
#   6. Backing file on smoothfs still present + sha256 unchanged
#      across the clear (the critical Phase 6 invariant).
#   7. targetctl restore brings the target back; IQN reappears.
#   8. Portal listening on loopback:3260 again.
#   9. Initiator relogs in; /dev/sdX surfaces.
#   10. Post-restart readback sha256 matches original payload.
#   11. iscsid bounce (stop, logout → no session, start) survives.
#   12. Read after iscsid bounce still matches.

set -u

ROOT=/tmp/iscsi-restart
UUID=00000000-0000-0000-0000-000000000630
PORTAL=127.0.0.1
PORT=3261
TARGET_IQN="iqn.2026-04.com.smoothnas:phase63lun"
LUN_NAME=smoothfs-lun63
BACKFILE=$ROOT/server/lun63.img
LUN_SIZE=64M
INITIATOR_IQN=$(awk -F= '/^InitiatorName=/ {print $2}' /etc/iscsi/initiatorname.iscsi)
SAVED=/tmp/iscsi-restart-save.json

cleanup() {
    iscsiadm -m node -T $TARGET_IQN -p $PORTAL:$PORT --logout 2>/dev/null || true
    iscsiadm -m node -T $TARGET_IQN -p $PORTAL:$PORT -o delete 2>/dev/null || true
    targetcli /iscsi delete $TARGET_IQN 2>/dev/null || true
    targetcli /backstores/fileio delete $LUN_NAME 2>/dev/null || true
    rm -f $SAVED
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

# Find the LUN's /dev/sdX by walking /sys/class/iscsi_session.
find_lun_dev() {
    local sdev=""
    for _ in $(seq 1 30); do
        sdev=$(ls /sys/class/iscsi_session/*/device/target*/*/block/ \
               2>/dev/null | head -1)
        [ -n "$sdev" ] && break
        sleep 0.2
    done
    echo "$sdev"
}

echo "=== preparing clean state ==="
cleanup
systemctl is-active iscsid >/dev/null 2>&1 || systemctl start iscsid

echo "=== laying down 2-tier XFS smoothfs ==="
mkdir -p $ROOT/{fast,slow,server}
truncate -s 1G $ROOT/fast.img $ROOT/slow.img
mkfs.xfs -q -f $ROOT/fast.img
mkfs.xfs -q -f $ROOT/slow.img
mount -o loop $ROOT/fast.img $ROOT/fast
mount -o loop $ROOT/slow.img $ROOT/slow
mount -t smoothfs -o pool=restart,uuid=$UUID,tiers=$ROOT/fast:$ROOT/slow \
      none $ROOT/server

truncate -s $LUN_SIZE $BACKFILE

echo "=== target up + initial login ==="
targetcli /backstores/fileio create name=$LUN_NAME file_or_dev=$BACKFILE size=$LUN_SIZE >/dev/null
targetcli /iscsi create $TARGET_IQN >/dev/null
targetcli /iscsi/$TARGET_IQN/tpg1/luns create /backstores/fileio/$LUN_NAME >/dev/null
targetcli /iscsi/$TARGET_IQN/tpg1/acls create $INITIATOR_IQN >/dev/null
targetcli /iscsi/$TARGET_IQN/tpg1 set attribute authentication=0 >/dev/null
targetcli /iscsi/$TARGET_IQN/tpg1/portals delete 0.0.0.0 3260 2>/dev/null || true
targetcli /iscsi/$TARGET_IQN/tpg1/portals create $PORTAL $PORT >/dev/null

iscsiadm -m discovery -t st -p $PORTAL:$PORT >/dev/null 2>&1
iscsiadm -m node -T $TARGET_IQN -p $PORTAL:$PORT --login >/dev/null 2>&1
SDEV=$(find_lun_dev)
assert test -n "$SDEV"
DEVPATH=/dev/$SDEV

echo "=== write payload ==="
PAYLOAD=/tmp/iscsi-restart-payload.bin
dd if=/dev/urandom of=$PAYLOAD bs=4K count=2048 status=none
SHA_PAYLOAD=$(sha256sum $PAYLOAD | awk '{print $1}')
assert dd if=$PAYLOAD of=$DEVPATH bs=4K count=2048 oflag=direct conv=fsync status=none

echo "=== logout before target bounce ==="
iscsiadm -m node -T $TARGET_IQN -p $PORTAL:$PORT --logout >/dev/null 2>&1
# Poll for session removal.
for _ in $(seq 1 25); do
    ls /sys/class/iscsi_session/ 2>/dev/null | grep -q . || break
    sleep 0.2
done
assert test ! -d /sys/class/iscsi_session/session1

echo "=== A. targetctl save → clear → restore ==="
targetctl save $SAVED
assert test -s $SAVED
targetctl clear
targetcli ls /iscsi >/tmp/iscsi-restart-lsA.txt 2>&1
assert test "$(grep -c "$TARGET_IQN" /tmp/iscsi-restart-lsA.txt)" = "0"

# Backing-file integrity across the clear — this is the iSCSI-over-
# smoothfs invariant that Phase 6 needs to hold.
SHA_LOWER_AFTER_CLEAR=$(sha256sum $ROOT/fast/lun63.img | awk '{print $1}')
# Want the first 8 MiB (our payload) to survive. Compare via prefix.
dd if=$ROOT/fast/lun63.img of=/tmp/iscsi-restart-prefix.bin \
   bs=4K count=2048 status=none
SHA_PREFIX_CLEAR=$(sha256sum /tmp/iscsi-restart-prefix.bin | awk '{print $1}')
assert test "$SHA_PREFIX_CLEAR" = "$SHA_PAYLOAD"

targetctl restore $SAVED
targetcli ls /iscsi >/tmp/iscsi-restart-lsB.txt 2>&1
assert grep -q "$TARGET_IQN" /tmp/iscsi-restart-lsB.txt
ss -lnt >/tmp/iscsi-restart-ports
assert grep -q ":$PORT " /tmp/iscsi-restart-ports

iscsiadm -m node -T $TARGET_IQN -p $PORTAL:$PORT --login >/dev/null 2>&1
SDEV=$(find_lun_dev)
assert test -n "$SDEV"
DEVPATH=/dev/$SDEV

echo "=== post-restart readback matches ==="
READBACK=/tmp/iscsi-restart-readback.bin
dd if=$DEVPATH of=$READBACK bs=4K count=2048 iflag=direct status=none
SHA_READ=$(sha256sum $READBACK | awk '{print $1}')
assert test "$SHA_READ" = "$SHA_PAYLOAD"

echo "=== B. iscsid bounce ==="
iscsiadm -m node -T $TARGET_IQN -p $PORTAL:$PORT --logout >/dev/null 2>&1
systemctl stop iscsid.socket iscsid 2>/dev/null
sleep 0.3
systemctl start iscsid
# discovery cache persists across iscsid restarts; just log back in.
iscsiadm -m node -T $TARGET_IQN -p $PORTAL:$PORT --login >/dev/null 2>&1
SDEV=$(find_lun_dev)
assert test -n "$SDEV"
DEVPATH=/dev/$SDEV

READBACK2=/tmp/iscsi-restart-readback2.bin
dd if=$DEVPATH of=$READBACK2 bs=4K count=2048 iflag=direct status=none
SHA_READ2=$(sha256sum $READBACK2 | awk '{print $1}')
assert test "$SHA_READ2" = "$SHA_PAYLOAD"

echo
if [ $rc -eq 0 ]; then
    echo "iscsi_restart: PASS (targetctl bounce + iscsid bounce + content intact)"
else
    echo "iscsi_restart: FAIL"
fi
exit $rc

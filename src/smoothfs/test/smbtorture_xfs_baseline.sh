#!/bin/bash
# smbtorture_xfs_baseline — Phase 5.5 diagnostic.
#
# Runs the Phase 5.4 KNOWN_ISSUES list against a plain-XFS Samba share
# (no smoothfs stacking) so each failure can be classified:
#
#   FAIL on XFS   → Samba/Linux limitation, not smoothfs
#   PASS on XFS   → smoothfs-specific bug; fix in smoothfs before
#                    promoting to MUST_PASS in smbtorture.sh
#
# The MAIN smbtorture.sh is the Phase 5.4 gate; this is the
# comparison tool you reach for when deciding what a failure is
# really telling you. Intended for ad-hoc runs, not CI.
#
# Run as root. Samba-testsuite + xfsprogs required.

set -u

. "$(dirname "$0")/lower_fs_lib.sh"

ROOT=/tmp/smbt-xfs-baseline
LOGDIR=/tmp/smbt-xfs-baseline-logs
PORT=8446
SHARE=xfs
USER=xfsbase
PASS=xfsbase-phase55
SMBD_PID=""

cleanup() {
    if [ -n "$SMBD_PID" ]; then
        pkill -9 -f "smbd.*$ROOT/samba/smb.conf" 2>/dev/null
    fi
    umount -l $ROOT/server 2>/dev/null || true
    rm -rf $ROOT
}
trap cleanup EXIT

# Same set as smbtorture.sh's KNOWN_ISSUES; mirror when that list
# changes. raw.search was here through Phase 5.5 and was promoted
# to MUST_PASS in Phase 5.6.
KNOWN_ISSUES=(
    raw.rename
    smb2.rw
    smb2.getinfo
    smb2.setinfo
)

echo "=== laying down plain XFS (no smoothfs) ==="
systemctl stop smbd nmbd samba samba-ad-dc 2>/dev/null || true
umount -l $ROOT/server 2>/dev/null
rm -rf $ROOT $LOGDIR
mkdir -p $ROOT/{server,samba/private} $LOGDIR
truncate -s 1G $ROOT/xfs.img
mkfs_lower $ROOT/xfs.img
mount -o loop $ROOT/xfs.img $ROOT/server
chmod 1777 $ROOT/server

echo "=== writing smb.conf (matches smbtorture.sh modulo mount path) ==="
cat > $ROOT/samba/smb.conf <<EOF
[global]
    workgroup = WORKGROUP
    server string = xfs baseline
    server role = standalone server
    log level = 0
    log file = $ROOT/samba/log.%m
    pid directory = $ROOT/samba
    lock directory = $ROOT/samba
    state directory = $ROOT/samba
    cache directory = $ROOT/samba
    private dir = $ROOT/samba/private
    passdb backend = tdbsam:$ROOT/samba/passdb.tdb
    smb ports = $PORT
    bind interfaces only = yes
    interfaces = lo
    map to guest = never
    disable spoolss = yes
    load printers = no
    printing = bsd
    printcap name = /dev/null
    ea support = yes
    store dos attributes = yes
    unix extensions = no
    client min protocol = NT1
    server min protocol = NT1

[$SHARE]
    path = $ROOT/server
    read only = no
    valid users = $USER
    force user = root
    ea support = yes
    strict locking = auto
    mangled names = yes
EOF

id $USER >/dev/null 2>&1 || useradd --no-create-home --shell /usr/sbin/nologin $USER
(echo "$PASS"; echo "$PASS") | smbpasswd -c $ROOT/samba/smb.conf -a -s $USER >/dev/null

smbd --foreground --no-process-group --configfile=$ROOT/samba/smb.conf \
     --debug-stdout >$ROOT/samba/smbd.out 2>&1 &
SMBD_PID=$!
for i in $(seq 1 50); do
    ss -lnt 2>/dev/null | grep -q ":$PORT " && break
    sleep 0.2
done
if ! ss -lnt 2>/dev/null | grep -q ":$PORT "; then
    echo "FAIL: smbd did not start"
    tail -20 $ROOT/samba/smbd.out
    exit 1
fi

echo
echo "============================================================"
echo "  Phase 5.4 KNOWN_ISSUES on plain XFS (reference point)"
echo "============================================================"
for t in "${KNOWN_ISSUES[@]}"; do
    if timeout 60 smbtorture //127.0.0.1/$SHARE \
            -U "$USER%$PASS" -p $PORT \
            --option="torture:progress=no" \
            --option="torture:writetimeupdatedelay=1000000" \
            "$t" > "$LOGDIR/smbt-$t.log" 2>&1; then
        printf "  %-18s  PASS on XFS — smoothfs failure at this test is a SMOOTHFS BUG\n" "$t"
    else
        first_fail=$(grep -m 1 "^failure:" "$LOGDIR/smbt-$t.log" | head -1)
        printf "  %-18s  fail on XFS — Samba/Linux limitation; not a smoothfs issue  %s\n" "$t" "$first_fail"
    fi
done
echo
echo "Logs: $LOGDIR/"

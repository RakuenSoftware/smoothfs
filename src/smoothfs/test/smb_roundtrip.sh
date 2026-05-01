#!/bin/bash
# smb_roundtrip — Phase 5.1 driver.
#
# Stands up a minimal Samba instance (loopback-only, port 8445, all
# state under /tmp/smb-smoothfs/samba) serving a two-tier smoothfs
# mount, then exercises the share via smbclient and a mount.cifs
# loopback mount. Also verifies trusted.smoothfs.fileid stability
# across rename (Phase 0 contract §SMB: "rename is metadata-only at
# the smoothfs layer (does not change object_id); FileId stable
# across rename").
#
# Phase 5.1 intentionally uses stock Samba with no VFS module — the
# smoothfs VFS module is Phase 5.3. Anything that works here must
# continue to work after the VFS module is added.

set -u

ROOT=/tmp/smb-smoothfs
UUID=55555555-5555-5555-5555-555555555551
PORT=8445
SHARE=smoothfs
USER=smbtest
PASS=smbtest-phase5
CIFS_MNT=$ROOT/cifs

SMBD_PID=""

cleanup() {
    if [ -n "$SMBD_PID" ] && kill -0 "$SMBD_PID" 2>/dev/null; then
        kill -TERM "$SMBD_PID" 2>/dev/null
        # smbd forks; kill anything in the cgroup
        pkill -TERM -f "smbd.*$ROOT/samba/smb.conf" 2>/dev/null
    fi
    umount -l $CIFS_MNT 2>/dev/null || true
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

echo "=== stopping any system Samba daemons that could grab port 445 or tdb state ==="
systemctl stop smbd nmbd samba samba-ad-dc 2>/dev/null || true

echo "=== laying down smoothfs (2-tier XFS) ==="
umount -l $CIFS_MNT 2>/dev/null
umount -l $ROOT/server $ROOT/fast $ROOT/slow 2>/dev/null; sleep 0.3
rm -rf $ROOT
mkdir -p $ROOT/{fast,slow,server,cifs,samba/private}
truncate -s 1G $ROOT/fast.img $ROOT/slow.img
mkfs.xfs -q -f $ROOT/fast.img
mkfs.xfs -q -f $ROOT/slow.img
mount -o loop $ROOT/fast.img $ROOT/fast
mount -o loop $ROOT/slow.img $ROOT/slow
mount -t smoothfs -o pool=smb51,uuid=$UUID,tiers=$ROOT/fast:$ROOT/slow \
      none $ROOT/server
chmod 1777 $ROOT/server

echo "=== writing smb.conf ==="
cat > $ROOT/samba/smb.conf <<EOF
[global]
    workgroup = WORKGROUP
    server string = smoothfs phase 5.1
    server role = standalone server
    log level = 1
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

[$SHARE]
    path = $ROOT/server
    read only = no
    guest ok = no
    valid users = $USER
    force user = root
    ea support = yes
    create mask = 0644
    directory mask = 0755
EOF

echo "=== provisioning smb user $USER ==="
id $USER >/dev/null 2>&1 || useradd --no-create-home --shell /usr/sbin/nologin $USER
(
  echo "$PASS"
  echo "$PASS"
) | smbpasswd -c $ROOT/samba/smb.conf -a -s $USER >/dev/null

echo "=== starting smbd on port $PORT ==="
# Wrap smbd in `setsid` so it runs in its own session/process group. Without
# this, smbd inherits the test driver's session (because `--no-process-group`
# tells smbd not to call setsid itself), and on shutdown smbd sends SIGTERM
# to its inherited process group — which kills the test's cleanup trap before
# it can `umount -l` the smoothfs mount and its tier mounts. Result: the
# tier mounts stay busy and either the next harness or systemd's shutdown
# umount fails with "target is busy". `setsid` here gives smbd its own
# session (the `--no-process-group` flag prevents smbd from then trying to
# setsid a second time, which would fail).
setsid smbd --foreground --no-process-group --configfile=$ROOT/samba/smb.conf \
     --debug-stdout >$ROOT/samba/smbd.stdout 2>&1 &
SMBD_PID=$!
# wait for smbd to listen
listening=0
for i in $(seq 1 50); do
    if ss -lnt 2>/dev/null | grep -q ":$PORT "; then
        listening=1
        echo "  smbd up (pid $SMBD_PID, listening after ${i} ticks)"
        break
    fi
    sleep 0.2
done
if [ $listening -eq 0 ]; then
    echo "  FAIL  smbd did not start listening on $PORT"
    echo "  --- last 30 lines of smbd stdout ---"
    tail -30 $ROOT/samba/smbd.stdout 2>/dev/null
    exit 1
fi

SMBC="smbclient //127.0.0.1/$SHARE -p $PORT -U $USER%$PASS"

echo "=== smbclient: list share ==="
$SMBC -c "ls" >/tmp/smb.ls 2>&1
assert grep -q "blocks of size" /tmp/smb.ls

echo "=== smbclient: put + get + content match ==="
printf "phase51-hello-world\n" > /tmp/phase51.src
$SMBC -c "put /tmp/phase51.src hello.txt" >/tmp/smb.put 2>&1
assert grep -q "putting file" /tmp/smb.put
$SMBC -c "get hello.txt /tmp/phase51.dst" >/tmp/smb.get 2>&1
assert cmp -s /tmp/phase51.src /tmp/phase51.dst

echo "=== fileid capture before rename ==="
FID_BEFORE=$(getfattr -n trusted.smoothfs.fileid --only-values -h "$ROOT/server/hello.txt" 2>/dev/null | od -An -tx1 | tr -d ' \n')
echo "  fileid(hello.txt) = $FID_BEFORE"
assert test -n "$FID_BEFORE"

echo "=== smbclient: rename ==="
$SMBC -c "rename hello.txt renamed.txt" >/tmp/smb.rename 2>&1
assert test ! -e "$ROOT/server/hello.txt"
assert test -e "$ROOT/server/renamed.txt"

echo "=== fileid stability across rename ==="
FID_AFTER=$(getfattr -n trusted.smoothfs.fileid --only-values -h "$ROOT/server/renamed.txt" 2>/dev/null | od -An -tx1 | tr -d ' \n')
echo "  fileid(renamed.txt) = $FID_AFTER"
assert test "$FID_BEFORE" = "$FID_AFTER"

echo "=== smbclient: mkdir / put into subdir / rmdir ==="
$SMBC -c "mkdir d1; cd d1; put /tmp/phase51.src f; cd ..; rmdir d1" >/tmp/smb.dir 2>&1
assert grep -q "NT_STATUS_DIRECTORY_NOT_EMPTY" /tmp/smb.dir
$SMBC -c "deltree d1" >/dev/null 2>&1
assert test ! -e "$ROOT/server/d1"

echo "=== mount.cifs loopback ==="
mount -t cifs //127.0.0.1/$SHARE $CIFS_MNT \
      -o port=$PORT,username=$USER,password=$PASS,vers=3.0,cache=none,noperm
assert mountpoint -q $CIFS_MNT

echo "=== cifs: read back ==="
assert grep -q phase51-hello-world "$CIFS_MNT/renamed.txt"

echo "=== cifs: write and read ==="
printf "from-cifs\n" > "$CIFS_MNT/viacifs.txt"
assert grep -q from-cifs "$ROOT/server/viacifs.txt"

echo "=== cifs: rename ==="
mv "$CIFS_MNT/viacifs.txt" "$CIFS_MNT/viacifs-2.txt"
assert test ! -e "$ROOT/server/viacifs.txt"
assert test -e "$ROOT/server/viacifs-2.txt"

echo "=== cifs: fileid stable across rename via cifs client too ==="
FID_CIFS_BEFORE=$(getfattr -n trusted.smoothfs.fileid --only-values -h "$ROOT/server/viacifs-2.txt" 2>/dev/null | od -An -tx1 | tr -d ' \n')
mv "$CIFS_MNT/viacifs-2.txt" "$CIFS_MNT/viacifs-3.txt"
FID_CIFS_AFTER=$(getfattr -n trusted.smoothfs.fileid --only-values -h "$ROOT/server/viacifs-3.txt" 2>/dev/null | od -An -tx1 | tr -d ' \n')
echo "  before=$FID_CIFS_BEFORE after=$FID_CIFS_AFTER"
assert test -n "$FID_CIFS_BEFORE" -a "$FID_CIFS_BEFORE" = "$FID_CIFS_AFTER"

umount -l $CIFS_MNT

echo
if [ $rc -eq 0 ]; then
    echo "smb_roundtrip: PASS"
else
    echo "smb_roundtrip: FAIL"
fi
exit $rc

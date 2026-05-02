#!/bin/bash
# Phase 5.8.1 / 5.8.2 / 5.8.3 / 5.8.4 — verifies the smoothfs Samba
# VFS module (src/smoothfs/samba-vfs/vfs_smoothfs.c) loads into
# smbd, passes through stock Samba behaviour on a smoothfs-backed
# SMB share (5.8.1), mirrors the kernel-oplock lifecycle onto the
# smoothfs lease pin xattr (5.8.2), drives an SMB-level oplock
# break in response to a foreign modify on the smoothfs mount
# (5.8.3), and produces SMB FileId triples (dev / ino / extid) from
# the kernel's trusted.smoothfs.fileid xattr (5.8.4).
#
# Same topology as smb_roundtrip.sh (Phase 5.1), with the share
# config extended with `vfs objects = smoothfs` and `kernel oplocks
# = yes`. Phase 5.8.1 assertions: put / get / rename / fileid
# stability across rename via smbclient AND via a mount.cifs
# loopback. Phase 5.8.2 assertions: holding a cifs fd open with
# `cache=strict` (the default) causes smbd to install a kernel
# lease, which drives the smoothfs hook's setxattr on
# trusted.smoothfs.lease; closing the fd clears it. Phase 5.8.3
# assertions: while the cifs fd is held, a write from a non-smbd
# pid through the smoothfs mount fires the module's fanotify
# handler, which posts MSG_SMB_KERNEL_BREAK on the held fsp;
# Samba's oplock dispatcher tears down the kernel lease, clearing
# trusted.smoothfs.lease back to 0 without waiting for fd close.
#
# Prereq: bash src/smoothfs/samba-vfs/build.sh has produced
#   /usr/lib/<multiarch>/samba/vfs/smoothfs.so

set -u

. "$(dirname "$0")/lower_fs_lib.sh"

ROOT=/tmp/smb-vfs-smoothfs
UUID=55555555-5555-5555-5555-555555555801
PORT=8445
SHARE=smoothfs
USER=smbvfstest
PASS=smbvfstest-phase58
CIFS_MNT=$ROOT/cifs
MULTIARCH=${DEB_HOST_MULTIARCH:-}
if [ -z "$MULTIARCH" ]; then
    MULTIARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || true)
fi
VFS_MODULE=${MULTIARCH:+/usr/lib/$MULTIARCH/samba/vfs/smoothfs.so}

SMBD_PID=""

cleanup() {
    if [ -n "$SMBD_PID" ]; then
        pkill -9 -f "smbd.*$ROOT/samba/smb.conf" 2>/dev/null
    fi
    umount -l $CIFS_MNT 2>/dev/null || true
    umount -l $ROOT/server $ROOT/fast $ROOT/slow 2>/dev/null || true
    rm -rf $ROOT
}
trap cleanup EXIT

if [ -z "$VFS_MODULE" ]; then
    echo "FAIL: unable to determine Debian multiarch library path"
    exit 1
fi

if [ ! -f "$VFS_MODULE" ]; then
    echo "FAIL: $VFS_MODULE missing"
    echo "  build it first: bash src/smoothfs/samba-vfs/build.sh"
    exit 1
fi

rc=0
assert() {
    if "$@"; then
        echo "  ok    $*"
    else
        echo "  FAIL  $*"
        rc=1
    fi
}

echo "=== stopping system Samba daemons ==="
systemctl stop smbd nmbd samba samba-ad-dc 2>/dev/null || true

echo "=== laying down smoothfs (2-tier XFS) ==="
umount -l $CIFS_MNT 2>/dev/null
umount -l $ROOT/server $ROOT/fast $ROOT/slow 2>/dev/null; sleep 0.3
rm -rf $ROOT
mkdir -p $ROOT/{fast,slow,server,cifs,samba/private}
truncate -s 1G $ROOT/fast.img $ROOT/slow.img
mkfs_lower $ROOT/fast.img
mkfs_lower $ROOT/slow.img
mount -o loop $ROOT/fast.img $ROOT/fast
mount -o loop $ROOT/slow.img $ROOT/slow
mount -t smoothfs -o pool=vfs58,uuid=$UUID,tiers=$ROOT/fast:$ROOT/slow \
      none $ROOT/server
chmod 1777 $ROOT/server

echo "=== smb.conf with vfs objects = smoothfs ==="
cat > $ROOT/samba/smb.conf <<EOF
[global]
    workgroup = WORKGROUP
    server string = smoothfs phase 5.8.1
    server role = standalone server
    log level = 3 vfs:10
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
    kernel oplocks = yes

[$SHARE]
    path = $ROOT/server
    read only = no
    guest ok = no
    valid users = $USER
    force user = root
    ea support = yes
    create mask = 0644
    directory mask = 0755
    vfs objects = smoothfs
EOF

id $USER >/dev/null 2>&1 || useradd --no-create-home --shell /usr/sbin/nologin $USER
(echo "$PASS"; echo "$PASS") | smbpasswd -c $ROOT/samba/smb.conf -a -s $USER >/dev/null

echo "=== starting smbd (vfs objects = smoothfs) on port $PORT ==="
smbd --foreground --no-process-group --configfile=$ROOT/samba/smb.conf \
     --debug-stdout >$ROOT/samba/smbd.stdout 2>&1 &
SMBD_PID=$!
for i in $(seq 1 50); do
    ss -lnt 2>/dev/null | grep -q ":$PORT " && break
    sleep 0.2
done
if ! ss -lnt 2>/dev/null | grep -q ":$PORT "; then
    echo "  FAIL  smbd did not start listening"
    echo "  last 20 lines of smbd stdout:"
    tail -20 $ROOT/samba/smbd.stdout
    exit 1
fi
echo "  smbd up (pid $SMBD_PID)"

# smbd is launched with --debug-stdout, so all log output goes to
# $ROOT/samba/smbd.stdout (not the `log file =` path). Log level 3
# surfaces DBG_NOTICE. Module-loaded and fanotify-watcher-active
# notices appear only after a client connects to the share, so
# we grep AFTER the first smbclient round-trip below, not here.
SMBD_LOG="$ROOT/samba/smbd.stdout"

SMBC="smbclient //127.0.0.1/$SHARE -p $PORT -U $USER%$PASS"

echo "=== smbclient round-trip (same assertions as smb_roundtrip.sh) ==="
printf "phase58-hello\n" > /tmp/phase58.src
$SMBC -c "put /tmp/phase58.src hello.txt" >/tmp/smbvfs.put 2>&1
assert grep -q "putting file" /tmp/smbvfs.put

echo "=== module-loaded + fanotify-watcher notices (after first connect) ==="
assert grep -q "smoothfs: VFS module loaded" "$SMBD_LOG"
assert grep -q "fanotify lease-break watcher active on" "$SMBD_LOG"
$SMBC -c "get hello.txt /tmp/phase58.dst" >/dev/null 2>&1
assert cmp -s /tmp/phase58.src /tmp/phase58.dst

FID_BEFORE=$(getfattr -n trusted.smoothfs.fileid --only-values -h "$ROOT/server/hello.txt" 2>/dev/null | od -An -tx1 | tr -d ' \n')
$SMBC -c "rename hello.txt renamed.txt" >/dev/null 2>&1
assert test -e "$ROOT/server/renamed.txt"
assert test ! -e "$ROOT/server/hello.txt"
FID_AFTER=$(getfattr -n trusted.smoothfs.fileid --only-values -h "$ROOT/server/renamed.txt" 2>/dev/null | od -An -tx1 | tr -d ' \n')
assert test -n "$FID_BEFORE" -a "$FID_BEFORE" = "$FID_AFTER"

echo "=== Phase 5.8.4: file_id_create_fn hits the per-conn gen cache ==="
# The module's openat_fn caches (ino, gen) from trusted.smoothfs.fileid
# on every successful open. file_id_create_fn looks that gen back up
# and returns it as the SMB FileId extid. On the current smoothfs
# kernel, si->gen doesn't increment (gen bumps land with oid-reuse in
# a later kernel phase), so gen is 0 for every file and the observed
# behaviour matches stock Samba. What we assert here is that the hook
# ran at all: DBG_INFO fires on the cache hit with ino + gen, so the
# log proves file_id_create_fn received a matching sbuf and upgraded
# the key. Compare the logged ino against the ino half of the xattr
# (bytes 0..8, LE) to make sure the cache is keyed correctly.
# Parse trusted.smoothfs.fileid via python: bash's signed 64-bit
# arithmetic can't represent the smoothfs inode_no (MSB is always
# set — synthesised as xxh64(oid) | (1<<63)).
INO_DEC=$(getfattr -n trusted.smoothfs.fileid --only-values -h \
    "$ROOT/server/renamed.txt" 2>/dev/null \
    | python3 -c 'import sys,struct; b=sys.stdin.buffer.read()[:8]; print(struct.unpack("<Q", b)[0])')
assert test -n "$INO_DEC"
assert grep -qE "smoothfs: file_id ino=$INO_DEC " "$SMBD_LOG"

echo "=== mount.cifs loopback ==="
mount -t cifs //127.0.0.1/$SHARE $CIFS_MNT \
      -o port=$PORT,username=$USER,password=$PASS,vers=3.0,cache=none,noperm
assert mountpoint -q $CIFS_MNT
assert grep -q phase58-hello "$CIFS_MNT/renamed.txt"

printf "from-cifs\n" > "$CIFS_MNT/viacifs.txt"
assert grep -q from-cifs "$ROOT/server/viacifs.txt"
mv "$CIFS_MNT/viacifs.txt" "$CIFS_MNT/viacifs-2.txt"
assert test -e "$ROOT/server/viacifs-2.txt"

umount -l $CIFS_MNT

echo "=== Phase 5.8.2: lease pin lifecycle via mount.cifs (cache=strict) ==="
# cache=strict is the cifs.ko default and enables oplock requests from
# the client. cache=none would suppress them and the server-side
# linux_setlease call our hook piggy-backs on would never fire. Use a
# fresh file so smbd hands out a level-1/write oplock on the first
# open.
mount -t cifs //127.0.0.1/$SHARE $CIFS_MNT \
      -o port=$PORT,username=$USER,password=$PASS,vers=3.0,cache=strict,noperm
assert mountpoint -q $CIFS_MNT

LEASE_FILE_CIFS=$CIFS_MNT/leasetest.txt
LEASE_FILE_LOWER=$ROOT/server/leasetest.txt
printf "lease-me\n" > "$LEASE_FILE_CIFS"

# Hold the file open read-write so smbd grants an RWH (level-1) oplock
# and installs a kernel lease (linux_setlease(F_WRLCK)) for the
# duration. A read-only `exec 8<` would only get a level-2 oplock,
# which Samba does NOT mirror to a kernel lease — so our VFS hook
# would never fire and trusted.smoothfs.lease would stay 00. On fast
# hosts a previous write-lease (from `printf > FILE` above) is still
# being torn down when the poll starts, so a read-only fd happens to
# observe lease=01 by accident; on slow hosts (TCG-emulated arm64
# under qemu-user) that close has long since landed, the test reads
# 00, and times out.
exec 8<> "$LEASE_FILE_CIFS"

# Let smbd's tevent loop finish granting the oplock and calling
# linux_setlease. Poll rather than sleep-and-hope. The budget needs to
# cover slow hosts (e.g., TCG-emulated arm64 under qemu-user where
# smbd's tevent loop runs ~20× slower than KVM-native): 60s. On fast
# hosts this still breaks out as soon as the xattr settles, so the
# upper bound costs nothing on amd64.
for i in $(seq 1 300); do
    v=$(getfattr -n trusted.smoothfs.lease --only-values -h \
        "$LEASE_FILE_LOWER" 2>/dev/null | od -An -tx1 | tr -d ' \n')
    [ "$v" = "01" ] && break
    sleep 0.2
done
# If we timed out, dump the relevant smbd log lines into the harness
# log so the workflow artifact captures them. smbd.stdout itself is in
# /tmp/<root>/samba/ which the trap cleans up before the artifact step
# runs, so we have to surface the diagnostic state inline. Set
# `log level = 3 vfs:10` in smb.conf above to catch the lease-pin
# DBG_DEBUG line ("smoothfs: set lease pin on ..."). Three diagnostic
# windows:
#   1. Did SMBD even open the file? (open_file_ntcreate / smbd_smb2_create_send)
#   2. Did SMBD grant a kernel oplock? (linux_set_kernel_oplock /
#      "got kernel oplock")
#   3. Did the VFS hook fire? ("smoothfs:" lease/setxattr/cleared)
# Empty (1) → CIFS client open never reached smbd. Empty (2) but (1)
# present → smbd refused the oplock (kernel_oplocks support, share
# config). Empty (3) but (2) present → real VFS regression.
if [ "$v" != "01" ]; then
    echo "  (lease xattr=$v after 60s; smbd log diagnosis follows:)"
    echo "  --- (1) opens of leasetest.txt ---"
    grep -E "leasetest\.txt|smbd_smb2_create_send|open_file_ntcreate" "$SMBD_LOG" \
        2>/dev/null | sed 's/^/    /' | tail -15
    echo "  --- (2) kernel-oplock grants ---"
    grep -E "linux_set_kernel_oplock|kernel oplock|set_file_oplock" "$SMBD_LOG" \
        2>/dev/null | sed 's/^/    /' | tail -10
    echo "  --- (3) smoothfs hook activity ---"
    grep -E "smoothfs:" "$SMBD_LOG" \
        2>/dev/null | sed 's/^/    /' | tail -15
fi
assert test "$v" = "01"

echo "=== Phase 5.8.3: foreign modify triggers fanotify lease break ==="
# The module's fanotify watcher on the share mount sees FAN_MODIFY
# from any non-smbd pid and posts MSG_SMB_KERNEL_BREAK on any fsp
# holding a kernel lease on the affected file_id. Simulate the
# forced-cutover signal with a foreign write from bash (pid !=
# smbd), going through the smoothfs mount so the event carries the
# smoothfs inode's file_id. This is the same code path Phase 5.3's
# forced cutover will exercise; smoothfs_movement_cutover fires
# fsnotify(FS_MODIFY) on the smoothfs inode from the netlink caller
# (tierd), which lands in our handler identically.
sh -c "echo foreign >> '$LEASE_FILE_LOWER'"

for i in $(seq 1 25); do
    v=$(getfattr -n trusted.smoothfs.lease --only-values -h \
        "$LEASE_FILE_LOWER" 2>/dev/null | od -An -tx1 | tr -d ' \n')
    [ "$v" = "00" ] && break
    sleep 0.2
done
assert test "$v" = "00"

# The module's DBG_NOTICE proves the fanotify handler ran. Whether
# it was OUR break_kernel_oplock or the kernel's SIGIO handler that
# won the race to clear the oplock is left unchecked — both are
# legitimate paths to oplock release and both funnel through our
# linux_setlease(F_UNLCK) hook for the xattr cleanup. The real
# forced-cutover path in production only reaches us (no kernel
# lease-break is triggered by a tier swap that doesn't open a new
# fd), so the fanotify arrival is what we need to guarantee.
assert grep -q "smoothfs: fanotify event:" "$SMBD_LOG"

# Close our fd. The oplock has already been broken above, so
# linux_setlease(F_UNLCK) here is idempotent and the xattr stays
# at 0. The smoothfs xattr handler always reports
# trusted.smoothfs.lease as a 1-byte value (0 when PIN_NONE, 1 when
# PIN_LEASE) — there's no ENODATA for this name — so we compare the
# byte, not presence.
exec 8<&-

v=$(getfattr -n trusted.smoothfs.lease --only-values -h \
    "$LEASE_FILE_LOWER" 2>/dev/null | od -An -tx1 | tr -d ' \n')
assert test "$v" = "00"

umount -l $CIFS_MNT

echo
if [ $rc -eq 0 ]; then
    echo "smb_vfs_module: PASS (passthrough + lease pin + foreign-modify break + fileid cache)"
else
    echo "smb_vfs_module: FAIL"
fi
exit $rc

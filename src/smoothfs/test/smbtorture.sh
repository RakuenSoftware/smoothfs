#!/bin/bash
# smbtorture — Phase 5.4 driver.
#
# Runs a curated subset of the Samba smbtorture test catalog against a
# stock-Samba share over a smoothfs mount (the Phase 5.1 topology).
# Establishes the baseline Phase 5.4 gate: the tests in MUST_PASS below
# have to pass with zero bugs surfaced by smoothfs; tests in the
# VFS_MODULE_REQUIRED list are covered by the completed Samba VFS
# module proposal (docs/proposals/completed/smoothfs-samba-vfs-module.md).
#
# Reasoning for the curated set:
#   - Stock Samba doesn't install a smoothfs-specific VFS module yet,
#     so tests that depend on behaviour smoothfs will only expose
#     via the module (lease semantics, case-insensitive lookup, SMB
#     FileId round-trip) are out of scope until that lands.
#   - Charset / unicode tests depend on the share's and Samba's
#     charset configuration — they're about Samba itself, not
#     smoothfs. Deferred.
#   - Locking tests exercise Samba's internal share-mode DB + the
#     POSIX lock surface on the lower. Some smoothfs bugs could hide
#     there, but the LOCK1..LOCK7 set is large and out-of-scope for
#     the baseline gate.
#
# If any test in MUST_PASS fails, this script exits non-zero and the
# summary names the failures.
#
# Run as root. Samba (smbd + samba-testsuite) must be installed.

set -u

ROOT=/tmp/smbt-smoothfs
LOGDIR=/tmp/smbt-smoothfs-logs
UUID=55555555-5555-5555-5555-555555555402
PORT=8445
SHARE=smoothfs
USER=smbttest
PASS=smbttest-phase54
SMBD_PID=""

cleanup() {
    if [ -n "$SMBD_PID" ] && kill -0 "$SMBD_PID" 2>/dev/null; then
        pkill -9 -f "smbd.*$ROOT/samba/smb.conf" 2>/dev/null
        wait "$SMBD_PID" 2>/dev/null
    fi
    umount -l $ROOT/server $ROOT/fast $ROOT/slow 2>/dev/null || true
    rm -rf $ROOT
}
trap cleanup EXIT

# ---------------- test lists ----------------
#
# base.*: classic SMB1-era file-op tests; the Samba base suite catches
# most "stacked filesystem behaves wrong" bugs.
# raw.*:  low-level SMB/SMB2 path parity — reads, writes, opens.
# smb2.*: SMB2/3 variants of the same.
#
# Keep the list short enough that the whole run fits in a ~1 minute
# budget so the harness is usable as a regression gate on every
# kernel change.
# MUST_PASS: Phase 5.4 baseline. These exercise the file operations
# that a smoothfs regression would most likely break. Any failure here
# is a ship-stopper.
MUST_PASS=(
    base.tcon           # SMB1 tree connect
    base.chkpath        # path validation
    base.dir1           # directory ops
    base.rw1            # SMB1 read/write + lock
    base.rename         # SMB1 rename
    base.unlink         # SMB1 unlink
    base.fdpass         # fd passthrough across duplicated connections
    base.attr           # DOS attributes
    base.xcopy          # server-side copy (xcopy) with nested trees
    raw.close           # raw close-state semantics
    raw.unlink          # raw unlink
    raw.read            # raw read (+ lock-conflict checks)
    raw.write           # raw write (+ lock-conflict checks)
    raw.search          # raw SEARCH — Phase 5.6 restored ea list subtest
    smb2.tcon           # SMB2 tree connect
    smb2.read           # SMB2 READ op
)

# KNOWN_ISSUES: failures that smoothfs does not need to fix to ship
# the Phase 5.4 baseline. Run for visibility (the script prints when
# status changes) but do not gate exit.
#
# Triage uses the XFS baseline run in
# src/smoothfs/test/smbtorture_xfs_baseline.sh, which runs the same
# subset against a plain XFS Samba share (no smoothfs). Anything
# failing there too is a Samba/Linux limitation independent of
# smoothfs; anything that PASSES on XFS but fails on smoothfs is a
# real smoothfs bug.
#
# Samba-upstream known failures — reproduce on plain XFS too (Phase
# 5.5's smbtorture_xfs_baseline.sh) AND match entries in Samba's own
# selftest/knownfail from 4.23 upstream, so smoothfs is neither the
# regression surface nor a reasonable fix site. Paper trail:
#
#   raw.rename :: directory rename
#       Samba knownfail: ^samba4.raw.rename.*.directory rename
#       — SMB share-mode rejects renaming a dir with an open child;
#         Samba never dispatches the rename to the filesystem.
#
#   smb2.rw :: invalid
#       Samba knownfail.d/rw-invalid: samba4.smb2.rw.invalid.ad_dc_ntvfs
#       — expects NT_STATUS_DISK_FULL; needs SMB quota reporting
#         only the AD-DC fileserver target implements. Phase 7
#         (appliance integration) owns quota on our side.
#
#   smb2.getinfo :: complex
#       Samba knownfail: ^samba3.smb2.getinfo.complex
#                        ^samba4.smb2.getinfo.complex
#                        # streams on directories does not work
#       — SMB alternate data streams on directories is a Samba
#         feature gap, not a filesystem one.
#
#   smb2.setinfo :: setinfo
#       Samba knownfail: ^samba3.smb2.setinfo.setinfo
#       — also carries the change_time round-trip diff we see in
#         Phase 5.5 triage; reproducible on plain XFS with the
#         same smb.conf, tracked Samba-side.
#
# raw.search — was in this list through Phase 5.5; fixed in Phase
# 5.6 (smoothfs_listxattr passthrough) and promoted into MUST_PASS
# above.
KNOWN_ISSUES=(
    raw.rename
    smb2.rw
    smb2.getinfo
    smb2.setinfo
)

# Documented for visibility; NOT run here. These require the smoothfs
# Samba VFS module (Phase 5.3 future proposal) to behave as SMB clients
# expect — stock Samba passes them on plain XFS only because smoothfs's
# identity / lease surface isn't in the VFS pipeline yet.
VFS_MODULE_REQUIRED=(
    base.defer_open          # lease/oplock interactions
    base.charset             # CASE_SENSITIVE matrix — needs vfs_catia or smoothfs VFS
    smb2.lease               # SMB3 lease protocol
    smb2.oplock              # oplock semantics
    smb2.acls                # security descriptors, mapped via VFS
)

# ---------------- setup ----------------
echo "=== stopping system Samba daemons ==="
systemctl stop smbd nmbd samba samba-ad-dc 2>/dev/null || true

echo "=== laying down smoothfs (2-tier XFS) ==="
umount -l $ROOT/server $ROOT/fast $ROOT/slow 2>/dev/null || true
rm -rf $ROOT
rm -rf $LOGDIR
mkdir -p $LOGDIR
mkdir -p $ROOT/{fast,slow,server,samba/private}
truncate -s 1G $ROOT/fast.img $ROOT/slow.img
mkfs.xfs -q -f $ROOT/fast.img
mkfs.xfs -q -f $ROOT/slow.img
mount -o loop $ROOT/fast.img $ROOT/fast
mount -o loop $ROOT/slow.img $ROOT/slow
mount -t smoothfs -o pool=smbt54,uuid=$UUID,tiers=$ROOT/fast:$ROOT/slow \
      none $ROOT/server
chmod 1777 $ROOT/server

echo "=== writing smb.conf ==="
cat > $ROOT/samba/smb.conf <<EOF
[global]
    workgroup = WORKGROUP
    server string = smoothfs phase 5.4
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
    # smbtorture uses SMB1 transactions for some of the base suite;
    # allow them.
    client min protocol = NT1
    server min protocol = NT1

[$SHARE]
    path = $ROOT/server
    read only = no
    guest ok = no
    valid users = $USER
    force user = root
    ea support = yes
    create mask = 0644
    directory mask = 0755
    # Default ("auto") enforces byte-range lock conflicts on R/W as
    # smbtorture.raw.{read,write} expect.
    strict locking = auto
    # DOS 8.3 short-name synthesis for raw.search.one-file-search
    # which round-trips both_directory_info.short_name against
    # alt_name_info.fname. Purely a Samba-side presentation concern;
    # smoothfs doesn't care about 8.3 names.
    mangled names = yes
EOF

echo "=== provisioning smb user $USER ==="
id $USER >/dev/null 2>&1 || useradd --no-create-home --shell /usr/sbin/nologin $USER
(
  echo "$PASS"
  echo "$PASS"
) | smbpasswd -c $ROOT/samba/smb.conf -a -s $USER >/dev/null

echo "=== starting smbd on port $PORT ==="
smbd --foreground --no-process-group --configfile=$ROOT/samba/smb.conf \
     --debug-stdout >$ROOT/samba/smbd.stdout 2>&1 &
SMBD_PID=$!
listening=0
for i in $(seq 1 50); do
    if ss -lnt 2>/dev/null | grep -q ":$PORT "; then
        listening=1
        break
    fi
    sleep 0.2
done
if [ $listening -eq 0 ]; then
    echo "FAIL: smbd did not start"
    tail -30 $ROOT/samba/smbd.stdout
    exit 1
fi
echo "  smbd up (pid $SMBD_PID)"

# ---------------- run ----------------
target="//127.0.0.1/$SHARE"
run_one() {
    local test="$1"
    local log="$LOGDIR/smbt-$test.log"
    # smbtorture parses host:port in the target URL as a literal
    # hostname and fails DNS; the port goes via -p.
    if timeout 60 smbtorture "$target" \
            -U "$USER%$PASS" \
            -p $PORT \
            --option="torture:progress=no" \
            --option="torture:writetimeupdatedelay=1000000" \
            "$test" > "$log" 2>&1; then
        return 0
    fi
    return 1
}

echo
echo "============================================================"
echo "  smbtorture MUST_PASS"
echo "============================================================"
mp_pass=0
mp_fail=0
mp_failed=()
for t in "${MUST_PASS[@]}"; do
    if run_one "$t"; then
        mp_pass=$((mp_pass+1))
        echo "  PASS  $t"
    else
        mp_fail=$((mp_fail+1))
        mp_failed+=("$t")
        echo "  FAIL  $t  (log: $LOGDIR/smbt-$t.log)"
    fi
done

echo
echo "============================================================"
echo "  smbtorture KNOWN_ISSUES (informational)"
echo "============================================================"
ki_pass=0
ki_fail=0
ki_unexpected_pass=()
for t in "${KNOWN_ISSUES[@]}"; do
    if run_one "$t"; then
        ki_pass=$((ki_pass+1))
        ki_unexpected_pass+=("$t")
        echo "  PASS  $t  (was documented as KNOWN_ISSUE — consider promoting to MUST_PASS)"
    else
        ki_fail=$((ki_fail+1))
        echo "  fail  $t  (expected)"
    fi
done

echo
echo "============================================================"
echo "  summary"
echo "============================================================"
echo "MUST_PASS:                  $mp_pass passed, $mp_fail failed  (of ${#MUST_PASS[@]})"
echo "KNOWN_ISSUES:               $ki_pass unexpected-pass, $ki_fail expected-fail  (of ${#KNOWN_ISSUES[@]})"
echo "VFS_MODULE_REQUIRED (skip): ${#VFS_MODULE_REQUIRED[@]} — see smoothfs-samba-vfs-module.md"
if [ $mp_fail -gt 0 ]; then
    echo
    echo "MUST_PASS regressions:"
    for t in "${mp_failed[@]}"; do
        echo "  $t"
        tail -1 "$LOGDIR/smbt-$t.log" 2>/dev/null | sed 's/^/      /'
    done
fi

echo
if [ $mp_fail -eq 0 ]; then
    echo "smbtorture: PASS (MUST_PASS: ${#MUST_PASS[@]}/${#MUST_PASS[@]})"
    exit 0
else
    echo "smbtorture: FAIL"
    exit 1
fi

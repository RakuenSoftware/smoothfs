#!/bin/bash
# cthon04 harness for smoothfs (Phase 4.4 exit-criterion driver).
#
# Mounts two XFS-backed loopback tiers under /tmp/cthon-smoothfs, stacks
# smoothfs on top, exports over loopback NFS (v3 + v4.2), and runs the
# Connectathon test suite.
#
# Requires on the host:
#   - /opt/cthon04 with the test binaries built (make -C /opt/cthon04)
#   - nfs-kernel-server installed; /proc/fs/nfsd available
#   - xfsprogs, the smoothfs module installed to /lib/modules/$(uname -r)/extra
#
# Run as root. Any currently-loaded smoothfs module is force-reloaded so
# the run tests the freshly-installed .ko — nfsd is stopped across the
# reload because it pins the module via cached export dentries.

set -u

ROOT=/tmp/cthon-smoothfs
UUID=44444444-4444-4444-4444-444444444444

cleanup() {
    exportfs -u 127.0.0.1:$ROOT/server 2>/dev/null || true
    umount -l $ROOT/client 2>/dev/null || true
    umount -l $ROOT/server 2>/dev/null || true
    umount -l $ROOT/fast $ROOT/slow 2>/dev/null || true
    rm -rf $ROOT
}
trap cleanup EXIT

# Full teardown so nfsd releases the module, then fresh-load.
exportfs -u 127.0.0.1:$ROOT/server 2>/dev/null || true
umount -l $ROOT/client $ROOT/server $ROOT/fast $ROOT/slow 2>/dev/null || true
rm -rf $ROOT
systemctl stop nfs-server 2>/dev/null || true
rmmod smoothfs 2>/dev/null || true
modprobe smoothfs
systemctl start nfs-server

mkdir -p $ROOT/{fast,slow,server,client}
truncate -s 1G $ROOT/fast.img $ROOT/slow.img
mkfs.xfs -q -f $ROOT/fast.img >/dev/null
mkfs.xfs -q -f $ROOT/slow.img >/dev/null
mount -o loop $ROOT/fast.img $ROOT/fast
mount -o loop $ROOT/slow.img $ROOT/slow
mount -t smoothfs -o pool=cthon,uuid=$UUID,tiers=$ROOT/fast:$ROOT/slow \
    none $ROOT/server

exportfs -o rw,sync,no_root_squash,no_subtree_check,fsid=$UUID \
    127.0.0.1:$ROOT/server

rc=0
for vers in 3 4.2; do
    echo
    echo "============================================================"
    echo "  NFSv$vers"
    echo "============================================================"
    if ! mount -t nfs -o vers=$vers 127.0.0.1:$ROOT/server $ROOT/client; then
        echo "MOUNT FAILED for NFSv$vers"
        rc=1
        continue
    fi

    for suite in basic general special; do
        echo
        echo "--- cthon04/$suite (NFSv$vers) ---"
        workdir=$ROOT/client/${suite}-v${vers}
        rm -rf $workdir
        # Capture-then-tail rather than pipe directly: a `runtests | tail -30`
        # pipeline always exits 0 because tail -30 succeeds, so any segfault
        # / non-zero exit from runtests was silently masked and the harness
        # reported PASS even when individual cthon04 tests crashed.
        log=$ROOT/${suite}-v${vers}.log
        if ! (cd /opt/cthon04/$suite && NFSTESTDIR=$workdir ./runtests > "$log" 2>&1); then
            rc=1
        fi
        tail -30 "$log"
    done

    umount -l $ROOT/client
done

# Phase 4.5: connectable filehandle round-trip (name_to_handle_at ->
# open_by_handle_at with AT_HANDLE_CONNECTABLE). Separate from cthon04
# because it exercises the local exportfs path, not NFS.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -x "$SCRIPT_DIR/connectable_fh" ]; then
    echo
    echo "============================================================"
    echo "  connectable-fh round-trip (Phase 4.5)"
    echo "============================================================"
    mkdir -p $ROOT/server/connectable_test
    echo hello-connectable > $ROOT/server/connectable_test/file.txt
    if ! "$SCRIPT_DIR/connectable_fh" $ROOT/server \
            $ROOT/server/connectable_test/file.txt; then
        rc=1
    fi
fi

echo
if [ $rc -eq 0 ]; then
    echo "cthon04: PASS"
else
    echo "cthon04: FAIL"
fi
exit $rc

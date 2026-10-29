#!/bin/bash
# NFSv4 must traverse subdirectory exports on multiple anonymous SmoothFS
# mounts. All pools used to report a zero statfs fsid, so mountd could not
# identify implicit ancestor exports and mounts intermittently returned ESTALE.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"
ROOT=${SMOOTHFS_NFS_IDENTITY_ROOT:-/tmp/smoothfs-nfs-identity}
UUIDS=(00000000-0000-0000-0000-00000000f801 00000000-0000-0000-0000-00000000f802 00000000-0000-0000-0000-00000000f803)
cleanup() {
    set +e
    umount "$ROOT/client" 2>/dev/null
    for i in 0 1 2; do
        exportfs -u "127.0.0.1:$ROOT/pool$i/server/export" 2>/dev/null
        SPILL_ROOT="$ROOT/pool$i" spill_cleanup
    done
}
trap cleanup EXIT
mkdir -p "$ROOT/client"
modprobe smoothfs
systemctl start nfs-server
ids=()
for i in 0 1 2; do
    SPILL_ROOT="$ROOT/pool$i"
    SPILL_MODE=tier
    [ "$i" = 0 ] || SPILL_MODE=cache
    spill_setup_pool "nfsidentity$i" "${UUIDS[$i]}"
    mkdir "$SPILL_ROOT/server/export"
    id=$(stat -f -c %i "$SPILL_ROOT/server")
    test "$id" != 0
    for previous in "${ids[@]}"; do test "$previous" != "$id"; done
    ids+=("$id")
    sync
    umount "$SPILL_ROOT/server"
    mount -t smoothfs -o "pool=nfsidentity$i,uuid=${UUIDS[$i]},tiers=$SPILL_ROOT/fast:$SPILL_ROOT/slow,mode=$SPILL_MODE" none "$SPILL_ROOT/server"
    test "$(stat -f -c %i "$SPILL_ROOT/server")" = "$id"
    exportfs -o "rw,sync,no_root_squash,no_subtree_check,fsid=${UUIDS[$i]}" "127.0.0.1:$SPILL_ROOT/server/export"
done
for attempt in $(seq 1 120); do
    [ "$(cat /proc/fs/nfsd/v4_end_grace)" = Y ] && break
    sleep 1
done
test "$(cat /proc/fs/nfsd/v4_end_grace)" = Y
# No server restart between clients/rounds: exercise live export-cache reuse.
for round in $(seq 1 5); do
    for i in 0 1 2; do
        for version in 3 4.2; do
            timeout -k 10 60 mount -t nfs -o "vers=$version,timeo=50,retrans=3" "127.0.0.1:$ROOT/pool$i/server/export" "$ROOT/client"
            QA_PATH="$ROOT/client/payload.bin" python3 - <<'PY'
import os,pathlib
p=pathlib.Path(os.environ['QA_PATH']); data=os.urandom(16384)
with p.open('wb') as f:
    f.write(data); f.flush(); os.fsync(f.fileno())
assert p.read_bytes()==data
p.rename(p.with_name('renamed.bin')); p.with_name('renamed.bin').unlink()
PY
            umount "$ROOT/client"
        done
    done
    echo "NFS multi-pool round $round: PASS"
done
echo 'nfs_multi_pool_identity: stable distinct IDs, remounts and 30 NFS mounts: PASS'

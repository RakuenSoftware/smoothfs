#!/bin/bash
# Continuous create/unlink must not retain deleted payloads in OID writeback.
set -eu
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"
export SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-unlink-reclaim}
export SPILL_UUID=00000000-0000-0000-0000-00000000f207
trap spill_cleanup EXIT
spill_setup_pool unlinkreclaim "$SPILL_UUID"

# Write twice the entire pool capacity with a 64 MiB live set. No syncfs or
# sleep: either would drain the OID queue and hide retention during churn.
for ((i=0; i<128; i++)); do
    dd if=/dev/zero of="$SPILL_ROOT/server/file-$i" bs=1M count=16 conv=fsync status=none
    if (( i >= 4 )); then
        rm "$SPILL_ROOT/server/file-$((i-4))"
    fi
done
for ((i=124; i<128; i++)); do
    spill_assert test "$(stat -c %s "$SPILL_ROOT/server/file-$i")" = 16777216
done
spill_assert test "$(find "$SPILL_ROOT/server" -maxdepth 1 -type f | wc -l)" = 4
spill_finish unlink_reclaims_space

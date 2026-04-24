#!/bin/bash
# Crash/replay smoke harness for tier spill: create a spilled file,
# reload the module, remount, and verify the file is rediscovered.
#
# This is a pragmatic harness rather than a true crash injector. It
# still exercises the mount-time replay path that has to make spilled
# objects reachable after a module unload/reload cycle.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/tier_spill_lib.sh"

trap spill_cleanup EXIT

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool replay 00000000-0000-0000-0000-00000000f106

echo "=== fill fast tier, then create spilled file ==="
spill_fill_fast_tier
SRC=/tmp/tier-spill-replay.src
spill_make_payload "$SRC" 8
cp "$SRC" "$SPILL_ROOT/server/replay.bin"
spill_assert test -f "$SPILL_ROOT/slow/replay.bin"

echo "=== unmount, reload module, remount ==="
umount "$SPILL_ROOT/server"
rmmod smoothfs
modprobe smoothfs
mount -t smoothfs -o "pool=replay,uuid=00000000-0000-0000-0000-00000000f106,tiers=$SPILL_ROOT/fast:$SPILL_ROOT/slow" \
	none "$SPILL_ROOT/server"

spill_assert test -f "$SPILL_ROOT/server/replay.bin"
spill_assert test "$(spill_sha "$SRC")" = "$(spill_sha "$SPILL_ROOT/server/replay.bin")"

rm -f "$SRC"
spill_finish "tier_spill_crash_replay"

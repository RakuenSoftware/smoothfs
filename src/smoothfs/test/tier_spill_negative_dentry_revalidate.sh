#!/bin/bash
# Regression: an out-of-band file appearing on a lower tier must invalidate a
# previously cached negative merged dentry without drop_caches or remount.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
export SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-negative-revalidate}
. "$SCRIPT_DIR/tier_spill_lib.sh"

trap spill_cleanup EXIT

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool negrevalidate 00000000-0000-0000-0000-00000000f10e

MERGED="$SPILL_ROOT/server/appeared.txt"
SLOW="$SPILL_ROOT/slow/appeared.txt"

echo "=== cache a negative dentry in the merged mount ==="
spill_assert test ! -e "$MERGED"

echo "=== create the file directly on the slow backing tier ==="
printf 'appeared\n' > "$SLOW"
sync
spill_assert test -f "$SLOW"

echo "=== merged lookup must revalidate without drop_caches ==="
spill_assert test -f "$MERGED"
spill_assert test "$(cat "$MERGED" 2>/dev/null)" = "appeared"
spill_assert bash -c "ls '$SPILL_ROOT/server' | grep -qx appeared.txt"

spill_finish "tier_spill_negative_dentry_revalidate"

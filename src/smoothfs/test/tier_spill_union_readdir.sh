#!/bin/bash
# Union readdir regression guard: once a file spills to a colder tier,
# readdir on the smoothfs mount must still show it.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/tier_spill_lib.sh"

trap spill_cleanup EXIT

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool readdir 00000000-0000-0000-0000-00000000f105

echo "=== fill fast tier, then create spilled file ==="
spill_fill_fast_tier
echo listed > "$SPILL_ROOT/server/list-me.txt"
spill_assert test -f "$SPILL_ROOT/slow/list-me.txt"

echo "=== readdir should include spilled entry ==="
if ls -1 "$SPILL_ROOT/server" | grep -qx "list-me.txt"; then
	echo "  ok    spilled file visible in readdir"
else
	echo "  FAIL  spilled file missing from readdir"
	spill_rc=1
fi

spill_finish "tier_spill_union_readdir"

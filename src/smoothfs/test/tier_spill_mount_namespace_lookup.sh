#!/bin/bash
# Regression: lower-tier resolution must not depend on the calling task's
# mount namespace. Containers see the merged bind mount but intentionally do
# not see SmoothNAS's backing-tier mountpoints.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
export SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-mount-namespace-lookup}
. "$SCRIPT_DIR/tier_spill_lib.sh"

trap spill_cleanup EXIT

echo "=== laying down 2-tier XFS smoothfs ==="
spill_setup_pool nslookup 00000000-0000-0000-0000-00000000f10f

mkdir -p "$SPILL_ROOT/slow/library"
printf 'namespace-independent\n' > "$SPILL_ROOT/slow/library/episode.mkv"
sync

echo "=== hide lower mountpoints from a container-like mount namespace ==="
VIEW="$SPILL_ROOT/container-view"
mkdir -p "$VIEW"
if unshare --mount bash -eu -c '
	mount --bind "$1/server" "$2"
	umount -l "$1/server"
	umount -l "$1/fast"
	umount -l "$1/slow"

	# The caller can no longer resolve the host backing path by name.
	test ! -e "$1/slow/library/episode.mkv"
	# Smoothfs must still resolve through its pinned lower struct path.
	test -f "$2/library/episode.mkv"
	test "$(cat "$2/library/episode.mkv")" = namespace-independent
	ls "$2/library" | grep -qx episode.mkv
' _ "$SPILL_ROOT" "$VIEW"; then
	spill_assert true
else
	spill_assert false "merged lookup failed outside the host mount namespace"
fi

spill_finish "tier_spill_mount_namespace_lookup"

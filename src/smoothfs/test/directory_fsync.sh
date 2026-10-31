#!/bin/bash
# Namespace barriers must work for both canonical and merged directories.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"
SPILL_ROOT=${SMOOTHFS_DIR_FSYNC_ROOT:-/tmp/smoothfs-directory-fsync}
SPILL_UUID=00000000-0000-0000-0000-00000000f51c
trap spill_cleanup EXIT
spill_setup_pool directoryfsync "$SPILL_UUID"

# Seed a contributing directory on each tier, and one absent from tier 1.
mkdir -p "$SPILL_ROOT/fast/merged" "$SPILL_ROOT/slow/merged"
mkdir "$SPILL_ROOT/fast/canonical"
printf 'slow-tier entry\n' > "$SPILL_ROOT/slow/merged/slow"
python3 - "$SPILL_ROOT/server" <<'PY'
import os
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
for directory in (root, root / "canonical", root / "merged"):
    fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
        os.fdatasync(fd)
        source = directory / "barrier.tmp"
        target = directory / "barrier"
        with source.open("wb") as handle:
            handle.write(b"durable namespace entry\n")
            handle.flush()
            os.fsync(handle.fileno())
        source.rename(target)
        os.fsync(fd)
        assert target.read_bytes() == b"durable namespace entry\n"
        target.unlink()
        os.fsync(fd)
    finally:
        os.close(fd)
assert (root / "merged" / "slow").read_bytes() == b"slow-tier entry\n"
print("directory_fsync: PASS (root, canonical, merged, rename and unlink)")
PY

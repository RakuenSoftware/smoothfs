#!/bin/bash
# Base ownership, dirty-write drain, read-replica eviction, and cache-full IO.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"
SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-cache-mode}
SPILL_UUID=00000000-0000-0000-0000-00000000ca01
SPILL_MODE=cache
export SPILL_ROOT SPILL_UUID SPILL_MODE
trap spill_cleanup EXIT
spill_setup_pool cachemode "$SPILL_UUID"
SYSFS="/sys/fs/smoothfs/$SPILL_UUID"
export SYSFS
spill_assert test "$(cat "$SYSFS/storage_mode")" = cache
python3 - <<'PY'
import os, pathlib, mmap
root = pathlib.Path(os.environ['SPILL_ROOT'])
sysfs = pathlib.Path(os.environ['SYSFS'])
mount, fast, base = (root / p for p in ('server', 'fast', 'slow'))
p = mount / 'series' / 'episode.mkv'
p.parent.mkdir()
payload = b'episode data\n' * 100000
with p.open('wb', buffering=0) as f:
    f.write(payload)
    assert (base / 'series/episode.mkv').exists(), 'missing base inode'
    assert not (fast / 'series/episode.mkv').exists(), 'authoritative cache placement'
    assert p.stat().st_size == len(payload), 'staged size is invisible'
    assert p.read_bytes() == payload, 'merged read failed'
    os.fsync(f.fileno())
assert (base / 'series/episode.mkv').read_bytes() == payload
assert int((sysfs / 'range_staged_bytes').read_text()) == 0
# Multiple chunks and non-aligned reads, then disposable replica eviction.
assert p.read_bytes() == payload
with p.open('rb') as f:
    f.seek(17)
    assert f.read(80000) == payload[17:80017]
(sysfs / 'drop_read_cache').write_text('1\n')
assert p.read_bytes() == payload
# A stale named replica never overrides permanent data or resurrects unlink.
(fast / 'series').mkdir(exist_ok=True)
(fast / 'series/episode.mkv').write_bytes(b'obsolete replica')
assert p.read_bytes() == payload
(fast / 'series/episode.mkv').unlink()
# Overwrite pending bytes twice; drain accounting must reach zero.
with p.open('r+b', buffering=0) as f:
    f.write(b'FIRST')
    f.seek(0)
    f.write(b'FINAL')
    os.fsync(f.fileno())
expected = b'FINAL' + payload[5:]
assert p.read_bytes() == expected, 'read replica remained stale after write'
assert (base / 'series/episode.mkv').read_bytes() == expected
assert int((sysfs / 'range_staged_bytes').read_text()) == 0
# Truncate cannot leave a larger dirty overlay to replay later.
with p.open('r+b', buffering=0) as f:
    f.seek(1)
    f.write(b'NEW')
    f.truncate(4)
    os.fsync(f.fileno())
assert p.read_bytes() == b'FNEW'
# Shared writable mappings invalidate clean replicas.
with p.open('r+b', buffering=0) as f:
    with mmap.mmap(f.fileno(), 4) as m:
        m[:] = b'MMAP'
        m.flush()
        f.seek(0)
        f.write(b'MMAP')
        assert int((sysfs / 'range_staged_bytes').read_text()) == 0, 'mapped write was staged'
assert p.read_bytes() == b'MMAP'
# Buffered append respects staged size, not the not-yet-drained base size.
a = mount / 'append.txt'
with a.open('ab', buffering=0) as f:
    f.write(b'one')
    f.write(b'two')
    os.fsync(f.fileno())
assert a.read_bytes() == b'onetwo'
# Namespace deletions do not reveal obsolete higher-tier copies.
(fast / 'append.txt').write_bytes(b'obsolete')
a.unlink()
assert not a.exists(), 'deletion resurrected a disposable replica'
(fast / 'append.txt').unlink()
PY
# Force cache admission off using actual occupancy, then write through to base.
echo 1 > "$SYSFS/write_staging_full_pct"
spill_fill_fast_tier 32
printf 'cache full fallback\n' > "$SPILL_ROOT/server/full.txt"
spill_assert grep -qx 'cache full fallback' "$SPILL_ROOT/slow/full.txt"
spill_assert test ! -e "$SPILL_ROOT/fast/full.txt"
# Evict all clean replicas and remount: namespace/data must come entirely from base.
echo 1 > "$SYSFS/drop_read_cache"
umount "$SPILL_ROOT/server"
mount -t smoothfs -o "pool=cachemode,uuid=$SPILL_UUID,mode=cache,tiers=$SPILL_ROOT/fast:$SPILL_ROOT/slow" none "$SPILL_ROOT/server"
spill_assert grep -qx 'cache full fallback' "$SPILL_ROOT/server/full.txt"
spill_assert grep -qx 'MMAP' "$SPILL_ROOT/server/series/episode.mkv"
spill_finish cache_mode

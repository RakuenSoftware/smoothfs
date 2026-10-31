#!/bin/bash
# The presented owner must see merged entries through restrictive backing dirs.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"
SPILL_ROOT=${SMOOTHFS_READDIR_PERMISSIONS_ROOT:-/tmp/smoothfs-readdir-permissions}
SPILL_UUID=00000000-0000-0000-0000-00000000f520
trap spill_cleanup EXIT
spill_setup_pool readdirpermissions "$SPILL_UUID"
mkdir -p "$SPILL_ROOT/fast/private/merged" "$SPILL_ROOT/slow/private/merged" "$SPILL_ROOT/bind"
chmod 700 "$SPILL_ROOT/fast/private" "$SPILL_ROOT/slow/private"
printf 'fast tier data\n' > "$SPILL_ROOT/fast/private/merged/fast"
printf 'slow tier data\n' > "$SPILL_ROOT/slow/private/merged/slow"
mount --bind "$SPILL_ROOT/server/private/merged" "$SPILL_ROOT/bind"
trap 'umount "$SPILL_ROOT/bind"; spill_cleanup' EXIT
python3 - "$SPILL_ROOT" <<'PY'
from pathlib import Path
import os,subprocess,sys
root=Path(sys.argv[1]);expected={'fast','slow'}
probe=r'''
import os,sys
for path in sys.argv[1:]:
 names={e.name for e in os.scandir(path)}
 assert names=={'fast','slow'},f'UID {os.geteuid()} merged listing {names}'
 for name in names:
  with open(os.path.join(path,name)) as f:assert f.read()==name+' tier data\n'
print('PRESENTED_OWNER_MERGED_READDIR_OK',os.geteuid())
'''
for path in (root/'server/private/merged',root/'bind'):assert set(os.listdir(path))==expected
subprocess.run([sys.executable,'-c',probe,str(root/'server/private/merged'),str(root/'bind')],user=1000,group=1000,extra_groups=[],check=True)
# Privilege applies to lower I/O only; upper directory DAC must still reject
# a different caller before the filesystem iterator runs.
code='import os,sys\ntry:os.listdir(sys.argv[1])\nexcept PermissionError:sys.exit(0)\nraise RuntimeError("upper DAC bypassed")'
subprocess.run([sys.executable,'-c',code,str(root/'server/private/merged')],user=1001,group=1001,extra_groups=[],check=True)
# A broken contributing path must fail enumeration instead of returning a
# partial listing that a backup collector could mistake for a complete tree.
slow=root/'slow/private/merged';saved=root/'slow/private/saved'
slow.rename(saved);slow.symlink_to('merged')
try:
 try:os.listdir(root/'server/private/merged')
 except OSError as e:
  import errno
  assert e.errno==errno.ELOOP,e
 else:raise RuntimeError('partial listing hid a contributing-tier lookup error')
finally:
 slow.unlink();saved.rename(slow)
print('readdir_backing_permissions: PASS (owner union, bind mount, upper DAC, contributing-error propagation)')
PY

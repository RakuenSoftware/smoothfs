#!/bin/bash
# mkdir failure must release the lower parent lock on every supported kernel.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"
SPILL_ROOT=${SMOOTHFS_MKDIR_ENOSPC_ROOT:-/tmp/smoothfs-mkdir-enospc}
SPILL_UUID=00000000-0000-0000-0000-00000000f51f
trap spill_cleanup EXIT
spill_setup_pool mkdirenospc "$SPILL_UUID"
python3 - "$SPILL_ROOT" <<'PY'
import errno,os,signal,subprocess,sys
from pathlib import Path
root=Path(sys.argv[1]);server=root/'server'
(server/'fast-parent').mkdir()
for tier in ('fast','slow'):
 base=root/tier
 with (base/'fill').open('wb',buffering=0) as f:
  st=os.statvfs(base)
  try:os.posix_fallocate(f.fileno(),0,max(0,st.f_bavail*st.f_frsize-2*1024*1024))
  except OSError as e:
   if e.errno!=errno.ENOSPC:raise
  f.seek(0,os.SEEK_END)
  while True:
   try:f.write(bytes(65536))
   except OSError as e:
    if e.errno!=errno.ENOSPC:raise
    break
 # Exhaust remaining inode space too, so the lower mkdir itself must fail.
 for n in range(8192):
  try:(base/('padding-'+str(n))).mkdir()
  except OSError as e:
   if e.errno!=errno.ENOSPC:raise
   break
 else:raise RuntimeError('lower mkdir ENOSPC was not exercised')
 print('LOWER_FULL',tier,flush=True)
probe=r'''
import errno,os,sys
from pathlib import Path
p=Path(sys.argv[1])
for target,missing in [(p/'fast-parent'/'must-fail',p/'missing-after-materialization'),(p/'must-also-fail',p/'missing-after-mkdir')]:
 try:target.mkdir()
 except OSError as e:
  if e.errno!=errno.ENOSPC:raise
 else:raise RuntimeError('mkdir unexpectedly succeeded on full tiers')
 print('MKDIR_ENOSPC_RETURNED',target.name,flush=True)
 try:os.stat(missing)
 except FileNotFoundError:pass
 else:raise RuntimeError('missing path unexpectedly exists')
 print('PARENT_LOOKUP_RESPONSIVE',flush=True)
'''
child=subprocess.Popen([sys.executable,'-c',probe,str(server)],start_new_session=True)
try:code=child.wait(timeout=10)
except subprocess.TimeoutExpired:
 # A broken module can leave the probe uninterruptible. Report the exact PID;
 # the disposable test guest must be reset before treating cleanup as complete.
 os.kill(child.pid,signal.SIGKILL)
 print('mkdir_enospc: FAIL (lower parent lock leaked; probe PID '+str(child.pid)+')',flush=True)
 os._exit(1)
if code:raise RuntimeError('mkdir probe failed')
for tier in ('fast','slow'):(root/tier/'fill').unlink()
(server/'fast-parent'/'recovered').mkdir()
path=server/'fast-parent'/'recovered'/'data'
with path.open('wb') as f:f.write(b'write after freeing space\n');f.flush();os.fsync(f.fileno())
assert path.read_bytes()==b'write after freeing space\n'
print('mkdir_enospc: PASS (materialization and direct mkdir ENOSPC, responsive lookup, write recovery)')
PY

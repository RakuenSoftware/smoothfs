#!/bin/bash
# syncfs on a stacked mount must flush buffered file data on every backing FS.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/tier_spill_lib.sh"
SPILL_ROOT=${SMOOTHFS_SYNCFS_ROOT:-/tmp/smoothfs-syncfs-lower}
SPILL_UUID=00000000-0000-0000-0000-00000000f520
trap spill_cleanup EXIT
spill_setup_pool syncfs-lower "$SPILL_UUID"
python3 - "$SPILL_ROOT" <<'PY'
import ctypes,fcntl,os,struct,sys
from pathlib import Path
root=Path(sys.argv[1]);payload=b'backing filesystem sync boundary\n'*(256*1024)
paths=[root/'fast'/'direct-fast',root/'slow'/'direct-slow']
for p in paths:
 with p.open('wb') as f:f.write(payload)
with (root/'server'/'upper-write').open('wb') as f:f.write(payload)
paths.append(root/'fast'/'upper-write')
def delayed(p):
 fd=os.open(p,os.O_RDONLY)
 try:
  buf=bytearray(struct.pack('<QQIIII',0,(1<<64)-1,0,0,64,0)+bytes(56*64))
  fcntl.ioctl(fd,0xC020660B,buf,True) # FS_IOC_FIEMAP, no FIEMAP_FLAG_SYNC
  count=struct.unpack_from('<I',buf,20)[0]
  return any(struct.unpack_from('<I',buf,32+56*n+40)[0]&4 for n in range(count))
 finally:os.close(fd)
before=[delayed(p) for p in paths]
assert all(before),f'cannot establish delayed-allocation prerequisite: {before}'
libc=ctypes.CDLL(None,use_errno=True);fd=os.open(root/'server',os.O_RDONLY|os.O_DIRECTORY)
try:
 if libc.syncfs(fd):raise OSError(ctypes.get_errno(),'syncfs')
finally:os.close(fd)
after=[delayed(p) for p in paths]
assert not any(after),f'syncfs returned success with unflushed backing data: {after}'
print('syncfs_lower: PASS (both backing filesystems and upper writes flushed)')
PY

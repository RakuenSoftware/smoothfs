# NFS getattr locking and cache/tier regression validation

SmoothNAS PR 471's protocol gate passed cache_mode.sh, then self-deadlocked
on the first NFSv3 mkdir. Its stack was nfsd_create -> fh_fill_pre_attrs ->
smoothfs_getattr: nfsd held the directory i_rwsem exclusively, and getattr
tried to take that same rwsem for reading.

The fix pins a consistent backing dentry/mount pair under a separate inode
spinlock. Placement writers swap the pair under that lock and release old
references outside it. Existing inode/placement writer serialization stays
in place. The open path uses the same reference snapshot. A lazy reopen
after cutover checks destination permissions with the saved caller's
credentials; initial opens retain privileged backing IO after the upper VFS
permission check, preserving the uniform visible owner contract.

## Local reproduction and checks

Executed on .253 in disposable Debian 13 VM 9221, booted with the freshly
built 7.2.10-smoothkernel, GCC 14.2 and matching installed kernel headers.
No production mount, module or pool was modified.

- Original a7a713e module: the new 30-second NFS metadata probe failed on
  mkdir, and an nfsd task remained blocked in smoothfs_getattr with the same
  pre-operation attribute stack as CI. Reset the disposable VM to clear it.
- Fixed module: bounded mkdir, file/directory chmod, truncate, stat, rename
  and unlink probes passed over NFSv3 and NFSv4.2. Complete cthon04 suites
  passed using exactly the legacy SIGSEGV exceptions already in SmoothNAS
  CI; no additional known-failure exceptions were introduced.
- movement_getattr_cutover.sh: six independent cutovers, eight concurrent
  statters, 8,362,134 stat calls, zero ESTALE or other stat errors. All source
  copies were removed and all destination-visible checksums matched.
  The old harness immediately replanned the same switched inode, so its
  subsequent rounds returned EBUSY rather than exercising another cutover.
  The revised harness performs six real moves and verifies source cleanup.
- cache_mode.sh: base authority, staged write drain, read-replica eviction,
  cache-full fallback, truncate, mmap and namespace tests passed. Added a
  uid/gid 1000 mode-0600 owner read/write/fsync case; permanent data matched.
- movement_open_fd_cutover.sh: existing writable descriptor followed the
  destination; an unprivileged reopen of an inaccessible destination failed
  instead of using privileged credentials or falling back to stale source.
- write_staging_truncate.sh and metadata_tier_activity_gate.sh passed.
- Module build, shell syntax and diff whitespace checks passed.

Logs remain in VM 9221 at /home/debian/smoothfs-nfs-before.log and
/root/smoothfs-fixed-tests/*.log. This is targeted runtime validation, not
a hardware or long-duration soak. SmoothNAS CI must still run its complete
6.18 guest protocol and mixed-client gate against the new pinned commit.

## Full Linux 7.2 follow-up

The initial d758496 protocol/mixed-client CI run passed on Linux 6.18.
The additional native 7.2.10 VM run found two failures that CI's slower
emulated guest did not expose:

- Truncate staging called notify_change on the new XFS inode without holding
  that lower inode's i_rwsem. XFS reported xfs_break_layouts and
  xfs_assert_ilocked warnings. Hold the new lower inode lock around the
  attribute change; the upper inode remains locked by VFS as before.
- Deferred OID entries retained deleted backing files until the five-second
  writeback deadline. Fast create/unlink churn exhausted the two-tier pool
  despite a bounded live set. Remove pending entries on last-link unlink,
  releasing their path references outside the queue spinlock. Open files
  retain their own backing references and in-memory identity. Entries already
  handed to the worker are released when that worker completes.

Also removed the unbalanced path_get in both cached-iget paths. Each caller
already owns its input path and releases it; only a new inode needs an extra
owned path. Attach a lookup alias before releasing that input reference, and
remove get_parent's second dput after path_put has released its dget_parent
reference.

Rebuilt both the previous and fixed module against the installed 7.2 headers
and ran the new regression scripts in VM 9221:

- Previous d758496 module: write_staging_truncate failed on the XFS warnings;
  unlink_reclaims_space failed ENOSPC on file 25.
- Fixed module: truncate passed without new kernel warnings; 128 writes of
  16 MiB into a 1 GiB pool with a 64 MiB live set passed without syncfs/sleeps.
- Full protocol gate passed (NFSv3/v4.2 cthon04, smbtorture, spill, replay,
  truncate, unlink-reclaim, metadata activity). The same existing legacy
  cthon allowances and smb_vfs_module exclusion as hosted CI were used.
- Cache mode, open-descriptor cutover/permission rejection and six concurrent
  getattr cutovers passed again with the fixed module.
- 120-second concurrent local/NFS/SMB soak passed: 3,484 local, 3,042 NFS,
  1,879 SMB complete 16 MiB writes (8,405 total), with a bounded live set and
  no incomplete workload payloads. No filesystem cache dropping or capacity
  increase was used to make the soak pass.

SmoothNAS's harness now propagates writer I/O failures explicitly (bash's
conditional wait disables errexit inside the writer), validates workload
payload sizes and surviving names, and excludes internal placement metadata
from payload checks. Injected writer failure correctly returned nonzero.

Logs: /home/debian/smoothfs-baseline-{truncate,unlink}.log,
/home/debian/smoothfs-next-{protocol,cache,open-fd,getattr,truncate,unlink}.log,
and /home/debian/smoothfs-final-soak.log. This remains disposable-VM runtime
validation, not a long-duration or physical-hardware test.

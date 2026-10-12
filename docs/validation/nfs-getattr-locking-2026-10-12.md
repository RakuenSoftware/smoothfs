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

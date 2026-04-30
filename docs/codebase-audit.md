# smoothfs Codebase Audit and Technical Documentation

Audit date: 2026-04-30

Remediation pass 1: 2026-04-30
Remediation pass 2: 2026-04-30
Remediation pass 3: 2026-04-30
Remediation pass 4: 2026-04-30
Remediation pass 5: 2026-04-30
Remediation pass 6: 2026-04-30
Remediation pass 7: 2026-04-30
Remediation pass 8: 2026-04-30
Remediation pass 9: 2026-04-30
Remediation pass 10: 2026-04-30
Remediation pass 11: 2026-04-30
Remediation pass 12: 2026-04-30
Remediation pass 13: 2026-04-30
Remediation pass 14: 2026-04-30
Remediation pass 15: 2026-04-30

Repository: `github.com/RakuenSoftware/smoothfs`

Observed HEAD: `a80a6a4` (`Merge pull request #62 from RakuenSoftware/docs/update-active-lun-status`)

## Executive Summary

`smoothfs` is a mixed Go and Linux-kernel codebase that implements the storage
engine behind SmoothNAS file-tiering. It consists of:

- A public Go package that exposes the generic-netlink contract, event decoder,
  and managed-pool helpers.
- A Go `controlplane` package that discovers mounted pools, ingests heat
  samples, plans movement, performs copy/verify/cutover orchestration, and
  recovers interrupted movement state in SQLite.
- A Linux stacked filesystem module under `src/smoothfs`.
- A Samba VFS integration module under `src/smoothfs/samba-vfs`.
- Runtime shell/C harnesses for NFS, SMB, iSCSI, DKMS, write staging, spill,
  and recovery validation.
- Operator and support docs under `docs`.

The Go tests pass, `go vet ./...` passes, `go test -race ./...` passes for the
current test suite, and `make verify` is clean after remediation pass 15. CI now
builds the kernel module against current Debian headers through
`make kernel-build-debian`. Host-native kernel build verification still cannot
be completed on this host because the running kernel is `6.17.2-1-pve`, the
module's declared floor is 6.18, and matching kernel headers are absent.

The most important audit findings are:

- Fixed in remediation pass 1: `src/smoothfs/range_staging.c` used an obsolete `struct renamedata` shape even
  though the project floor is 6.18. This likely breaks kernel builds on the
  supported kernel matrix.
- Fixed in remediation pass 1: `src/smoothfs/file.c` leaked an SRCU read lock when direct writes are attempted
  after range staging, which can wedge future cutovers waiting in
  `synchronize_srcu`.
- Fixed in remediation pass 1: `src/smoothfs/movement.c` resolved cutover destination dentries at the
  destination tier root using only the source basename. Nested-file movement can
  fail even though the userspace worker copied to the correct relative path.
- Fixed in remediation pass 1: `controlplane.Service.Run` closed `planChan` before the planner goroutine is
  guaranteed to stop, so shutdown can panic if the planner sends concurrently.
- Fixed in remediation pass 1: `controlplane.Planner` owned an unguarded map that is modified by mount-event
  registration while the planner goroutine iterates it.
- Strengthened in remediation pass 1: the movement worker's source-race check used only mtime and size after copy.
  The worker now rehashes the source after destination verification; a fully
  atomic guarantee still needs a kernel-assisted source generation/freeze.
- Fixed in remediation pass 1: the Samba VFS package advertises `amd64 arm64` but hardcoded x86_64 paths in
  both the build script and Debian install rule.
- Fixed in remediation pass 1: the documentation said range-staging remount replay is pending, but the code
  and test harness now implement and exercise Phase 6O replay.

## Repository Map

Top-level Go package:

- `uapi.go`: shared constants and wire structs for the generic-netlink UAPI.
- `client.go`: thin generic-netlink client for commands and multicast receive.
- `events.go`: multicast event decoder for mount-ready, heat, move-state, tier
  fault, and spill events.
- `pools.go`: managed pool validation and systemd mount-unit creation/destruction.

Control plane:

- `controlplane/contract.go`: package aliases to the root Go API.
- `controlplane/service.go`: top-level orchestration, event loop, pool discovery,
  recovery, planner, subtree reconcile, and worker pool lifecycle.
- `controlplane/planner.go`: heat/policy planner that emits `MovementPlan`.
- `controlplane/worker.go`: movement executor: inspect, plan, copy, verify,
  cutover, cleanup, LUN re-pin, and DB logging.
- `controlplane/recovery.go`: startup reconciliation for interrupted movement
  states.
- `controlplane/heat.go`: EWMA heat aggregation from kernel samples.
- `controlplane/reconcile.go`: duplicate empty subtree cleanup across tiers.
- `controlplane/lun_pin.go`: `trusted.smoothfs.lun` xattr helpers.
- `controlplane/lun_plan.go`: active-LUN movement planning and quiesce/resume
  orchestration.

Kernel module:

- `src/smoothfs/module.c`: module init/exit, inode cache, filesystem
  registration, sysfs root.
- `src/smoothfs/super.c`: mount parsing, tier resolution, sysfs pool attributes,
  placement/OID maps, write-staging drains, super operations, inode allocation.
- `src/smoothfs/inode.c`: lookup, create, mkdir, link, unlink, rename, getattr,
  setattr, symlink, dentry operations.
- `src/smoothfs/file.c`: regular-file open/read/write/fsync/mmap/llseek/splice,
  direct I/O forwarding, reflink ioctls, range staging.
- `src/smoothfs/dir.c`: merged readdir cache across active tiers.
- `src/smoothfs/xattr.c`: xattr passthrough plus reserved `trusted.smoothfs.*`
  handlers.
- `src/smoothfs/export.c`: NFS export file-handle encode/decode.
- `src/smoothfs/movement.c`: kernel movement state machine and cutover barrier.
- `src/smoothfs/netlink.c`: generic-netlink family, commands, sb registry, and
  multicast events.
- `src/smoothfs/placement.c`: placement log, replay, and lower-tier scan.
- `src/smoothfs/range_staging.c`: range-staging metadata persistence and replay.
- `src/smoothfs/lower.c`: lower-file open/release and lazy fd reissue after
  cutover.
- `src/smoothfs/heat.c`: periodic kernel heat-drain work.
- `src/smoothfs/probe.c`: lower filesystem capability matrix.
- `src/smoothfs/oid.c`: UUIDv7 object-id allocation and xattr persistence.
- `src/smoothfs/compat.h`: kernel API compatibility surface and 6.18 floor.
- `src/smoothfs/uapi_smoothfs.h`: kernel/userspace UAPI header.

Packaging and integration:

- `src/smoothfs/debian`: DKMS packaging for the kernel module.
- `src/smoothfs/dkms.conf`: DKMS recipe and 6.18+ `BUILD_EXCLUSIVE_KERNEL`.
- `src/smoothfs/samba-vfs`: Samba VFS module, build script, and Debian package.
- `src/smoothfs/test`: root-only protocol and kernel-runtime harnesses.
- `.github/workflows/ci.yml`: Go-only CI (`make test`).

## Design Overview

### Execution Domains

The system has three runtime domains:

- Kernel: the `smoothfs` filesystem module presents one namespace over ordered
  lower tiers. It owns object identity, lower path resolution, xattr contracts,
  cutover atomicity, write staging, sysfs counters, and netlink events.
- Userspace control plane: the Go control plane owns policy, heat aggregation,
  movement planning, copy/verify orchestration, SQLite state, active-LUN safety,
  and startup recovery.
- Appliance integration: SmoothNAS/tierd consumes the Go APIs, owns UI/API
  policy entry points, and coordinates external protocol lifecycle such as SMB
  lease behavior and iSCSI target quiesce/resume.

### Data Placement Model

Each smoothfs pool has up to `SMOOTHFS_MAX_TIERS` (8) lower filesystems ordered
by rank, with rank 0 as fastest. Lower filesystems are mounted independently,
then smoothfs stacks over their root paths using mount options:

```text
mount -t smoothfs -o pool=<name>,uuid=<uuid>,tiers=/fast:/slow none /mnt/smoothfs/<name>
```

Each object has:

- A 16-byte UUIDv7 object id in `trusted.smoothfs.oid`.
- A generation xattr `trusted.smoothfs.gen`.
- A synthesized inode number derived from the object id.
- A current tier and intended tier in kernel memory.
- A movement state, pin state, transaction sequence, and cached relative path.

Placement is recoverable from a fastest-tier `.smoothfs/placement.log` plus a
scan of lower tiers for OID xattrs. Writeback of placement records is
asynchronous; `sync_fs` and unmount drain it.

### Kernel Mount Lifecycle

Mount flow:

1. Parse `pool`, optional `uuid`, and colon-separated `tiers`.
2. Allocate and initialize `smoothfs_sb_info`.
3. Initialize OID map, lower-inode map, SRCU cutover barrier, OID writeback, and
   placement writeback.
4. Resolve lower tier paths and probe lower filesystem capabilities.
5. Create root smoothfs inode over fastest tier root.
6. Open `.smoothfs/placement.log`.
7. Replay placement state and scan lower tiers.
8. Replay range-staging sidecar metadata.
9. Register the pool in the in-kernel sb registry.
10. Create `/sys/fs/smoothfs/<uuid>/...`.
11. Start heat drain work.
12. Emit `MOUNT_READY` over generic netlink.

Unmount flow:

1. Stop heat drain.
2. Remove sysfs pool node.
3. Unregister from netlink sb registry.
4. Drain/destroy OID writeback.
5. Drain/close placement log.
6. Destroy placement writeback.
7. Clean up SRCU, hash maps, lower paths, and `sbi`.
8. `smoothfs_kill_sb` releases placement-replay inode pins before
   `kill_anon_super`.

### Lower Filesystem Compatibility

`src/smoothfs/probe.c` accepts xfs, zfs, ext4, and btrfs by superblock magic.
The compatibility check is currently heuristic: it assigns the required
capability bitset to known filesystems rather than performing live capability
round trips. Unknown lower filesystems fail closed.

Required capabilities include trusted/user/security xattrs, POSIX ACL,
atomic rename, hardlinks, coherent mmap, direct I/O, durable fsync, sparse seek,
and inode generation.

### Netlink UAPI

Family:

- Name: `smoothfs`
- Version: `1`
- Multicast group: `events`

Commands:

- `REGISTER_POOL`: accepted as no-op. Mount options carry the lower paths.
- `POLICY_PUSH`: reserved / `-ENOSYS`.
- `MOVE_PLAN`: kernel validates object, destination tier, pin state, mmap state,
  and movement state, then marks `PLAN_ACCEPTED`.
- `TIER_DOWN`: placeholder no-op.
- `RECONCILE`: clears pool quiesce, clears mapping-quiesce flags, kicks heat
  drain.
- `QUIESCE`: sets the pool quiesce gate.
- `INSPECT`: returns object state, pin state, cutover generation, write
  sequence, rel path, and current tier path.
- `REPROBE`: reserved / `-ENOSYS`.
- `MOVE_CUTOVER`: switches the object to the intended tier after copy/verify.
- `REVOKE_MAPPINGS`: zaps writable shared mappings for an object.

Events:

- `MOUNT_READY`: pool UUID/name/fsid, tier list, spill flag.
- `HEAT_SAMPLE`: packed `smoothfs_heat_sample_record` array.
- `MOVE_STATE`: object id, new state, transaction sequence.
- `TIER_FAULT`: tier rank.
- `SPILL`: object id, source tier, destination tier, size.

The Go UAPI mirrors the C header and statically asserts
`HeatSampleRecordSize == 56`.

### Movement State Machine

Canonical states:

- `placed`
- `plan_accepted`
- `destination_reserved`
- `copy_in_progress`
- `copy_complete`
- `copy_verified`
- `cutover_in_progress`
- `switched`
- `cleanup_in_progress`
- `cleanup_complete`
- `failed`
- `stale`

Normal flow:

1. Planner emits a `MovementPlan`.
2. Worker calls kernel `MOVE_PLAN`.
3. Worker creates destination parent directories.
4. Worker records the source write sequence, copies source to destination, and
   computes SHA-256 during copy.
5. Worker hashes destination and compares it with source hash.
6. Worker checks source mtime, size, SHA-256, and write sequence after copy.
7. Worker calls kernel `MOVE_CUTOVER` with the expected write sequence.
8. Kernel switches `si->lower_path`, updates dentry lower data when possible,
   increments `cutover_gen`, records placement, emits move-state. If the write
   sequence changed after the worker's verification, cutover fails with
   `-ESTALE`.
9. Worker removes source and finalizes DB row as `placed` on destination.

Kernel movement gates:

- Pool quiesce blocks `MOVE_PLAN` and `MOVE_CUTOVER`.
- Destination tier must exist and differ from current tier.
- Object must be regular file and currently `PLACED`.
- Non-none pin blocks movement, except `PIN_LEASE` can be bypassed by
  `force=true`.
- Multi-link files block movement.
- Writable shared mmap blocks movement until admin revokes and holders unmap.
- Cutover uses SRCU to drain in-flight writes after setting
  `CUTOVER_IN_PROGRESS`.

### File Operation Semantics

Regular file operations mostly pass through to a lower `struct file`.
`smoothfs_lower_file()` lazily reopens the lower file if `cutover_gen` changed,
so open file descriptors can follow cutovers without eager reissue.

Important details:

- Direct I/O is exposed by mirroring lower `FMODE_CAN_ODIRECT`.
- `read_iter` and `write_iter` maintain heat counters.
- `mmap` rebinds `vma->vm_file` to the lower file so the lower owns page cache.
- Writable shared mmaps are tracked on the lower mapping and gate movement.
- `fallocate`, `splice`, `llseek`, and reflink ioctls forward to lower ops.
- Once range staging is active, direct I/O and mmap are refused.

### Directory Semantics

Directory iteration builds a per-open cache:

- It always reads the canonical/current tier directory first.
- It then walks other metadata-active tiers at the same relative path.
- Duplicate names are suppressed; canonical-tier entries win.
- `.` and `..` are emitted by `dir_emit_dots`.
- Seeking to offset 0 clears the directory cache.

Metadata tier activity is controlled by `metadata_active_tier_mask`. The fastest
tier bit is forced on. Inactive lower tiers are skipped for fallback lookup and
union readdir, and the kernel increments `metadata_tier_skips`.

### Xattr Contracts

Pass-through namespaces:

- `user.*`
- non-reserved `trusted.*`
- `security.*`
- POSIX ACL xattrs via ACL handlers

Reserved `trusted.smoothfs.*` names:

- `oid`: 16-byte UUIDv7 object id, served from memory when available.
- `gen`: 4-byte little-endian generation.
- `fileid`: 12-byte SMB FileId source: inode number plus generation.
- `lease`: 1-byte lease pin, toggles `PIN_LEASE`, not persisted to lower.
- `lun`: 1-byte LUN pin, toggles `PIN_LUN`, not persisted to lower.

`trusted.smoothfs.fileid` is read-only. `lease` and `lun` pins are mutually
exclusive with stronger pin states and require `CAP_SYS_ADMIN`.

### Write Staging

Write staging is controlled under `/sys/fs/smoothfs/<uuid>/`.

Main attributes:

- `write_staging_supported`
- `write_staging_enabled`
- `write_staging_full_pct`
- `staged_bytes`
- `staged_rehome_bytes`
- `range_staged_bytes`
- `range_staged_writes`
- `staged_rehomes_total`
- `staged_rehomes_pending`
- `write_staging_drainable_rehomes`
- `write_staging_drain_pressure`
- `write_staging_drainable_tier_mask`
- `oldest_staged_write_at`
- `last_drain_at`
- `last_drain_reason`
- `metadata_active_tier_mask`
- `write_staging_drain_active_tier_mask`
- `metadata_tier_skips`
- `range_staging_recovery_supported`
- `range_staging_recovered_bytes`
- `range_staging_recovered_writes`
- `range_staging_recovery_pending`
- `recovered_range_tier_mask`
- `oldest_recovered_write_at`
- `last_recovery_at`
- `last_recovery_reason`

Two staging paths exist:

- Truncate rehome: an `O_TRUNC`/size-zero setattr against an unpinned cold-tier
  regular file moves placement to fastest tier before truncation.
- Range staging: buffered non-truncating writes to unpinned cold-tier files write
  changed byte ranges into a fastest-tier sidecar, then reads merge old lower
  data plus staged ranges.

Drain is controlled by `write_staging_drain_active_tier_mask`. The fastest tier
bit is forced on. When a source tier becomes drain-active, truncate rehome drains
delete stale original files and range-staging drains copy sidecar ranges back to
the source lower file, fsync, clear in-memory staging state, and remove sidecars.

### Range-Staging Recovery

`src/smoothfs/range_staging.c` persists per-inode range metadata under:

```text
<fastest>/.smoothfs/range-<oid_hex>.meta
<fastest>/.smoothfs/range-<oid_hex>.stage
```

The `.meta` sidecar is written via `.meta.tmp`, fsync, and rename. Replay runs
after placement replay and restores range-staging state for matching OIDs
without touching the source tier. Recovered ranges drain later when the source
tier becomes drain-active.

The implementation and `write_staging_range_crash_replay.sh` indicate this is no
longer merely pending. At audit time, the operator/support docs still described
remount replay as pending.

### NFS Export

`export.c` implements:

- Non-connectable handle: `fsid | oid | gen`
- Connectable handle: `fsid | oid | gen | parent_oid`
- `fh_to_dentry`
- `fh_to_parent`
- `get_parent`

Resolution uses the in-memory OID map and `d_obtain_alias`. Anonymous dentries
are populated with `d_fsdata` from `si->lower_path` to avoid later lower-dentry
NULL dereferences.

### SMB Integration

`src/smoothfs/samba-vfs/vfs_smoothfs.c` provides a Samba VFS module with a
minimal override surface:

- `connect_fn`: detects smoothfs using `trusted.smoothfs.fileid` and sets up
  fanotify when possible.
- `linux_setlease_fn`: mirrors Samba kernel lease lifecycle into
  `trusted.smoothfs.lease`.
- `fstat_fn`: reads `trusted.smoothfs.fileid` and caches inode generation.
- `file_id_create_fn`: uses cached generation as SMB FileId extid.

Fanotify watches the share mount for `FAN_MODIFY`, filters out the current smbd
pid, finds matching fsps by file id, and posts Samba's kernel-oplock break path.

The module is intentionally pass-through for all other operations.

### iSCSI / Active LUN Movement

LUN safety is modeled with `trusted.smoothfs.lun` / `PIN_LUN`.

Normal planner movement skips pinned LUN objects. Active-LUN movement is opt-in:

1. Caller stops/drains the LUN target.
2. Caller clears `trusted.smoothfs.lun`.
3. Control plane verifies kernel pin state is `none` while DB still records
   `pin_lun`.
4. Control plane builds a `MovementPlan` with `RePinLUN`.
5. Worker moves the file.
6. Worker verifies destination placement.
7. Worker sets `trusted.smoothfs.lun` on destination.
8. Worker verifies pin visibility through kernel inspect.
9. Worker resumes the LUN target if a resumer hook was provided.

If pre-switch movement fails, the worker attempts to re-pin source and resume
the target. If post-switch LUN completion fails, it marks the DB row failed but
preserves destination tier as current.

### Control Plane Data Model

The test schema reveals the expected SQLite surface:

- `placement_domains`: placement domains by backend.
- `tier_targets`: ranked tier records with fill targets, full thresholds, and
  optional lower backing paths.
- `managed_namespaces`: namespace identity, backend, placement domain, backend
  reference.
- `smoothfs_objects`: object id, namespace, current/intended tier, movement
  state, pin state, transaction sequence, cutover generation, failure reason,
  heat EWMA, timestamps, relative path.
- `smoothfs_movement_log`: append-only movement transition log.
- `control_plane_config`: policy and interval settings.

The production migrations live outside this repository; `docs/control-plane-schema.md`
now captures the required schema contract and the migration owner should check
that contract in integration.

### Planner

The planner:

- Loads defaults from `control_plane_config`.
- Computes filesystem fill percentage by `statfs`.
- Reads placed objects for a namespace.
- Sorts by EWMA heat.
- Promotes hot objects one tier upward.
- Demotes cold objects one tier downward.
- Forces demotion from an overfull source tier.
- Skips pinned objects.
- Skips movement during min residency / cooldown unless source is overfull.
- Applies configured hysteresis around promote/demote EWMA cutoffs.
- Skips destinations at or above full threshold.

### Heat Aggregation

Kernel heat samples include:

- Object id.
- Open count delta.
- Read bytes delta.
- Write bytes delta.
- Last access timestamp.
- Sample window duration.

The Go aggregator scores each sample as:

```text
read_bytes + 2 * write_bytes + 4096 * open_count
```

It merges into `smoothfs_objects.ewma_value` using exponential half-life based
on `last_heat_sample_at`.

### Managed Pools

`pools.go` validates:

- Pool name: lowercase alnum start, then lowercase alnum, dot, underscore, or
  dash, max 63 characters.
- Tiers: non-empty, absolute directories, no newline/NUL/comma/colon, no exact
  duplicate path strings.

`CreateManagedPool`:

- Generates UUID if omitted.
- Creates mountpoint under `/mnt/smoothfs` unless overridden.
- Writes a systemd `.mount` unit under `/etc/systemd/system`.
- Runs `systemctl daemon-reload`.
- Runs `systemctl enable --now`.
- Rolls back the unit file on failure.
- Removes a newly-created empty mountpoint on failure while preserving
  mountpoints that existed before the call.

`DestroyManagedPool` disables/stops the unit, removes it if present, and reloads
systemd.

## Verification Performed

Commands run:

```bash
aimee setup
aimee index overview
aimee memory search audit documentation smoothfs codebase
go test ./...
go vet ./...
go test -race ./...
gofmt -l .
go list ./...
bash -n src/smoothfs/samba-vfs/build.sh
bash -n src/smoothfs/test/smb_vfs_module.sh
make verify
make kernel-build
dpkg-architecture -qDEB_HOST_MULTIARCH
uname -r
test -d /lib/modules/$(uname -r)/build
make -C src/smoothfs
```

Results:

- `go test ./...`: pass.
- `go vet ./...`: pass.
- `go test -race ./...`: pass for current tests.
- `gofmt -l .`: clean after remediation pass 1.
- `make verify`: pass after remediation pass 3.
- Samba VFS build/test shell syntax checks: pass.
- `dpkg-architecture -qDEB_HOST_MULTIARCH`: `x86_64-linux-gnu` on this host.
- Go packages found: root and `controlplane`.
- Kernel build attempted with `make kernel-build` but blocked before compile:
  `/lib/modules/6.17.2-1-pve/build` is missing.
- CI now includes Go formatting, vet, tests, race tests, shell syntax checks,
  and a kernel-module compile job against current Debian headers.

## Audit Findings

### Critical: Range-Staging Rename Uses the Wrong Kernel `struct renamedata`

Status in remediation pass 1: fixed by routing the range-staging sidecar rename
through `smoothfs_compat_init_renamedata()`.

Evidence:

- `src/smoothfs/compat.h` documents the 6.18 `renamedata` shape as
  `mnt_idmap`, `old_parent`, `old_dentry`, `new_parent`, `new_dentry`, and
  exposes `smoothfs_compat_init_renamedata`.
- `src/smoothfs/inode.c` uses the newer fields directly.
- `src/smoothfs/range_staging.c` initializes `.old_mnt_idmap`, `.old_dir`,
  `.new_mnt_idmap`, and `.new_dir`.

Impact:

The module likely fails to compile on the supported 6.18+ kernels as soon as
`range_staging.c` is built against the target API. CI does not catch this
because it does not build the kernel module.

Recommendation:

Use `smoothfs_compat_init_renamedata()` in `smoothfs_range_rename_meta()` or
update that initializer to the project floor's active API. Add a kernel-module
build job against the supported kernel headers.

### Critical: Direct Writes After Range Staging Leak SRCU Read Lock

Status in remediation pass 1: fixed by unlocking `cutover_srcu` before the
direct-I/O refusal returns `-EBUSY`.

Evidence:

`smoothfs_write_iter()` acquires `srcu_read_lock()` before checking whether a
range-staged inode is receiving direct I/O. The direct-write refusal returns
`-EBUSY` before `srcu_read_unlock()`.

Impact:

One rejected direct write can leave an SRCU reader registered indefinitely.
Later `MOVE_CUTOVER` waits in `synchronize_srcu()`, potentially wedging
movement for that pool.

Recommendation:

Move the direct-I/O/range-staged check before acquiring SRCU, or route that
branch through a common unlock path. Add a regression test for direct write
after range staging followed by movement/cutover.

### High: Nested-Path Cutover Resolves Destination at Tier Root

Status in remediation pass 1: fixed by resolving the destination lower dentry
from the object-relative path instead of only the basename.

Evidence:

The worker copies to `filepath.Join(p.DestLowerDir, p.RelPath)`. Kernel cutover
then uses `sbi->tiers[si->intended_tier].lower_path` and looks up only
`src_dentry->d_name` under that root.

Impact:

Movement of `a/b/file` can fail with `ENOENT` at cutover even if userspace
copied the destination correctly to `<dest>/a/b/file`. A root-level file works,
which makes this easy to miss with shallow tests.

Recommendation:

Resolve destination by the object's relative path, not by basename under the
tier root. The kernel already caches `si->rel_path`; use that to resolve parent
and basename on the intended tier.

### High: `Service.Run` Can Close `planChan` While Planner Still Sends

Status in remediation pass 1: fixed by leaving `planChan` owned by service
lifetime and relying on context cancellation for worker shutdown.

Evidence:

`Service.Run` starts the planner and workers in the same `WaitGroup`, then on
context cancellation immediately closes `s.planChan` before waiting for the
planner goroutine to exit.

Impact:

If the planner is inside `planPool` and selects the send case as the channel is
closed, shutdown can panic with "send on closed channel". Workers already listen
to `ctx.Done()`, so closing the channel is not required for shutdown.

Recommendation:

Do not close `planChan` from `Service.Run`, or split goroutine ownership so the
planner is joined before closing the channel for workers. Add a cancellation
test with a planner blocked/sending.

### High: Planner Pool Map Has a Data Race

Status in remediation pass 1: fixed by protecting `Planner.pools` with an
RWMutex and snapshotting pools before planning.

Evidence:

`Planner.RegisterPool` writes `p.pools[pool.UUID]`; `Planner.tick` iterates
`p.pools`. Registration is called from the event loop while the planner runs in
its own goroutine. `Planner` has no mutex.

Impact:

Concurrent map iteration/write can panic at runtime and is a data race. Current
race tests do not exercise this lifecycle.

Recommendation:

Protect `Planner.pools` with a mutex/RWMutex or route pool registrations through
the planner goroutine. Add a race test for concurrent mount-ready registration
and planner ticks.

### High: Movement Copy Consistency Uses Weak Source Race Detection

Status in remediation pass 1: strengthened by hashing the source again after
destination verification and before cutover. This closes same-size/same-mtime
mutations already present by verification time; a fully atomic live-write
guarantee still needs a kernel-assisted source generation or freeze barrier.

Status in remediation pass 3: fixed with a kernel-maintained per-inode
`write_seq`. `INSPECT` now reports the current write sequence, the worker
requires it to remain unchanged after copy/hash verification, and `MOVE_CUTOVER`
can reject with `-ESTALE` after draining writers if the sequence changed in the
final pre-cutover window. Data-changing paths now share the same cutover SRCU
barrier/write-sequence accounting for writes, splice writes, fallocate,
clone/remap, and truncate.

Evidence:

The worker hashes source during copy, verifies destination hash, then checks
only source mtime and size after copy.

Impact:

If a writer mutates content but restores size and mtime, or if lower timestamp
granularity hides a mutation, cutover can publish a stale copy. The kernel
drains writes only at cutover, not for the whole copy window.

Recommendation:

Use a stronger source verification strategy. Options include kernel-assisted
copy freeze, generation/lease barrier, source hash re-read after copy, or
copy-then-rehash-source and compare to original hash. The right choice depends
on expected movement cost and live-write semantics.

### Medium: Samba VFS Packaging Claims arm64 But Hardcodes x86_64 Paths

Status in remediation pass 1: fixed in the build script, Debian install rule,
and SMB VFS test harness by using Debian multiarch paths.

Status in remediation pass 4: guarded in CI with `make
samba-vfs-package-check`, which rejects fixed library-path triplets in the
Samba VFS packaging files and dry-runs the Debian install rule for both
`x86_64-linux-gnu` and `aarch64-linux-gnu`.

Evidence:

At audit time, `src/smoothfs/samba-vfs/debian/control` declared
`Architecture: amd64 arm64`. `build.sh` configured Samba with
`/usr/lib/x86_64-linux-gnu` and installed to the same x86_64 path.
`debian/rules` also installed into
`usr/lib/x86_64-linux-gnu/samba/vfs`.

Impact:

arm64 packages build incorrectly or install the module to the wrong multiarch
directory.

Recommendation:

Use `dpkg-architecture -qDEB_HOST_MULTIARCH` in both the script and Debian rules,
or restrict the package to `amd64` until multiarch output paths are implemented.

### Medium: Documentation Is Stale on Range-Staging Replay

Status in remediation pass 1: fixed in English and Dutch support matrix and
operator runbook pages.

Evidence:

`src/smoothfs/range_staging.c` implements metadata persistence and replay.
`src/smoothfs/test/write_staging_range_crash_replay.sh` validates recovery.
At audit time, `docs/smoothfs-support-matrix.md` and
`docs/smoothfs-operator-runbook.md` still stated that range-staging
remount/crash recovery remained pending.

Impact:

Operators and release gating can make the wrong decision about what is supported
or partial.

Recommendation:

Update the support matrix and runbook to describe Phase 6O recovery, including
the remaining limitations if any.

### Medium: Go Source Is Not Fully Formatted

Status in remediation pass 1: fixed; `gofmt -l .` is clean.

Evidence:

At audit time, `gofmt -l .` reported `controlplane/worker.go`.

Impact:

CI does not enforce formatting, so formatting drift can accumulate. This is not
behavioral by itself, but it reduces review clarity in a safety-sensitive file.

Recommendation:

Run `gofmt` on the file and add a formatting check to CI.

### Medium: `ErrNotLoaded` Contract Is Not Implemented

Status in remediation pass 1: fixed by wrapping failed family discovery with
`ErrNotLoaded` while preserving the original error text.

Evidence:

`client.go` documents `ErrNotLoaded` as returned by `Open` when the smoothfs
generic-netlink family is missing. `Open` currently returns a formatted
`GetFamily` error and never wraps or returns `ErrNotLoaded`.

Impact:

Callers cannot reliably branch on `errors.Is(err, ErrNotLoaded)`.

Recommendation:

Wrap the family-not-found path with `ErrNotLoaded`, preserving the original
netlink error for diagnostics.

### Medium: Planner Loads Hysteresis But Does Not Use It

Status in remediation pass 2: fixed by applying the configured hysteresis
percentage to the promote/demote EWMA cutoffs and adding planner regression
tests for marginal promotion and demotion.

Evidence:

`PlannerConfig.HysteresisPct` is loaded from `smoothfs_hysteresis_pct`, but the
planning logic uses only percentile cutoffs, residency, cooldown, pins, and fill
thresholds.

Impact:

Operators may believe a configured hysteresis value reduces movement flapping
when it currently has no effect.

Recommendation:

Either implement hysteresis in movement candidate selection or remove the
configuration until it is implemented.

### Low: Managed Pool Rollback Leaves Mountpoint Directory

Status in remediation pass 2: fixed by tracking whether `CreateManagedPool`
created the mountpoint and removing it during rollback only when it was newly
created and still empty.

Evidence:

`CreateManagedPool` creates the mountpoint before writing/enabling the systemd
unit. On failure it removes the unit file but does not remove a newly-created
mountpoint.

Impact:

Failed pool creation can leave empty directories under the mount base.

Recommendation:

Track whether the mountpoint was created by this call and remove it on failure
if it is still empty.

### Low: Kernel CI Coverage Is Missing

Status in remediation pass 2: improved by adding Makefile verification targets,
CI formatting/vet/race/script checks, and a kernel-module compile job against
current Debian headers.

Evidence:

`.github/workflows/ci.yml` runs only `make test`, and top-level `Makefile` runs
only `go test ./...`.

Impact:

Kernel compile breaks, packaging breakage, shell harness drift, and Samba VFS
build issues are not caught before merge.

Recommendation:

Add a matrix that at least compiles `src/smoothfs` against supported headers and
runs shellcheck-like static validation for scripts. Runtime protocol harnesses
can remain separate because they require privileged hosts.

## Test Surface

Go tests cover:

- Managed pool name/tier/unit rendering behavior.
- Managed pool rollback cleanup for newly-created mountpoints.
- Event decoding.
- Heat EWMA behavior.
- Planner promote/demote behavior and hysteresis gates.
- Service discovery from mount events.
- Service concurrent pool registration and lookup under the race detector.
- Service `Run` cancellation while the event receive loop is blocked.
- Worker movement and active-LUN edge cases.
- Recovery of interrupted movement states.
- LUN pin helpers and active-LUN planning.
- Subtree reconcile behavior.

Kernel/runtime harnesses cover:

- A unified privileged runner for runtime harness execution:
  `make runtime-harnesses`, with `make runtime-harnesses-list` for manifest
  validation.
- Tier-spill create, nested parent materialization, union readdir, unlink, XDEV
  rename, and replay after reload.
- Kernel movement cutover of nested file paths.
- Metadata active-tier gate.
- Write staging truncate and range behavior.
- Write-staging drain mask.
- Range-staging crash/replay.
- Range-staged direct-I/O refusal followed by movement cutover.
- NFS cthon04 and connectable file handles.
- SMB roundtrip, smbtorture subsets, XFS baseline comparison, Samba VFS module.
- SMB identity pin and fanotify lease-break helper.
- O_DIRECT conformance.
- Real-kernel netlink receive cancellation on client close.
- iSCSI roundtrip, pin behavior, and target restart.
- DKMS kernel upgrade and module signing checks.

Coverage gaps:

- Host-native kernel builds still require installed 6.18+ headers; local
  containerized Debian-header builds can use `make kernel-build-debian` when a
  Docker-compatible runtime is available.
- CI validates the runtime harness manifest with `make runtime-harnesses-list`
  but does not run privileged harnesses; capable hosts can run the core suite
  with `make runtime-harnesses`.
- Real-kernel netlink receive cancellation is covered by a runtime harness but
  is not run in CI.
- Kernel movement of nested files is covered by a runtime harness but is not
  run in CI.
- Direct I/O refusal after range staging and subsequent cutover is covered by a
  runtime harness but is not run in CI.
- Samba VFS multiarch install paths are statically validated for arm64; a full
  native arm64 Samba VFS package build still requires an arm64/cross packaging
  runner.

## Operational Notes

Supported platform according to docs:

- Debian 13.
- Kernel 6.18+.
- OpenZFS 2.4.1 when using zfs lower tiers.
- Samba `2:4.22.8+dfsg-0+deb13u1` for current packaged VFS module.
- Lower filesystems: xfs, ext4, btrfs, zfs.

Important operator controls:

- Quiesce before manual intervention; it blocks movement but not normal I/O.
- Reconcile clears quiesce, clears mapping-quiesce flags, and kicks heat drain.
- LUN movement must be target-quiesced and explicitly prepared.
- Write-staging drain masks must only include non-fast tiers after SmoothNAS has
  externally observed those backing devices active.
- Secure Boot relies on per-appliance DKMS MOK generation/enrollment.

## Recommended Remediation Order

1. Fixed in remediation pass 1: `range_staging.c` `renamedata` API mismatch. Kernel compile CI remains open.
2. Fixed in remediation pass 1: SRCU unlock leak in `smoothfs_write_iter`.
3. Fixed in remediation pass 1: nested-path destination resolution in kernel cutover.
4. Fixed in remediation pass 1: `Service.Run` shutdown channel ownership.
5. Fixed in remediation pass 1: locking for `Planner.pools`.
6. Fixed in remediation pass 3: source mutation detection now uses a kernel write-sequence cutover guard.
7. Fixed in remediation pass 1 and CI-guarded in remediation pass 4: Samba VFS
   packaging multiarch paths.
8. Fixed in remediation pass 1: range-staging recovery docs.
9. Fixed in remediation pass 1 and CI-enforced in remediation pass 2: `gofmt`.
10. Fixed in remediation pass 1: `ErrNotLoaded` is observable with `errors.Is`.
11. Fixed in remediation pass 2: planner hysteresis is implemented.
12. Fixed in remediation pass 2: managed-pool rollback removes newly-created empty mountpoints.
13. Improved in remediation pass 2 and consolidated in remediation pass 9: CI
    covers static checks and kernel-module compilation through shared Makefile
    targets.
14. Documented in remediation pass 12: the control-plane SQLite schema contract
    for downstream migrations.
15. Documented in remediation pass 13: the generic-netlink and fixed-record
    UAPI compatibility policy.
16. Documented in remediation pass 14: the movement consistency argument,
    including staged-range and existing-file-descriptor proof conditions.
17. Fixed in remediation pass 15: existing file descriptors now fail closed if
    lower-file reissue fails after cutover instead of falling back to the stale
    source lower.

## Suggested Future Documentation Additions

- A kernel build/test matrix that maps exact kernel versions to CI artifacts.
- A release checklist tying DKMS, Samba VFS, Go module, docs, and SmoothNAS/tierd
  integration versions together.

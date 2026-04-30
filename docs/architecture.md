# smoothfs architecture

This document is the architectural reference for smoothfs as implemented in this
repository. It explains the system boundary, the kernel and userspace
responsibilities, the major data structures, the normal data paths, the
movement protocol, failure behavior, and the validation model used for release
signoff.

For exact wire numbers, schema definitions, and release gates, treat these
companion documents as authoritative:

- [UAPI compatibility policy](uapi-compatibility.md)
- [Control-plane schema contract](control-plane-schema.md)
- [Movement consistency argument](movement-consistency.md)
- [Kernel build and test matrix](kernel-test-matrix.md)
- [Release checklist](release-checklist.md)
- [Operator runbook](smoothfs-operator-runbook.md)
- [Support matrix](smoothfs-support-matrix.md)

## 1. System Purpose

smoothfs is a Linux stacked filesystem and control-plane contract for
SmoothNAS tiering. It presents one namespace backed by multiple lower
filesystems, tracks each file as a stable object, observes heat, and allows
userspace to move regular-file contents between tiers without changing the path
seen by NFS, SMB, iSCSI fileio, or local clients.

The key design choice is separation of concerns:

- The kernel owns namespace resolution, object identity, lower-file forwarding,
  movement acceptance gates, cutover atomicity, and protocol-visible safety.
- The userspace control plane owns policy, heat aggregation, movement planning,
  copying, verification, recovery, and operator-visible history.
- The appliance layer owns UI/API workflows, lifecycle guardrails, service
  orchestration, package installation, and release signoff.

The kernel never decides which object should be promoted or demoted for policy
reasons. Userspace never swaps an inode's authoritative lower path directly. The
generic-netlink UAPI is the boundary between those responsibilities.

## 2. Repository Map

Top-level Go package:

- `uapi.go`: generic-netlink command, attribute, movement-state, pin-state, and
  fixed-record mirrors.
- `client.go`: generic-netlink client for commands and multicast receive.
- `events.go`: multicast event decoder for mount-ready, heat, movement, tier
  fault, and spill events.
- `pools.go`: managed-pool helpers for validating tier paths and rendering
  systemd mount units.

Control-plane package:

- `controlplane/service.go`: runtime orchestration for netlink receive, heat
  aggregation, planner, worker pool, recovery, and subtree reconcile.
- `controlplane/planner.go`: EWMA and tier-fill based movement candidate
  selection.
- `controlplane/worker.go`: copy, verify, cutover, cleanup, active-LUN re-pin,
  and failure handling.
- `controlplane/recovery.go`: startup recovery for interrupted movement rows.
- `controlplane/heat.go`: heat sample aggregation into SQLite state.
- `controlplane/lun_plan.go` and `lun_pin.go`: active-LUN preparation and pin
  helpers.
- `controlplane/reconcile.go`: operator-triggered reconcile behavior.

Kernel module:

- `src/smoothfs/module.c`: module registration and filesystem type lifecycle.
- `src/smoothfs/super.c`: mount parsing, superblock state, sysfs, write-staging
  controls, inode allocation, deferred metadata writeback, sync, and unmount.
- `src/smoothfs/inode.c`: lookup, create, link, unlink, rename, mkdir, setattr,
  getattr, and tier selection for creates.
- `src/smoothfs/file.c`: read/write forwarding, O_DIRECT, mmap, fsync, splice,
  fallocate, reflink/remap ioctls, and range-staging overlay.
- `src/smoothfs/lower.c`: lower-file open/release and per-file-descriptor
  reissue across cutover.
- `src/smoothfs/dir.c`: merged readdir cache across tiers.
- `src/smoothfs/movement.c`: kernel movement state machine and cutover.
- `src/smoothfs/netlink.c`: generic-netlink family, commands, and multicast
  events.
- `src/smoothfs/heat.c`: periodic heat sample drain.
- `src/smoothfs/placement.c`: placement log, replay, and placement writeback.
- `src/smoothfs/range_staging.c`: persisted range-staging recovery metadata.
- `src/smoothfs/oid.c`: object id and generation xattr helpers.
- `src/smoothfs/probe.c`: lower filesystem capability probing.
- `src/smoothfs/export.c`: NFS export operations.
- `src/smoothfs/xattr.c`, `acl.c`, and `lock.c`: passthrough metadata surfaces.
- `src/smoothfs/samba-vfs`: Samba integration module and packaging support.
- `src/smoothfs/test`: privileged runtime, protocol, and operational harnesses.

## 3. Deployment Topology

A normal SmoothNAS deployment has these runtime pieces:

```text
Clients and protocols
  local POSIX, NFS, SMB, iSCSI fileio
        |
        v
smoothfs mount
  one synthetic namespace, one superblock per pool
        |
        v
lower tiers
  rank 0 fastest, higher ranks colder or larger
        |
        v
userspace control plane
  netlink listener, heat aggregator, planner, worker, recovery
        |
        v
appliance services
  UI/API/CLI, policy, packaging, release validation
```

The mount syntax is:

```sh
mount -t smoothfs \
  -o pool=<name>,uuid=<uuid>,tiers=<tier0>:<tier1>[:<tierN>...] \
  none /mnt/smoothfs/<pool>
```

Tier rank is positional. The first tier is rank `0` and is considered the
fastest tier. The kernel supports up to `SMOOTHFS_MAX_TIERS` tiers, currently
`8`.

The managed-pool helper in `pools.go` renders systemd mount units with the same
mount contract. It validates pool names, absolute tier paths, duplicate tiers,
and the `tiers=` characters that the kernel parser cannot safely accept.

## 4. Core Concepts

### Pool

A pool is one mounted smoothfs superblock. Kernel pool state lives in
`struct smoothfs_sb_info` and includes:

- pool UUID and pool name
- filesystem id derived from the UUID
- tier descriptors and lower root paths
- OID and lower-inode hash tables
- per-pool inode list
- movement transaction sequence
- placement log state and async placement writeback queue
- OID xattr writeback queue
- heat drain work
- quiesce state
- write-staging, range-staging, spill, and recovery counters
- per-pool sysfs object
- cutover SRCU structure

Userspace mirrors a mounted pool as `controlplane.Pool`, with the pool UUID,
namespace id, tier topology, spill flag, and a per-pool transaction sequence.

### Tier

A tier is one already-mounted lower filesystem root. Kernel tier state is
`struct smoothfs_tier`:

- rank
- capability bitmap
- lower root `struct path`
- optional lower id
- active and pending write counters

Userspace tier state is `controlplane.TierInfo`:

- rank
- tier target id
- lower directory
- current fill percentage
- target fill percentage
- full threshold percentage

Lower filesystems must provide the required capability set documented in
`smoothfs.h` and the support matrix. The important required categories are
trusted/user/security xattrs, POSIX ACLs, atomic rename, hardlinks, coherent
mmap, direct I/O, durable fsync, sparse seek, and inode generation.

### Object

Each regular file or namespace object is tracked by a 16-byte object id. The
object id is persisted on the lower file as:

```text
trusted.smoothfs.oid
```

Generation and file-id helpers use:

```text
trusted.smoothfs.gen
trusted.smoothfs.fileid
```

The kernel embeds object state in `struct smoothfs_inode_info`, including:

- object id and generation
- current and intended tier
- movement state
- pin state
- transaction sequence
- cutover generation
- authoritative lower path
- heat counters
- write sequence
- range-staging state
- namespace-relative path cache

The control plane stores the same object identity as lowercase hex in
`smoothfs_objects.object_id`. The database row is the policy and operator
mirror, not the mechanism that makes kernel cutover atomic.

### Movement State

Movement states intentionally cross all layers:

- kernel enum in `src/smoothfs/smoothfs.h`
- Go constants in `uapi.go`
- SQLite text values in `smoothfs_objects`

The normal state progression is:

```text
placed
  -> plan_accepted
  -> destination_reserved
  -> copy_in_progress
  -> copy_complete
  -> copy_verified
  -> cutover_in_progress
  -> switched
  -> cleanup_in_progress
  -> cleanup_complete
  -> placed
```

The states are observable. The worker records transition history in
`smoothfs_movement_log`, and the kernel emits move-state events for kernel-side
transitions.

### Pin State

Pin state prevents unsafe background movement:

- `none`: normal movement is allowed if every other gate passes.
- `pin_hot` and `pin_cold`: policy pins.
- `pin_hardlink`: hardlink safety.
- `pin_lease`: SMB lease pin. Forced movement may bypass this one pin so the
  SMB integration can break leases.
- `pin_lun`: active LUN backing file. Generic planner movement is refused.

Active-LUN movement is a separate operator-gated flow described later.

## 5. Kernel Architecture

### Mount and Superblock Initialization

Mount parsing happens through the kernel `fs_context` path in `super.c`.
The mount creates one `smoothfs_sb_info`, resolves and pins lower root paths,
probes lower capabilities, initializes hash tables and workqueues, opens the
placement log, replays placement state, replays range-staging metadata, registers
the superblock for netlink lookup, exposes sysfs state, and emits a mount-ready
event.

The placement log lives on the fastest tier under:

```text
.smoothfs/placement.log
```

The fastest tier is used for metadata because it is the tier expected to stay
available for control-plane metadata under normal operation.

### Inode and Dentry Model

smoothfs is a stacked filesystem. A smoothfs inode owns a reference to the
current lower `struct path`; a smoothfs dentry stores the lower dentry in
`d_fsdata`. Operations usually resolve to the lower object, delegate to the
lower filesystem, then update smoothfs accounting.

Two hash tables make object lookup efficient:

- `oid_map`: object id to smoothfs inode, used by netlink movement commands and
  NFS file-handle resolution.
- `lower_ino_map`: `(tier, lower inode number)` to synthetic smoothfs inode
  number, used to avoid repeated xattr reads on hot lookup paths.

The per-pool inode list remains the canonical iteration surface for heat drain,
placement replay, pool sweeps, and recovery-adjacent work.

### Object Identity and Metadata Durability

New objects receive a smoothfs object id. OID xattr writes are deferred through
the OID writeback queue for create-path latency. `sync_fs`, `syncfs`, and
unmount drain deferred OID and placement writes.

If a crash happens after a lower file is created but before its OID xattr has
been flushed, the file contents are still on the lower filesystem. A future
mount may mint a new object id for that file. This preserves data while allowing
pre-sync object identity to change across crash.

Placement records are also written asynchronously. That is safe because replay
combines placement log state with lower-tier scanning and normalizes
non-terminal movement states.

### File Operations

For ordinary files, smoothfs opens the current lower file and stores a
`smoothfs_file_info` on the upper file:

- lower file pointer
- `cutover_gen` observed at open
- original open flags
- opener credentials
- reissue mutex
- optional directory cache pointer

Read and write operations call `smoothfs_lower_file()`, which lazily reopens
the lower file if the inode's `cutover_gen` has advanced. This is the mechanism
that lets an already-open descriptor follow a movement cutover.

The file operation surface includes:

- `read_iter`
- `write_iter`
- `fsync`
- `mmap`
- `llseek`
- `splice_read`
- `splice_write`
- `fallocate`
- `remap_file_range`
- `FICLONE` and `FICLONERANGE`
- passthrough ioctls

Data-changing paths participate in movement safety by entering the cutover SRCU
read side and incrementing `write_seq` after successful data changes.

### O_DIRECT

smoothfs mirrors lower direct-I/O capability to the upper file on open. Direct
I/O is forwarded through lower `read_iter` and `write_iter`. If an inode has
active range-staged bytes, direct reads and writes return `-EBUSY` because the
direct path cannot merge staged ranges with authoritative lower bytes.

### mmap

smoothfs rebinds mmap VMAs to the lower file with `vma_set_file` and then calls
the lower `vfs_mmap`. The smoothfs address-space operations are intentionally
empty; pages belong to the lower mapping.

This design means writable shared mappings are visible through the lower
mapping. Movement checks `mapping_writably_mapped()` on the lower inode to
block unsafe movement. The `REVOKE_MAPPINGS` command can zap shared mappings
and mark the inode mapping-quiesced; new writable shared mmaps are refused while
that flag is set.

### Directory View

Directory iteration builds a merged directory cache:

1. Collect entries from the inode's current authoritative tier.
2. Walk other tiers when their metadata tier is active.
3. Add entries not already present.
4. Prefer canonical-tier names on duplicates.

This makes spill and recovery states visible without requiring the lower tiers
to be perfectly synchronized at every instant.

### xattrs, ACLs, Locks, and Metadata

xattrs, POSIX ACLs, and locks pass through to the current lower object. The
support matrix requires lower filesystems to support the xattr and ACL surfaces
needed by SmoothNAS protocols.

Metadata operations such as create, unlink, rename, link, mkdir, rmdir, setattr,
and getattr live in `inode.c`. The code tracks the object placement and updates
placement records where needed.

### NFS Export

The NFS export surface uses stable object identity and generation information.
The important invariant is that file handles must resolve by object identity
rather than by lower path alone, because the lower path can move between tiers.

### SMB Integration

The Samba VFS module is separate from the kernel core. Its job is to integrate
SMB lease and file-id behavior with smoothfs:

- lease pins map to `trusted.smoothfs.lease` and `pin_lease`
- fanotify-driven lease-break behavior lets forced movement notify SMB clients
- stable file-id behavior is layered over smoothfs object metadata

Normal movement will not bypass active pins. The forced path exists specifically
for the SMB lease-break workflow and is narrower than general pin bypass.

### iSCSI and Active LUNs

LIO fileio opens LUN backing files through smoothfs. O_DIRECT support exists for
that path. A LUN backing file is protected by `pin_lun`, represented by
`trusted.smoothfs.lun` at the lower file and mirrored through kernel inspect and
control-plane state.

Background movement does not move `pin_lun` files. The active-LUN movement flow
requires target quiesce, source unpin, verified movement, destination re-pin,
destination inspect, and target resume.

## 6. Generic-Netlink UAPI

The generic-netlink family is named:

```text
smoothfs
```

The multicast group is:

```text
events
```

The Go package mirrors the UAPI in `uapi.go` and implements a client in
`client.go`. The compatibility rules are documented in
[uapi-compatibility.md](uapi-compatibility.md).

Commands include:

- `REGISTER_POOL`: accepted as a no-op. Mount options carry tier paths.
- `POLICY_PUSH`: reserved and currently not implemented.
- `MOVE_PLAN`: prepare kernel movement state.
- `TIER_DOWN`: reserved placeholder.
- `RECONCILE`: clear quiesce and mapping-quiesce state and kick heat drain.
- `QUIESCE`: block new movement transitions.
- `INSPECT`: return object placement, movement, pin, write sequence, rel path,
  current lower path, and range-staging state.
- `REPROBE`: reserved and currently not implemented.
- `MOVE_CUTOVER`: commit movement after userspace copy and verification.
- `REVOKE_MAPPINGS`: revoke writable shared mappings for one object.

Events include:

- `MOUNT_READY`: pool UUID, name, filesystem id, tiers, and spill state.
- `HEAT_SAMPLE`: packed heat sample records.
- `MOVE_STATE`: object id, movement state, and transaction sequence.
- `TIER_FAULT`: pool and tier rank.
- `SPILL`: object id, source tier, destination tier, object size, and spill
  state.

Unknown future attributes and events are ignored by userspace decoders where
safe. Numeric UAPI values are append-only.

## 7. Control-Plane Architecture

The control plane is the policy and orchestration side of smoothfs. It is a Go
package intended to be wired into `tierd` or another appliance service.

### Service Runtime

`controlplane.Service` wires together:

- netlink client and receive loop
- heat aggregator
- planner
- worker pool
- startup recovery
- subtree reconcile loop
- registered pool map
- optional LUN target resumer

On startup, `Service.Run` first calls `Recover`, then starts the netlink event
loop, planner loop, subtree reconcile loop, and worker goroutines. The service
closes the netlink client on context cancellation and waits for goroutines to
exit.

### Mount Discovery

The kernel emits `MOUNT_READY` when a pool is mounted. The service decodes that
event, discovers the namespace and tier targets from SQLite, fills missing lower
paths from the event tier list, registers the pool with the planner, and keeps a
runtime pool map keyed by UUID.

Production migrations must keep the schema contract in
[control-plane-schema.md](control-plane-schema.md) compatible with that
discovery path.

### Heat Aggregation

The kernel periodically drains per-inode counters into `HEAT_SAMPLE` events.
Each record contains:

- object id
- open count delta
- read byte delta
- write byte delta
- last access time
- sample window

The control-plane heat aggregator applies those samples to
`smoothfs_objects.ewma_value` using the configured half-life. The planner reads
the EWMA values later; heat application itself does not move data.

### Planner

The planner runs periodically. For each registered pool, it:

1. Refreshes tier fill percentage from lower filesystem stats.
2. Reads `placed` objects for the pool namespace.
3. Sorts objects by EWMA heat.
4. Calculates promote and demote gates from configured percentiles.
5. Applies hysteresis, minimum residency, and cooldown gates.
6. Skips pinned objects.
7. Skips destination tiers at or above their full threshold.
8. Emits a `MovementPlan` to the worker channel.

Promotion moves toward lower rank numbers. Demotion moves toward higher rank
numbers. A source tier over its full threshold can force demotion behavior even
when heat alone would not.

### Worker

The worker is the userspace half of movement. It is deliberately conservative.
For each plan, it:

1. Calls kernel `INSPECT`.
2. Refuses active LUN objects unless the plan is an explicit LUN movement.
3. Refuses range-staged objects for direct-lower copy.
4. Checks kernel placement, pin, rel path, and destination validity.
5. Calls `MOVE_PLAN`.
6. Creates the destination parent.
7. Copies source lower bytes to the destination lower path.
8. Verifies destination checksum.
9. Rechecks source size, mtime, source checksum, kernel write sequence, and
   range-staged state.
10. Calls `MOVE_CUTOVER`, including the expected write sequence when available.
11. Performs active-LUN destination re-pin and target resume when required.
12. Removes the old source and finalizes the database row.

Every material state transition is recorded in `smoothfs_movement_log`.

### Recovery

Startup recovery scans `smoothfs_objects` rows whose state is not terminal.
Recovery is based on whether cutover had become authoritative:

- Pre-cutover states roll back to `placed` on `current_tier_id`.
- Post-cutover states move forward to `placed` on `intended_tier_id`.
- Failed active-LUN rows preserve destination placement when a destination was
  already authoritative.
- LUN pin repair uses `rel_path` and the final tier lower path.

This mirrors the placement-log normalization in the kernel. The kernel and
control plane can both recover from interrupted movement, but they recover
different layers of state.

## 8. Main Data Paths

### Mount and Discovery

```text
systemd or operator mounts smoothfs
        |
kernel parses pool uuid, name, and tier roots
        |
kernel probes lower capabilities and replays placement metadata
        |
kernel emits MOUNT_READY
        |
control plane maps pool UUID to namespace and tier targets
        |
planner begins considering objects in that namespace
```

Mount succeeds only after the kernel can resolve lower tier roots and required
capabilities. Userspace pool registration is a policy step after the kernel has
already created the filesystem.

### Create and Spill

On create, the kernel chooses a lower tier. It prefers the fastest tier when
write staging is enabled and the fastest tier is not near the configured full
percentage. Otherwise it considers active and pending write load and can spill
to another tier.

The kernel then creates the lower object, assigns or persists object identity,
tracks placement, and emits spill observability when a create lands away from
the fastest tier.

### Read

For an ordinary non-range-staged file:

```text
upper read_iter
  -> smoothfs_lower_file()
  -> lower read_iter
  -> update smoothfs read counters
```

If range-staged bytes exist, buffered reads use the range-stage overlay path:

```text
read lower bytes
  -> overlay staged byte ranges from sidecar
  -> return merged view
```

Direct reads are refused while range staging is active.

### Write

For ordinary writes:

```text
enter cutover SRCU read section
  -> reopen lower if cutover_gen changed
  -> write lower file or stage range
  -> leave SRCU read section
  -> increment write_seq on success
  -> update heat/write counters
```

The SRCU read section lets cutover wait for in-flight writes without charging
every steady-state write with a heavy per-inode lock.

### Movement

Normal movement is split across userspace and kernel:

```text
planner
  -> worker
  -> INSPECT
  -> MOVE_PLAN
  -> userspace lower-to-lower copy
  -> userspace verification
  -> MOVE_CUTOVER
  -> source cleanup
```

The userspace copy is intentionally outside the kernel. The kernel only accepts
the plan and commits the final lower-path swap. That keeps the kernel movement
primitive small and lets userspace own retry, checksumming, logging, and policy.

### Cutover

Kernel cutover is the atomic visibility point:

1. Validate pool, object, movement state, transaction sequence, and pin/mapping
   gates.
2. Set `cutover_in_progress`.
3. Drain in-flight writes with `synchronize_srcu`.
4. Revalidate state and optional write sequence.
5. Resolve the destination by namespace-relative path on the intended tier.
6. Swap `si->lower_path`.
7. Update the smoothfs dentry lower pointer.
8. Set `current_tier = intended_tier`.
9. Increment `cutover_gen`.
10. Wake stalled writers.
11. Emit movement state and placement records.

Existing file descriptors observe the new `cutover_gen` on their next operation
and reopen the destination lower file. If reopen fails, the operation returns
that error rather than falling back to the stale source lower.

### Range-Staged Movement

Range staging creates a merged view: authoritative lower bytes plus staged byte
ranges stored on the fastest tier. Direct lower-to-lower copy cannot see that
merged view.

The in-repository worker therefore refuses direct-lower movement when
`INSPECT` reports `range_staged`, and rechecks after copy before cutover. A
custom integration may still copy from the smoothfs mount path to capture the
merged view, which is what the runtime harness
`write_staging_direct_cutover.sh` exercises.

### Active-LUN Movement

LUN movement is excluded from generic background planning. The active-LUN flow
is:

1. Stop or drain the owning target.
2. Clear the source LUN pin.
3. Verify kernel inspect reports `pin_state = none` while the database row still
   records `pin_lun`.
4. Build a movement plan with `RePinLUN`.
5. Copy, verify, and cut over normally.
6. Verify destination placement.
7. Set the destination LUN xattr.
8. Verify destination pin visibility.
9. Resume the target.

If failure occurs before cutover, the worker attempts to re-pin the source and
resume the target. If failure occurs after cutover, the row is marked failed
while preserving destination placement.

## 9. Write Staging and Range Staging

Write staging is controlled through per-pool sysfs files under:

```text
/sys/fs/smoothfs/<pool-uuid>/
```

Important controls and counters include:

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
- `write_staging_drain_active_tier_mask`
- range-staging recovery counters

Truncate rehome moves a cold-tier object to the fastest tier for subsequent
writes when write staging is enabled and safe. Range staging stores individual
buffered write ranges in a sidecar and overlays those ranges during buffered
reads.

Drain is operator or appliance mediated. The drain-active tier mask is intended
to be set only after SmoothNAS has externally observed that the backing devices
for those tiers are active. Draining copies staged ranges back to the
authoritative lower file, fsyncs, clears sidecars, updates counters, and clears
recovery metadata.

Range-staging metadata is persisted so remount can recover staged ranges after a
crash. Recovery counters make that state visible until drain completes.

## 10. Observability

Kernel observability:

- generic-netlink multicast events
- per-pool sysfs counters and controls
- placement log
- runtime harnesses

Control-plane observability:

- `smoothfs_objects` current placement and movement state
- `smoothfs_movement_log` transition history
- heat EWMA values
- policy rows in `control_plane_config`
- movement failure reasons

Operator observability:

- quiesce and reconcile controls
- movement logs in appliance UI/API/CLI
- runtime harness artifacts
- release checklist artifacts

The preferred diagnostic path is to correlate:

1. Kernel `INSPECT` for live object state.
2. `smoothfs_objects` for control-plane mirrored state.
3. `smoothfs_movement_log` for attempted transitions.
4. sysfs counters for staging/spill/recovery pressure.
5. runtime harness logs for environment-level validation.

## 11. Safety and Consistency Model

The movement proof rests on five mechanisms:

- Kernel movement gates refuse unsafe objects before planning.
- Userspace verifies copied bytes and source stability before cutover.
- The kernel drains in-flight data changes during cutover.
- `write_seq` catches successful data changes that userspace cannot infer from
  mtime and size alone.
- Existing file descriptors reissue to the new lower path after cutover.

Important refusal gates include:

- pool quiesced
- destination tier invalid or same as current tier
- movement state not eligible
- object not regular where regular-file movement is required
- pin state not allowed
- writable shared mapping active
- transaction sequence mismatch
- expected write sequence mismatch
- active range-staged bytes for the direct-lower worker

The design fails closed. If the kernel cannot resolve the destination, if a
write sequence changed, if destination reopen fails on an existing fd, or if a
required LUN pin cannot be verified, the system reports an error instead of
serving stale source data as if movement had succeeded.

## 12. Failure and Restart Behavior

There are two recovery layers:

- Kernel placement replay reconstructs object placement from the placement log
  and lower-tier state.
- Control-plane recovery reconciles SQLite movement rows on service startup.

Pre-cutover failures leave the source authoritative. Post-cutover failures
preserve destination authority. This distinction is the key recovery rule across
both layers.

Deferred metadata writes are drained on `sync_fs` and unmount. Lost placement
records are recoverable because replay also inspects lower tier contents. Lost
pre-sync OID xattrs can cause object identity to be re-minted after crash, but
file contents remain on the lower filesystem.

Quiesce blocks movement transitions but not normal I/O. Reconcile clears
quiesce, clears mapping-quiesce flags, and kicks heat drain.

## 13. Security, Privilege, and Lifecycle

smoothfs depends on privileged kernel and filesystem features:

- root or equivalent privileges to mount and manage the kernel module
- trusted xattrs on lower filesystems
- generic-netlink admin operations for movement commands
- sysfs writes for staging controls
- module signing for Secure Boot deployments
- DKMS or appliance build integration for kernel upgrades

The DKMS packaging and MOK helper scripts support module signing. The ops
runtime suite validates kernel upgrade and module signing behavior. Production
release signoff must validate the actual SmoothNAS kernel and package set, not
only GitHub-hosted compile jobs.

## 14. Validation Model

Default CI covers:

- formatting
- Go vet
- Go tests
- Go race tests
- shell syntax
- Samba VFS packaging static checks
- runtime harness manifest validation
- kernel module compile against current Debian headers

Privileged runtime behavior is validated by:

- `make runtime-harnesses`
- `SMOOTHFS_RUNTIME_SUITE=protocol make runtime-harnesses`
- `SMOOTHFS_RUNTIME_SUITE=ops make runtime-harnesses`
- the manual GitHub Actions workflow `Privileged runtime harnesses` on a
  labeled self-hosted runner

Runtime suites cover smoothfs-only movement and staging behavior, NFS, SMB,
iSCSI, DKMS upgrade behavior, and module signing. Hosted CI intentionally does
not run privileged mounts or protocol services.

## 15. Extension Boundaries

When changing smoothfs, keep these boundaries intact:

- Add UAPI fields append-only and update C and Go mirrors in the same change.
- Keep policy decisions in userspace unless a kernel safety invariant requires
  enforcement.
- Keep cutover atomicity in the kernel.
- Keep copying and verification in userspace unless a future kernel copy
  primitive has a separate proof.
- Do not bypass pin states except through documented, narrow flows.
- Treat `rel_path` as load-bearing for nested movement, recovery pin repair, and
  cutover destination resolution.
- Treat `write_seq` as load-bearing for movement race detection.
- Treat runtime harnesses as release gates for behavior that requires real
  mounts, protocol services, or DKMS state.

## 16. Read-Next Index

Use this document for the architecture-level map. Use the documents below for
contracts and operational detail:

- [Control-plane schema](control-plane-schema.md): required SQLite tables,
  columns, indexes, and recovery semantics.
- [UAPI compatibility](uapi-compatibility.md): command ids, attribute ids,
  event contracts, and compatibility rules.
- [Movement consistency](movement-consistency.md): proof conditions for
  successful movement and explicit exclusions.
- [Kernel test matrix](kernel-test-matrix.md): required compile, runtime, and
  protocol validation.
- [Release checklist](release-checklist.md): release signoff steps.
- [Operator runbook](smoothfs-operator-runbook.md): operational commands and
  triage surfaces.
- [Support matrix](smoothfs-support-matrix.md): supported kernel, Samba,
  OpenZFS, and lower filesystem versions.
- [Runtime deep dive](../src/README.md): implementation-focused runtime summary.

# Proposal: smoothfs — Phase 0 Contract and Conformance Spec

**Status:** Complete
**Parent:** [`smoothfs-stacked-tiering.md`](./smoothfs-stacked-tiering.md)

---

## Purpose

This document is the **Phase 0 deliverable** required by `smoothfs-stacked-tiering.md` §Phase 0 Contract (lines 565–730). It pins the semantics that Phase 1 kernel implementation must conform to, before any kernel C is written. It covers §0.1–0.10 of the parent proposal, in the same numbering.

It is a **correctness contract**, not a design exploration. Each subsection gives one of three answers per case:

- **Supported** — Phase 1+ must implement the stated behaviour.
- **Forbidden with explicit errno** — the syscall must fail with the named errno; no silent fallback.
- **Deferred to phase N** — out of scope for v1; named phase is on the hook.

Where the parent proposal said "Phase 0 must answer X", this document gives the answer. Where it said "the working assumption is Y", this document either ratifies or replaces Y with reasoning.

---

## Codebase Baseline

This contract is built against `main` at `e50c51a`. Concrete facts the contract leans on:

- **Object key today:** inode (`uint64`), big-endian, 8 bytes. `tierd/internal/tiering/meta/record.go:96–100` (`InodeKey`).
- **On-disk meta record today:** fixed 32 bytes, `version | pin_state | tier_idx | namespace_id | heat_counter | last_access_ns | reserved`. `tierd/internal/tiering/meta/record.go:33–71`. **Per-tier-local**, stored under `.tierd-meta/objects/{shard}/` on each tier's backing mount (`tierd/internal/tiering/meta/store.go:103–138`).
- **`heat_counter` and `last_access_ns` are placeholders, currently zero**, and explicitly ignored by the cache (`tierd/internal/tiering/meta/cache.go:84–88`, `record.go:41–42`).
- **zfsmgd movement-log states already exist** (`tierd/internal/db/zfs_managed.go:265–270`): `copy_in_progress`, `copy_complete`, `switched`, `cleanup_complete`, `failed`. Crash recovery for each non-terminal state is enumerated in `tierd/internal/tiering/zfsmgd/recovery_test.go:127–445`.
- **mdadm movement log** (`tierd/internal/db/migrations.go:534–545`) has a single default state `'copy_in_progress'` — **less rigorous than zfsmgd** and a Phase 0.8 follow-up.
- **`placement_intents` already has an `object_id TEXT` column** (migration 31, `migrations.go:399–410`). The column exists; the format and lifecycle do not. This contract pins both.
- **`managed_objects` and `movement_jobs` were dropped in migration 52** (`migrations.go:588–589`). Per-file metadata moved to `PoolMetaStore` on the fastest tier — already consistent with the metadata-on-SSD invariant in `disk-spindown.md`.
- **FUSE op surface today** (`src/fuse-ns/fuse_ns.c:2707–2726`): 18 ops registered. Missing: `symlink`, `link`, `readlink`, `mknod`, `statfs`, `access`, all xattr ops, `fallocate`, lock ops, `copy_file_range`. Phase 1 lands all of these on day one.
- **NAS orchestrators are path-based today** (`tierd/internal/{nfs,smb,iscsi}/`). No `fsid`, no file-handle, no inode tracking. Phase 4–6 build the protocol-stable identity surface from zero on top of `object_id`.
- **No kernel-module precedent in the tree.** The two C files in `src/fuse-ns/` are userspace FUSE clients built against `libfuse3`. No `dkms.conf`, no kernel headers anywhere. Phase 1 stands up the build system from scratch.

This baseline drives every "supported / forbidden / deferred" answer below.

---

## 0.1 Object Identity

Three identity layers, distinct, each with its own persistence and lifecycle.

### `object_id` — durable file identity

- **Format:** 128-bit UUIDv7, encoded as 16 raw bytes on-wire and as a 32-character lowercase hex string in SQLite (matches the existing `placement_intents.object_id TEXT` column).
- **Why UUIDv7:** time-ordered prefix gives bbolt key locality for create-heavy workloads and aligns with how movement transactions are time-clustered; remaining bits are random, so two concurrent creates on different hosts (future multi-host) cannot collide.
- **Allocation point:** kernel-side, inside `smoothfs_create()`, before any IO to the lower. Allocation is monotonic per-pool: kernel reserves a UUIDv7 from the in-core allocator that's seeded at mount from a per-pool secret.
- **Authoritative on-disk persistence:**
  - `trusted.smoothfs.oid` xattr on the lower file (16 raw bytes). Survives rename, lower remount, kernel module reload. Lost only with the file itself.
  - Mirrored into `placement_intents.object_id` in `tierd` SQLite at the next reconcile tick (typically within 1s; coalesced).
- **Recovery after crash:**
  - Kernel re-reads `trusted.smoothfs.oid` lazily on first lookup of each lower file.
  - If the xattr is absent (lower file was created out-of-band — supported only for repair tooling), the inode is **quarantined**: visible via debugfs but not exposed in the namespace until an operator runs `tierd-cli smoothfs adopt <path>`, which assigns a new `object_id`.
  - SQLite is replayed into the kernel via `SMOOTHFS_CMD_RECONCILE` at mount. Conflict resolution: lower xattr wins for `object_id`; SQLite wins for placement state. Reasoning: xattrs cannot lie about identity (they live next to the data), but placement state can lag behind movement that the kernel didn't get to ack before crash.
- **Lifecycle:** an `object_id` is allocated at create, retained across rename/movement/snapshot-restore, and **destroyed only on `unlink(2)` of the last hard link**. It is not reused; new files always get a fresh UUIDv7.

### Exported inode identity

The kernel must expose a 64-bit inode number through `stat(2)`. It is not the lower's inode number (that changes on movement) and it is not the `object_id` directly (too wide).

- **Synthesis rule:** `inode_no = xxhash64(object_id) | (1 << 63)` — high bit set to keep smoothfs inodes outside any conceivable lower allocator range, low 63 bits derived deterministically.
- **Collision handling:** at the in-core inode-cache level only. Two `object_id`s that hash to the same `inode_no` are stored in a chained slot; lookup disambiguates by full `object_id` comparison. The kernel-exposed `inode_no` is allowed to collide because no protocol or client cares about uniqueness of the 64-bit number alone — they always combine it with `st_dev`.
- **Stability:** stable for the lifetime of the `object_id`. Movement does not change it. Module reload does not change it. Restoring from snapshot preserves it.

### Protocol export identity

NFSv3/v4 file handles, SMB FileId, iSCSI is N/A (LUN backing files are pinned and addressed by path).

- **NFS file handle body** = `fsid (4) | object_id (16) | gen (4)` = **24 bytes**. Fits inside NFSv3's 32-byte and NFSv4's 128-byte handle envelopes with room for protocol-version bytes added by the export layer.
- **`fsid`:** `xxhash32(pool_uuid)`. Stable across reboots and module reloads. Different pools always have different `fsid`s; collision probability is negligible for the appliance's pool count.
- **Generation counter (`gen`):** persisted in `trusted.smoothfs.gen` xattr next to `trusted.smoothfs.oid`. **Monotonic uint32, incremented only on identity reuse**, never on movement, never on rename. Identity reuse cannot happen for a UUIDv7 (non-reusing allocator), so the practical role of `gen` is to bump when the **lower inode number** is reused after `unlink + create-on-same-lower` — protecting clients holding handles to the unlinked file from accidentally seeing the new file. Phase 4 hard-wires `gen = 0` in the on-wire NFS handle body and ignores it on decode (see §0.7 Phase 4 addenda); the bump path lands with Phase 5 SMB.
- **SMB FileId:** the same `inode_no` from §Exported inode identity, paired with `gen` in the volume serial. Samba VFS module pulls both via `getxattr`.

### Discrepancy with current code

Today `placement_intents.object_id TEXT` is **nullable** (no NOT NULL constraint, no format validator). Phase 1 migration tightens this: `object_id TEXT NOT NULL CHECK(length(object_id) = 32)` for any row in the smoothfs path. Existing FUSE namespaces continue to write NULL and are not subject to the constraint until they convert to smoothfs (if ever).

---

## 0.2 Placement Authority

**The contract ratifies the parent proposal's hybrid model.** SQLite is the durable truth; the kernel's on-disk record is a hot-path cache.

### Authoritative record fields

One row per object in the kernel cache, mirrored in SQLite:

| Field | Type | Origin | Purpose |
|---|---|---|---|
| `object_id` | 16 bytes | allocated by kernel at create | durable identity (see §0.1) |
| `current_tier_id` | TEXT (FK to `tier_targets.id`) | tierd | tier holding authoritative data |
| `intended_tier_id` | TEXT or NULL (FK to `tier_targets.id`) | tierd policy | non-null while a movement is queued or in flight |
| `movement_state` | enum | shared | one of the 9 states in §0.3 |
| `transaction_seq` | uint64 | per-pool monotonic, allocated by tierd | orders movement transactions; survives reboot |
| `last_committed_cutover_gen` | uint32 | bumped at each successful cutover | for stale-pointer detection in cached kernel handles |
| `pin_state` | enum | tierd | `none / pin_hot / pin_cold / pin_hardlink / pin_lun` |
| `nlink` | uint16 | kernel-observed, mirrored to tierd | hardlink-set pinning trigger (§0.4) |

### Where the durable copy lives

- **SQLite** (`tierd/internal/db/`): authoritative. The existing `placement_intents` table grows one new column per Phase 1 schema migration — `current_tier_id` (already present as `intended_target_id`'s sibling-in-spirit), `movement_state`, `transaction_seq`, `last_committed_cutover_gen`. New table `smoothfs_objects` indexes `object_id → namespace_id, current_tier_id, gen` for O(1) lookup at reconcile.
- **Per-pool kernel metadata file:** binary, append-only with periodic checkpoint. Stored at `<pool>/.smoothfs/placement.log` on the **fastest tier** (consistent with disk-spindown's metadata-on-SSD invariant — never on a spindownable HDD). Format: 64-byte fixed header per record, fields packed in the order above, varint-prefixed for forward compatibility.
- **Per-file lower xattrs:** `trusted.smoothfs.oid` (16 bytes), `trusted.smoothfs.gen` (4 bytes). These are the only metadata that lives on the file itself; everything else is in the per-pool log + SQLite.

### Reconcile algorithm at mount

1. Kernel mounts the pool with no in-memory placement state.
2. Kernel reads `<pool>/.smoothfs/placement.log` checkpoint + tail. Builds in-core map `object_id → placement record`.
3. Kernel emits `SMOOTHFS_EVENT_MOUNT_READY` with the checkpoint sequence number.
4. tierd compares `placement_intents WHERE namespace_id = <pool>` against the kernel's checkpoint sequence. Any rows with `transaction_seq > checkpoint_seq` are replayed via `SMOOTHFS_CMD_RECONCILE`.
5. Any object whose `movement_state` is non-terminal at this point enters the §0.8 repair flow.

### Conflict resolution

- **xattr says A, SQLite says B for `object_id`:** xattr wins. Reasoning: the lower file is the data; identity that disagrees with the data is meaningless. SQLite row is corrected.
- **kernel log says tier X, SQLite says tier Y for `current_tier_id`, both for the same `transaction_seq`:** SQLite wins. Reasoning: the kernel may have logged the cutover but failed to ack to tierd before crash; the SQLite record reflects what tierd believes was committed. (This is the only case where SQLite outranks the kernel; it's specifically the cutover ack hand-off.)
- **kernel log says tier X with `transaction_seq = N`, SQLite says tier Y with `transaction_seq = N+1`:** SQLite wins (higher sequence). Repair flow re-validates the lower data.

---

## 0.3 Movement Transaction Model

Nine states. Names that already exist in `tierd/internal/db/zfs_managed.go:265–270` are preserved verbatim; new states are introduced only where the existing log is too coarse.

| # | State | Durable record written | Authoritative copy | Allowed user-visible state |
|---|---|---|---|---|
| 1 | `plan_accepted` | `smoothfs_objects.movement_state = plan_accepted`, `intended_tier_id` set | source | reads/writes go to source; no visible change |
| 2 | `destination_reserved` | dest inode allocated on lower, `trusted.smoothfs.oid` set on dest, `placement.log` records reservation | source | reads/writes still go to source |
| 3 | `copy_in_progress` | (unchanged from zfsmgd; per-block progress counter optional) | source | reads/writes go to source; new writes propagate to dest copy via dirty-region tracking |
| 4 | `copy_complete` | `placement.log` records copy bytes equal source size; xattr set complete | source | reads/writes go to source; dest is byte-equal but unverified |
| 5 | `copy_verified` | checksum + xattr-set + sparse-map equality recorded; SQLite UPDATE | source | reads/writes go to source; dest is verified, awaiting cutover scheduling |
| 6 | `cutover_in_progress` | `placement.log` records cutover-start barrier; new opens are stalled briefly | undefined for ≤50 ms | new opens block; existing opens continue against source |
| 7 | `switched` | atomic xattr swap on lower; `placement.log` records cutover commit; `last_committed_cutover_gen++` (only on identity reuse — see §0.1) | **destination** | reads/writes flip to destination; existing source-backed opens are reissued against dest at next syscall boundary |
| 8 | `cleanup_in_progress` | source unlink scheduled; `placement.log` records cleanup start | destination | reads/writes go to destination |
| 9 | `cleanup_complete` | source removed from lower, log compacted, SQLite UPDATE | destination | terminal — record archived after retention window (default 7 days for forensics) |

Plus two error sinks reused from zfsmgd: `failed` and `stale`. Same semantics as existing log.

### Why nine instead of zfsmgd's five

The zfsmgd states `copy_in_progress / copy_complete / switched / cleanup_complete / failed` are fine for a single-host userspace cutover that holds an exclusive lock for the duration of the swap. smoothfs runs the cutover in the kernel against potentially-open handles, so it needs:

- explicit `destination_reserved` (so a concurrent unlink finds the dest and removes it without ambiguity)
- explicit `copy_verified` (separates I/O completion from policy-gate verification — required for the optional reflink-preserve path in §0.4)
- explicit `cutover_in_progress` (the brief barrier where new opens stall)
- explicit `cleanup_in_progress` (so a crash mid-cleanup is recoverable without re-validating the destination)

The five zfsmgd state names that survive are unchanged in meaning. zfsmgd continues to use its own state set in the FUSE-backed code path; smoothfs uses the nine-state set in kernel-backed code. **No state-name aliasing across the two.**

### Crash record per transition

Each transition writes one record to `<pool>/.smoothfs/placement.log` with the new state and a transition-specific payload (e.g., reserved dest inode, copied byte count, cutover commit timestamp). Records are append-only, fsync'd before the kernel returns success to tierd. The log is checkpointed every 256 records or every 60 s (whichever first) to bound replay time.

---

## 0.4 Concurrency Semantics

Each row below is a contract. "Phase 1" means the v1 kernel module must implement it; "Deferred to Phase N" means the case is rejected with the named errno until the named phase.

| Case | Ruling | Mechanism |
|---|---|---|
| File open for read during promotion | **Supported** | reads route to source until `switched`; on `switched`, kernel reissues open against dest transparently before next read |
| File open for read during demotion | **Supported** | identical mechanism |
| File open for write during promotion | **Supported** | writes route to source; dirty-region tracker propagates to dest copy; cutover stalls new opens for ≤50 ms while the dirty-region tail is flushed; on `switched`, write fd is reissued against dest |
| File open for write during demotion | **Supported** | identical mechanism |
| `MAP_PRIVATE` mapping during movement | **Supported** | mapping is backed by source pages until the process drops it; new faults after `switched` resolve against dest |
| `MAP_SHARED` for read during movement | **Supported** | mapping is torn down at `cutover_in_progress` and re-established against dest at `switched`; readers see a brief stall, no data corruption |
| `MAP_SHARED` for write during movement | **Forbidden** during movement | scheduler queries kernel for any `VM_WRITE | VM_SHARED` mapping on the object; if present, transition `plan_accepted → destination_reserved` is **refused** with no errno surfaced (movement is silently skipped, retried later); admin override path is `tierd-cli smoothfs quiesce <path>` which `EBUSY`s new `MAP_SHARED|PROT_WRITE` opens and waits for existing ones to drop |
| `rename(2)` during movement | **Supported** | rename is metadata-only on the object record; it does not touch the lower data; movement continues unaffected |
| `unlink(2)` during movement | **Supported** | sets `intended_tier_id = NULL`; depending on movement state: states 1–4 cancel the movement and drop the dest copy; states 5–7 complete the movement then unlink; states 8–9 unlink the new dest as the cleanup step |
| `link(2)` during movement, same lower | **Supported** | hardlink created on the source lower; movement transitions to `failed` (cannot move a hardlink-set in v1, see below) and the link-set is repinned |
| `link(2)` during movement, cross-tier (shouldn't reach here) | **`EXDEV`** | rejected at VFS layer because both lowers cannot satisfy `link(2)` |
| Hardlink-set with `nlink > 1` at scheduling time | **Pinned to current tier** | scheduler skips planning; `pin_state = pin_hardlink`; cleared automatically when `nlink` returns to 1 |
| `flock(2)` / OFD locks held during movement | **Supported** | locks held in smoothfs inode (not the lower); survive cutover unchanged |
| `fcntl(F_SETLK)` advisory locks held during movement | **Supported** | identical to flock |
| Mandatory locks | **`ENOTSUP`** at `mount(2)` if `mand` is in mount options | matches Linux kernel direction (mandatory locks deprecated since 5.15) |
| SMB lease / oplock held during movement | **Forbidden in Phase 1**; deferred to Phase 5 | scheduler skips planning if `pin_state = pin_lease` (set by Samba VFS module via `setxattr trusted.smoothfs.lease=1`); Phase 5 implements lease-break-on-cutover via fanotify |
| `O_DIRECT` open during movement | **Forbidden** during movement | scheduler skips planning if any `O_DIRECT` fd is open against the object; admin override is the same quiesce path as `MAP_SHARED` write |
| `O_DIRECT` against a placed file (no movement queued) | **Supported** if lower advertises it | XFS-on-LV: yes. ZFS: yes via the `direct` property; capability-probed at mount (§0.6) |
| `fallocate(2)` during movement | **`EAGAIN`** | scheduler skips space-changing ops on objects in non-terminal movement state; caller may retry |
| `copy_file_range(2)` between two smoothfs files | **Supported** | falls back to read+write loop unless source and destination are on the same lower with reflink; if same-lower-reflink, uses `copy_file_range` natively on the lower |
| `splice(2) / sendfile(2)` | **Supported** | implemented via `generic_file_splice_read` / `iter_file_splice_write` against the lower fd |
| Snapshot during movement | **Drained at quiesce** | `SMOOTHFS_CMD_QUIESCE` blocks new transitions and waits for in-flight cutovers; in-flight `read`/`write` ops are not blocked |

### Open-file reissue protocol

The "kernel reissues open against dest at next syscall boundary" mechanism in the table above needs to be unambiguous because it's the load-bearing trick of v1.

- Each smoothfs file struct holds a `lower_fd` plus a `cutover_gen`. The first I/O syscall after `cutover_gen` advances re-resolves `lower_fd` against the current authoritative tier and decrements a refcount on the old `lower_fd`.
- Re-resolution is a kernel `vfs_open()` against the new lower path with the same flags and cred from the original open. Failure modes (dest gone, permissions changed) translate to `ESTALE` returned from the syscall, which clients are required to handle.
- The reissue is invisible to userspace for normal `read`/`write` because they go through smoothfs's `read_iter`/`write_iter`, which do the re-resolution before calling the lower's iter ops.
- `mmap` reissue is handled by tearing down the VMA at cutover and refaulting through the new `lower_fd`.

This contract assumes every Phase 1 lower exposes a usable `vfs_open()`-compatible path. The §0.6 capability matrix verifies this.

---

## 0.5 Heat and Policy Contract

Heat collection is not optional in Phase 2. Re-introducing it as a kernel responsibility (rather than re-enabling the disabled fields in `meta.Record`) is the design intent of the parent proposal.

### Heat inputs (kernel-collected)

Per object, kept in the in-core inode struct:

| Input | Type | Updated by | Cost |
|---|---|---|---|
| `open_count` | uint32, saturating | `smoothfs_open` | 1 atomic inc per open |
| `read_bytes` | uint64 | `read_iter` | 1 atomic add per read |
| `write_bytes` | uint64 | `write_iter` | 1 atomic add per write |
| `last_access_ns` | uint64 | `read_iter` / `write_iter` / `mmap_fault` | 1 store per op (no atomic; tolerates lost-update at sample boundary) |
| `last_sample_ns` | uint64 | sample drain | 1 store per drain |

### Sampling

- Per-pool ring buffer in kernel, 8192 slots, single-producer per CPU, single-consumer drain.
- Drain triggers: every 30 s timer, or buffer >75% full, or `releaseop` for an object with non-zero counters.
- Drained record format: `object_id | open_count_delta | read_bytes_delta | write_bytes_delta | last_access_ns | sample_window_ns`. 48 bytes per record.
- Drain emits one `SMOOTHFS_EVENT_HEAT_SAMPLE` netlink message containing up to 256 records (≈12 KiB per message, well under netlink 16-page default).

### Aggregation (tierd)

- EWMA per object with configurable half-life. Default: 24 h.
- Per-pool heat distribution recomputed on every drain (at most once per 30 s). Output: deciles per tier.
- Promotion candidates: objects whose EWMA exceeds the 80th percentile of the current tier's distribution.
- Demotion candidates: objects whose EWMA falls below the 20th percentile of the current tier's distribution.

### Anti-thrash gates

Movement is **blocked** for an object if any of the following hold:

| Gate | Default | Override |
|---|---|---|
| Min residency on current tier | 1 h | per-pool `min_residency_seconds` |
| Cooldown after last successful movement | 6 h | per-pool `movement_cooldown_seconds` |
| Hysteresis between adjacent tiers | EWMA must exceed promote-threshold by ≥20% relative, or fall below demote-threshold by ≥20% relative, before scheduling | per-pool `hysteresis_factor` |
| Source-tier fullness override | If source is >`full_threshold_pct` and the object qualifies for demotion, residency and cooldown are bypassed | per-pool |
| Destination-tier fullness | If dest >`full_threshold_pct`, promotion is **deferred** (not failed) and the object is rechecked next tick | per-pool |
| Pin state | `pin_hot` blocks demotion; `pin_cold` blocks promotion; `pin_hardlink` and `pin_lun` block both directions | per-object |

### Tie-breaking

When two objects have identical EWMA at scheduling time, lower `object_id` (lexicographic on the 32-char hex) wins. Deterministic for replay and debugging.

### Fields persisted long-term

EWMA value (uint64, IEEE 754 double bit pattern), `last_movement_ns`, `pin_state`. Stored in the per-pool placement log, not in the lower file's xattrs. (Heat is policy state; it doesn't follow the file across pools.)

---

## 0.6 Lower-FS Capability Contract

Mount is refused if any **required** bit fails to probe true. Optional bits are recorded in `tier_targets.capabilities_json` (existing column at `migrations.go:336`) and consulted by movement and policy code.

### Capability bits

| Bit | Required? | Probe |
|---|---|---|
| `xattr_user` | required | `setxattr user.smoothfs-probe` round-trip on a probe file |
| `xattr_trusted` | required | same with `trusted.smoothfs.probe` (root-only) |
| `xattr_security` | required | preserve `security.capability` round-trip |
| `posix_acl` | required | `setfacl -m u:nobody:r` round-trip |
| `rename_atomic_within_dir` | required | concurrent `rename` race test |
| `rename_atomic_cross_dir` | required | concurrent `rename` cross-dir race test |
| `hardlink_within_lower` | required | `link` round-trip |
| `mmap_coherence_under_writers` | required | one writer + one mmap reader sees writer's bytes within 1 fsync |
| `direct_io` | required | `O_DIRECT` open + 4 KiB write/read |
| `fsync_durability` | required | write + fsync + crash-equivalent (drop caches, re-read) |
| `sparse_seek_data_hole` | required | `lseek SEEK_DATA / SEEK_HOLE` returns expected offsets on a sparse file |
| `inode_generation` | required | `FS_IOC_GETVERSION` returns a non-zero generation, or lower exposes equivalent (ZFS: dataset+object id pair) |
| `quota_per_user` | optional | `quotactl(Q_GETQUOTA)` succeeds |
| `quota_per_project` | optional | XFS-only; checked but not required |
| `reflink_within_lower` | optional | `FICLONE` succeeds for a small file |
| `copy_file_range_offload` | optional | `copy_file_range` returns >0 between two files on this lower |
| `fscrypt` | optional | `FS_IOC_GET_ENCRYPTION_POLICY` succeeds on a probe directory |

### Phase 1 expected values

| Capability | XFS-on-LV | ZFS dataset |
|---|---|---|
| `xattr_user` | true | true |
| `xattr_trusted` | true | true |
| `xattr_security` | true | true |
| `posix_acl` | true | true (with `acltype=posixacl`) |
| `rename_atomic_within_dir` | true | true |
| `rename_atomic_cross_dir` | true | true |
| `hardlink_within_lower` | true | true |
| `mmap_coherence_under_writers` | true | true |
| `direct_io` | true | true (with `direct=standard` or `always`) |
| `fsync_durability` | true | true |
| `sparse_seek_data_hole` | true | true |
| `inode_generation` | true (`FS_IOC_GETVERSION`) | true (object id + dataset GUID) |
| `quota_per_user` | true | true |
| `quota_per_project` | true | false |
| `reflink_within_lower` | true (with `reflink=1` mkfs) | false in v1 (block clones via `clone_range` not surfaced through VFS yet) |
| `copy_file_range_offload` | true | true (block-level clone via `copy_file_range`) |
| `fscrypt` | true | false |

A mount specifying ZFS as the lower with `reflink_required = true` in pool config is rejected with `EOPNOTSUPP` and a descriptive `dmesg` line.

### Probe execution

- Probes run synchronously at `mount(2)` time.
- Each probe is bounded to 5 s wall-clock; timeout = capability assumed false.
- Probe files live under `<lower>/.smoothfs/probes/`, cleaned up on probe exit.
- Probe results cached for the mount lifetime; a `SMOOTHFS_CMD_REPROBE` netlink command forces re-probe (used after a lower upgrade).

---

## 0.7 Protocol Invariants

### NFS

Required before Phase 4 enables NFS export of a smoothfs pool:

| Invariant | Source | Met by |
|---|---|---|
| Stable export handle | §0.1 NFS handle body | `object_id` is allocated once and never reused; `fsid` is `xxhash32(pool_uuid)`; `gen` only changes on identity reuse |
| Reconnect-safe handle resolution | NFSv4 STATEID lifetime | smoothfs `lookup_handle(fh)` decodes `object_id` from the handle, looks up in the placement map, returns the smoothfs inode; never goes through path resolution |
| Rename-safe handle behaviour | NFS RFC 7530 §4.2 | rename does not change `object_id`; handle remains valid |
| Movement-safe handle behaviour | this proposal | cutover does not change `object_id`; the kernel re-resolves `lower_fd` transparently inside `lookup_handle` |
| Generation counter never increments on movement | §0.1 | `gen` is only bumped by the unlink+create-on-same-lower path inside `smoothfs_create()`; movement does not call that path |
| `statfs` returns pool aggregate | parent §POSIX statfs | Phase 1 implements `smoothfs_statfs` summing all tier targets |

NFS export configuration: `tierd/internal/nfs/` continues to write `/etc/exports`. For a smoothfs-backed export, `tierd` adds `fsid=` derived from `pool_uuid` to the export options to make `fsid` stable across Linux kernel reboots that reassign the kernel's automatic fsid.

#### Phase 4 addenda — implementation pinning

The Phase 4 plan uncovered three details left implicit in §0.1 / §0.7 that must be pinned before kernel `export_operations` land. These are *constraints on the Phase 4 implementation*, not relaxations of the contract:

1. **`gen` is hard-wired to 0 in the Phase 4 file-handle body.** §0.1 says `gen` bumps on lower-inode reuse after `unlink + create-on-same-lower`, but no Phase 1–3 code path implements that bump (UUIDv7 OIDs are not reusable, so `gen` has no other reason to move). Rather than ship a half-implementation, Phase 4 encodes `gen = 0` in every emitted handle. The 4-byte field stays in the wire format (so the handle layout never changes) and is *reserved* for Phase 5+ when SMB lease-aware identity reuse may need it. `fh_to_dentry` ignores the `gen` field on decode in Phase 4.

2. **Phase 4 emits non-connectable handles only.** `export_operations.encode_fh` is called with `parent != NULL` when the export layer needs a handle the resolver can later combine with `fh_to_parent` for `LOOKUPP`-class operations (NFSv4 OPEN-by-name, silly-rename cleanup). Phase 4 returns `FILEID_INVALID` when the caller requests connectable encoding, and stubs `fh_to_parent` to `NULL`. nfsd falls back to path-based parent resolution, which is correct for Phase 4's non-replicated namespace. Connectable handles — which would require carrying `parent_object_id` in the handle body or a fast `inode_no → parent_inode_no` reverse-map — are deferred to Phase 4.5, conditional on cthon04 actually demanding them.

3. **`fsid` lives at two independent layers, both derived from the pool UUID.** NFS file handles carry both nfsd's per-export `fsid` (for nfsd-side routing) and smoothfs's per-fs prefix (for fileid validation):

   - **nfsd export fsid** — written to `/etc/exports` as `fsid=<pool_uuid>` (the full UUID string, the form `exportfs(8)` accepts; integer/`uuid`/UUID-string are the only valid forms). Stable across reboots; tierd renders it directly from the pool UUID.

   - **smoothfs fileid prefix** — `xxhash32(pool_uuid_bytes, 16, 0)`, computed by `smoothfs_fill_super` into `sbi->fsid` and emitted as the leading 4 bytes of every fileid body (the 24-byte `fsid | object_id | gen` layout above). On decode, `fh_to_dentry` rejects any fileid whose prefix doesn't match `sbi->fsid` — this is a smoothfs-internal sanity check that catches handles routed to the wrong sb instance (e.g. a stale handle from a previous mount of a different pool that happened to hash to the same nfsd export).

   The xxh32 prefix is **not** what `/etc/exports` carries. tierd's helper renders both forms from the same `pool_uuid` so they cannot drift from each other.

The on-wire fileid type for the 24-byte `fsid | object_id | gen` body is `FILEID_SMOOTHFS_OID = 0x53` (debug-friendly literal `'S'`). It is outside Linux's standard `enum fid_type` range (1–6, plus `FILEID_INVALID = 0xff`) and stable across the smoothfs lifetime; new variants (e.g. a 40-byte connectable form for Phase 4.5) take new fileid type values without breaking decoders.

### SMB

Required before Phase 5 enables SMB share of a smoothfs pool:

| Invariant | Source | Met by |
|---|---|---|
| Stable file identity | SMB FileId | smoothfs exposes `inode_no` (from §0.1) + `gen` to Samba via xattrs `trusted.smoothfs.fileid` and `trusted.smoothfs.gen` |
| Rename correctness | SMB protocol | rename is metadata-only at the smoothfs layer (does not change `object_id`); FileId stable across rename |
| Lease / oplock behaviour under movement | this proposal | scheduler skips movement on objects with `pin_state = pin_lease`; Samba VFS module sets the pin via `setxattr trusted.smoothfs.lease=1` on lease grant and clears it on lease break |
| Lease-break notification on movement | this proposal | Phase 5 only — when movement is forced (admin override), kernel emits `fanotify` event the Samba VFS module subscribes to |
| Case-insensitive matching | SMB clients | not in smoothfs — Samba VFS module's responsibility (Phase 5 ships the module) |
| Change notification | SMB FileNotifyChange | Phase 5 — `fanotify` events from smoothfs forwarded by Samba VFS module |

### iSCSI

Required before Phase 6 enables iSCSI LUN backing on a smoothfs pool:

| Invariant | Source | Met by |
|---|---|---|
| Active backing files are pinned | this proposal | LUN-backing files are created with `pin_state = pin_lun`; scheduler skips them in both directions |
| Pin lift requires admin quiesce | this proposal | `tierd-cli iscsi quiesce <target>` calls `targetcli` to take target offline, then clears the pin; Phase 6 ships this |
| Write-ordering | LIO `fileio` backend | smoothfs preserves the lower's `O_SYNC` / `O_DIRECT` semantics (capability probed in §0.6) |
| `fsync` / barrier semantics | LIO | smoothfs `fsync` calls down to lower; lower must report durable on success (XFS, ZFS both compliant) |
| `O_DIRECT` support in LUN path | LIO | required capability in §0.6; both Phase 1 lowers compliant |
| Sparse allocation | LIO | preserved at copy via `SEEK_DATA / SEEK_HOLE`; required capability in §0.6 |

---

## 0.8 Failure and Repair Model

For each crash state, a deterministic algorithm. "Lower X" means the on-disk bits on the tier holding the source/dest of the in-flight movement.

### State: `destination_reserved` after crash

- **Lower source:** intact, authoritative.
- **Lower dest:** zero-byte file with `trusted.smoothfs.oid` set; possibly a partial preallocation.
- **SQLite:** `movement_state = destination_reserved`.
- **Repair:** delete dest file; clear `intended_tier_id`; transition `failed` → archive. Source remains authoritative. No data risk.

### State: `copy_in_progress` after crash

- **Lower source:** intact, authoritative.
- **Lower dest:** partial copy.
- **SQLite:** `movement_state = copy_in_progress`, optional progress byte count.
- **Repair:** delete dest file and its xattrs; transition `failed` → archive. Operator may re-plan via tierd. (Identical to zfsmgd's existing `copy_in_progress` recovery in `recovery_test.go:127–241`.)

### State: `copy_complete` after crash

- **Lower source:** intact, authoritative.
- **Lower dest:** byte-equal but unverified.
- **SQLite:** `movement_state = copy_complete`.
- **Repair:** re-run verify step; on success, transition to `copy_verified` and resume; on failure, delete dest and transition `failed`. (Slight enhancement over zfsmgd, which deletes the unverified copy unconditionally per `recovery_test.go:247–326`.)

### State: `copy_verified` after crash

- **Lower source:** intact, authoritative.
- **Lower dest:** byte-equal, verified.
- **SQLite:** `movement_state = copy_verified`.
- **Repair:** resume from `cutover_in_progress`. Idempotent.

### State: `cutover_in_progress` after crash

- **Lower source:** xattr swap may have completed partially.
- **Lower dest:** xattr swap may have completed partially.
- **SQLite:** `movement_state = cutover_in_progress`.
- **Repair:** examine `trusted.smoothfs.oid` on both sides:
  - If only source has the OID: roll back. Cutover did not commit. Transition `copy_verified` → re-attempt cutover.
  - If only dest has the OID: cutover committed. Transition `switched`. Source xattr will be cleared by cleanup.
  - If both have the OID (intermediate state): cutover is partial. Force forward: clear source OID, transition `switched`. Reasoning: the dest already has the OID, so any client cache lookup that reaches a kernel after restart will find the dest first; rolling back risks two files claiming the same identity.
  - If neither has the OID (operator interfered with xattrs): quarantine both; require manual `tierd-cli smoothfs reconcile <object_id>`.

### State: `switched` after crash

- **Lower source:** present, OID cleared.
- **Lower dest:** present, OID set, authoritative.
- **SQLite:** `movement_state = switched`.
- **Repair:** transition to `cleanup_in_progress`. Schedule source unlink. Idempotent. (Matches zfsmgd `recovery_test.go:332–414`.)

### State: `cleanup_in_progress` after crash

- **Lower source:** present (unlink not committed) or absent (unlink committed but not logged).
- **Lower dest:** authoritative.
- **SQLite:** `movement_state = cleanup_in_progress`.
- **Repair:** check source presence; if present, unlink; transition `cleanup_complete`. Idempotent.

### State: stale movement intent after policy change

- **Trigger:** `tier_targets` row updated (rank change, tier removed, etc.) while a movement is in flight.
- **Repair:** at the next `SMOOTHFS_CMD_POLICY_PUSH`, kernel walks the in-flight movement set, re-evaluates each against the new policy, and either:
  - lets it complete (if dest still valid)
  - aborts and reverts to source (if dest no longer valid; only safe pre-cutover)
  - completes and re-plans demotion (if cutover already committed)

### State: tier disappears mid-transaction

- **Trigger:** lower mount lost (block device error, ZFS pool fault).
- **Repair:** `SMOOTHFS_EVENT_TIER_FAULT` to tierd. All in-flight movements involving the lost tier transition to `failed` immediately. Objects whose **current** tier is lost are marked `unavailable`; reads return `EIO`. Recovery requires operator intervention via `tierd-cli smoothfs tier resume <id>` after the lower is reattached.

### State: metadata record disagrees with on-disk reality

- **Trigger:** offline `fsck`-like tooling, or operator copied data into a tier out-of-band.
- **Repair:** `tierd-cli smoothfs scrub <pool>` walks lower files, reconciles xattrs against placement log + SQLite, prints a diff. Operator decides apply vs. abort. No automatic apply in v1.

### State: kernel module reload mid-transaction

- **Trigger:** appliance update; admin `rmmod && modprobe`.
- **Repair:** module unload waits for the in-flight ring-buffer drain and a `placement.log` checkpoint. On reload, the standard mount-time reconcile (§0.2) covers everything in flight. No transaction is lost; some may be replayed with the same effect.

### mdadm-adapter parity

The mdadm movement log (`migrations.go:534–545`) currently has only a single default state `'copy_in_progress'`. **Phase 1 migration adds a CHECK constraint** matching the nine-state enum above for any mdadm-backed pool that runs under smoothfs. Existing FUSE-backed mdadm pools keep the looser schema.

---

## 0.9 Conformance Test Plan

### Frameworks

| Framework | Purpose | Owner | Status |
|---|---|---|---|
| `xfstests` (suites `generic/`, `shared/`) | POSIX/filesystem conformance baseline | TBD | not started |
| Bespoke `smoothfs-suite` | movement transitions, crash injection, capability probes, heat sampling | TBD | not started |
| `pjdfstest` | sanity layer (rename, link, mode, perms) | TBD | not started |
| `cthon04` | NFS conformance (Phase 4 gate) | TBD | not started |
| `smbtorture` | SMB conformance (Phase 5 gate) | TBD | not started |
| LIO self-tests | iSCSI conformance (Phase 6 gate) | TBD | not started |
| `fuse-ns-create-fast-path.md` 10×1000-file harness | performance regression, baseline against current FUSE | TBD | not started |

### Required test inventory (Phase 1 gating)

- **xfstests pass list:** the `quick` group must pass against XFS-on-LV and against ZFS lowers, on the matrix of supported kernels (§Operational Delivery in parent). Failures are categorised as (a) lower-fs limitation — accepted, documented; (b) smoothfs bug — blocking.
- **Crash-replay tests** at every transition in §0.3:
  - one test per transition that injects a crash before the transition's durable record is fsynced
  - one test per transition that injects a crash after the durable record but before tierd-side ack
  - one test per transition that simulates a tierd restart only (kernel survives)
  - matrix size: 9 transitions × 3 crash points × 2 lowers = 54 mandatory tests
- **mmap correctness:**
  - `MAP_PRIVATE` during movement: data integrity verified against pre-cutover bytes
  - `MAP_SHARED` read during movement: re-fault correctness verified
  - `MAP_SHARED` write during movement: scheduler-skip behaviour verified (movement does not start)
- **Hardlink and rename:**
  - `link(2)` while idle: stays on current tier
  - `link(2)` during in-flight movement: aborts the movement, repins to source tier
  - `rename(2)` during all 9 movement states: each must complete without breaking the movement
- **Lower-fs capability probe:** all 17 bits in §0.6 tested against both Phase 1 lowers; expected values match the table.
- **Heat sampling:**
  - synthetic workload generates known per-object op counts
  - drained samples are received by tierd and EWMA values match expected within 5%
  - anti-thrash gates verified by forced rapid-toggle workload
- **Identity invariants:**
  - `object_id` survives 1000-cycle rename / movement / snapshot-restore loop unchanged
  - `gen` does **not** increment on any of those operations
  - `gen` **does** increment on unlink + create-on-same-lower-inode-number

### Performance baseline (Phase 0 deliverable, gating Phase 1 start)

- Run the `fuse-ns-create-fast-path.md` 10×1000-file rsync harness against the current FUSE stack on an XFS-on-LV pool and a ZFS pool. Record p50, p95, p99 CREATE latency.
- Re-run the same harness against native XFS and native ZFS (no smoothfs, no FUSE) for upper-bound reference.
- Numbers are recorded in `docs/proposals/pending/smoothfs-phase-0-baseline.md` (created with the first run; not committed empty).
- Phase 3 completion gate is "smoothfs CREATE p99 within 2× native" per parent §Non-Functional Targets. Baseline numbers from Phase 0 establish what "native" means on the appliance hardware.

### CI integration

- Per-merge: build module against every pinned kernel in the supported matrix (§Operational Delivery in parent). Compile-only on the kernels not present in the test fleet.
- Per-merge: `xfstests quick` against XFS-on-LV.
- Nightly: `xfstests auto`, ZFS lower added, full crash-injection suite.
- Per-tag: protocol suites (`cthon04`, `smbtorture`, LIO) once Phases 4/5/6 are in flight.

---

## 0.10 Phase 0 Exit Criteria (status)

The parent proposal §0.10 says Phase 1 may not start until five things are true. This contract addresses them as follows.

| Exit criterion | Status | Evidence / next step |
|---|---|---|
| Phase 0 contract document exists | **complete (this document)** | landed in `docs/proposals/completed/smoothfs-phase-0-contract.md` |
| Reviewed and accepted by a named engineering reviewer with VFS / stacked-filesystem experience | **outstanding** | reviewer slot empty; PR description must name reviewer before merge |
| Conformance test plan exists with named owner | **partial** — plan exists (§0.9), owner TBD | named owner must be set in `docs/proposals/pending/smoothfs-phase-0-baseline.md` when that file is created |
| Performance regression harness exists and produces a baseline number against the current FUSE stack | **outstanding** | next deliverable; produces `smoothfs-phase-0-baseline.md` |
| Kernel version matrix and DKMS / packaging plan agreed | **partial** — parent §Operational Delivery defines the shape; the exact pinned kernel range and signing-key custody process need an explicit decision | resolution recorded in `docs/proposals/pending/smoothfs-phase-0-kernel-matrix.md` (not yet created) |
| No open questions remain in §0.1–0.8 | **complete in this document** | each subsection above gives an unambiguous ruling; future amendments require a new dated revision header |

**Phase 1 is therefore still blocked**, by:

1. Named VFS reviewer.
2. Performance baseline numbers.
3. Pinned kernel matrix decision.

This contract closes the design questions; the three blockers above are operational and need humans with names attached.

---

## Appendix A — Open Question Resolutions Summary

| Parent §0.x | Question | This document's answer | Section |
|---|---|---|---|
| 0.1 | How is `object_id` created? | UUIDv7, allocated kernel-side at `smoothfs_create` | §0.1 |
| 0.1 | Where is `object_id` persisted? | `trusted.smoothfs.oid` xattr (durable) + SQLite (cache for global lookup) | §0.1 |
| 0.1 | How is `object_id` recovered after crash? | Lazy xattr read at first lookup; SQLite reconciles at mount | §0.1 |
| 0.1 | How is inode identity derived from `object_id`? | `xxhash64(object_id) | (1 << 63)` with chained collision slots | §0.1 |
| 0.1 | What generation field prevents stale-handle reuse? | `trusted.smoothfs.gen` xattr, monotonic uint32 | §0.1 |
| 0.1 | NFSv3 generation counter persistence and update rule? | persisted in `trusted.smoothfs.gen`; bumped only on lower-inode-number reuse, never on movement | §0.1 |
| 0.2 | One authoritative placement record per file — where? | hybrid: SQLite is durable truth, kernel placement log is hot-path cache, lower xattr is identity anchor | §0.2 |
| 0.2 | Conflict resolution rules? | xattr wins for identity, SQLite wins for placement state; sequence numbers break ties | §0.2 |
| 0.3 | Exact transaction boundaries? | nine states, durable record at every transition, source/destination authority defined per state | §0.3 |
| 0.4 | Concurrency case-by-case rulings? | full table covering reads, writes, mmap variants, rename, unlink, link, locks, leases, O_DIRECT, fallocate, copy_file_range, splice, snapshot | §0.4 |
| 0.5 | Heat inputs, decay, thresholds, anti-thrash? | kernel collects 4 inputs, drains every 30 s; tierd EWMA half-life 24 h; promote >80th percentile, demote <20th; min residency 1 h; cooldown 6 h | §0.5 |
| 0.6 | Lower-fs capability matrix and Phase 1 expected values? | 17 bits; XFS-on-LV and ZFS expected-value table | §0.6 |
| 0.7 | NFS, SMB, iSCSI invariants? | per-protocol tables of required invariants and how each is met | §0.7 |
| 0.8 | Crash repair algorithm per state? | deterministic per-state algorithm; mdadm log brought to parity with zfsmgd | §0.8 |
| 0.9 | Conformance test plan? | xfstests + bespoke + pjdfstest + cthon04 + smbtorture + LIO + perf rig; CI integration | §0.9 |
| 0.10 | Exit criteria status? | contract complete; reviewer + harness + kernel matrix outstanding | §0.10 |

---

## Appendix B — Schema Migrations Implied by Phase 1

These are the SQLite migrations Phase 1 must add. Listed here so the contract is closed on schema before code lands.

```sql
-- New table: per-object placement record (the kernel mirror)
CREATE TABLE IF NOT EXISTS smoothfs_objects (
    object_id                  TEXT PRIMARY KEY CHECK(length(object_id) = 32),
    namespace_id               TEXT NOT NULL,
    current_tier_id            TEXT NOT NULL,
    intended_tier_id           TEXT,
    movement_state             TEXT NOT NULL DEFAULT 'placed'
                                  CHECK(movement_state IN (
                                      'placed',
                                      'plan_accepted',
                                      'destination_reserved',
                                      'copy_in_progress',
                                      'copy_complete',
                                      'copy_verified',
                                      'cutover_in_progress',
                                      'switched',
                                      'cleanup_in_progress',
                                      'cleanup_complete',
                                      'failed',
                                      'stale'
                                  )),
    transaction_seq            INTEGER NOT NULL DEFAULT 0,
    last_committed_cutover_gen INTEGER NOT NULL DEFAULT 0,
    pin_state                  TEXT NOT NULL DEFAULT 'none'
                                  CHECK(pin_state IN ('none', 'pin_hot', 'pin_cold', 'pin_hardlink', 'pin_lease', 'pin_lun')),
    nlink                      INTEGER NOT NULL DEFAULT 1,
    created_at                 TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at                 TEXT NOT NULL DEFAULT (datetime('now')),
    FOREIGN KEY (namespace_id)     REFERENCES managed_namespaces(id),
    FOREIGN KEY (current_tier_id)  REFERENCES tier_targets(id),
    FOREIGN KEY (intended_tier_id) REFERENCES tier_targets(id)
);

CREATE INDEX IF NOT EXISTS smoothfs_objects_namespace
    ON smoothfs_objects(namespace_id);

CREATE INDEX IF NOT EXISTS smoothfs_objects_movement
    ON smoothfs_objects(namespace_id, movement_state)
    WHERE movement_state NOT IN ('placed', 'cleanup_complete', 'failed');

-- Tighten placement_intents.object_id when used by smoothfs.
-- Existing FUSE rows continue to write NULL; this constraint applies only
-- to rows where namespace_id resolves to a smoothfs-backed namespace.
-- Enforced by trigger rather than CHECK so the FUSE path is unaffected.
CREATE TRIGGER IF NOT EXISTS placement_intents_smoothfs_object_id_format
BEFORE INSERT ON placement_intents
WHEN (
    SELECT backend_kind FROM managed_namespaces
    WHERE id = NEW.namespace_id
) = 'smoothfs'
  AND (NEW.object_id IS NULL OR length(NEW.object_id) != 32)
BEGIN
    SELECT RAISE(ABORT, 'smoothfs placement_intents row requires object_id of length 32');
END;

-- mdadm movement log: tighten to the nine-state enum for smoothfs-backed pools.
-- Existing FUSE-backed mdadm rows continue to use the loose schema.
CREATE TRIGGER IF NOT EXISTS mdadm_movement_log_smoothfs_state
BEFORE INSERT ON mdadm_movement_log
WHEN (
    SELECT backend_kind FROM managed_namespaces
    WHERE id = NEW.namespace_id
) = 'smoothfs'
  AND NEW.state NOT IN (
      'placed', 'plan_accepted', 'destination_reserved',
      'copy_in_progress', 'copy_complete', 'copy_verified',
      'cutover_in_progress', 'switched',
      'cleanup_in_progress', 'cleanup_complete',
      'failed', 'stale'
  )
BEGIN
    SELECT RAISE(ABORT, 'smoothfs mdadm_movement_log row uses unknown state');
END;
```

The migration goes in a new file under `tierd/internal/db/migrations.go` as the next sequential migration number after the current head.

---

## Appendix C — Out of scope for v1, recorded for the record

Each item below is something a careful reader will ask about. Each gets a one-line explicit answer for v1.

| Question | v1 answer |
|---|---|
| Per-block tiering | No. File granularity only. Block-level tiering remains the mdadm/LVM path. |
| Mirrored placement (one file on multiple tiers) | No. Each object on exactly one tier at a time. |
| Multi-host clustering | No. Single-host appliance. UUIDv7 layout reserves bits for future host-id but v1 uses zero. |
| In-place conversion of existing FUSE pools | No. Migration is rsync / `zfs send` to a new smoothfs pool. |
| Mainline kernel upstreaming | No. Out-of-tree DKMS module signed against appliance kernel. |
| Pool-wide per-user/group quotas | No. Tier-local quotas only. Cross-tier quotas need a separate proposal. |
| Reflink preservation across movement | Optional, off by default; opt-in via `preserve_reflinks` if both lowers support it. |
| Cross-encryption-policy movement | No. `EXDEV`-equivalent kernel error (`EOPNOTSUPP`) with a descriptive `dmesg` line. |
| Smoothfs-native snapshots | Deferred to Phase 5+. Phase 0.2 placement schema leaves room. |
| Kernel-side policy engine | No. Policy stays in tierd; kernel only implements anti-thrash gates and reports state. |

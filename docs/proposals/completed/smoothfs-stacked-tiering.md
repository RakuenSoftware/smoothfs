# Proposal: smoothfs — Native Kernel Stacked Tiering Filesystem

**Status:** Complete for v1. Phase 0 contract, Phase 1 scaffold, and all Phase 2 work through Phase 2.7 (heat + movement state machine, per-fd cutover reissue, writable-fd movement, SmoothKernel extraction, `vma_set_file` mmap pivot, auto-discovery + revoke, live E2E validation, and kernel placement-log replay + crash-recovery parity) have landed. Phase 3 functional compatibility validation for pool filesystems (`xfs`, `ext4`, `btrfs`) has now landed as well, and the in-tree non-functional benchmark harness is in place. Phase 4 (NFS, through cthon04 + connectable filehandles) and Phase 5 (SMB, through smbtorture baseline + the full Samba VFS module — skeleton, lease pin, fanotify lease-break watcher, and SMB FileId) have both landed. Phase 6 (iSCSI) has also landed — O_DIRECT conformance (6.0), end-to-end LIO file-backed LUN round-trip over smoothfs (6.1), `trusted.smoothfs.lun` pin contract (6.2), target restart / reconnect correctness across `targetctl save / clear / restore` + `iscsid` bounce (6.3), and tierd-side file-backed LUN orchestration that auto-pins `PIN_LUN` on create and clears on destroy (6.5). Individual tier backends remain a separate matrix (`zfs`, `mdadm`, `btrfs`). Phase 7 (appliance integration + kernel update path + signed-module pipeline) has also landed: three apt-installable debs (`smoothfs-dkms`, `smoothfs-samba-vfs`, `tierd`), DKMS-signed modules with an operator MOK enrollment helper, a kernel-upgrade regression harness, a tierd-ui surface for pool + LUN lifecycle including a quiesce/reconcile + movement-log observability page, and a published support matrix + operator runbook at `docs/smoothfs-{support-matrix,operator-runbook}.md`. Active-LUN movement is explicitly unsupported in v1 and has been split into [`smoothfs-active-lun-movement.md`](../completed/smoothfs-active-lun-movement.md), gated on Phase 6 production soak.

---

## Implementation Status

This section tracks what has shipped against this proposal. Updated as work lands.

### Phase 0 — Contract and Conformance Spec — **complete**

[`smoothfs-phase-0-contract.md`](../completed/smoothfs-phase-0-contract.md) closes §0.1–0.10 with unambiguous rulings on object identity, placement authority, the nine-state movement transaction model, per-case concurrency semantics, heat/policy contract, lower-fs capability matrix (XFS-on-LV, ZFS expected values), NFS/SMB/iSCSI invariants, deterministic crash repair per state, and the conformance test plan. Appendix B specifies the schema migration Phase 1 must add.

### Phase 1 — Core Stacked Filesystem — **scaffold landed and validated**

What shipped:

- **Kernel module** under `src/smoothfs/`. `register_filesystem` via `fs_context`, full Phase 1 VFS op surface (lookup / create / mknod / link / symlink / mkdir / rmdir / rename with `RENAME_*` flags / unlink / getattr / setattr / readlink / read_iter / write_iter / fsync / mmap / llseek / splice_read / splice_write / fallocate / iterate_shared / xattr / POSIX ACL passthrough / advisory lock passthrough). Targets kernel **6.18 LTS+** (uses `lookup_one`, `set_default_d_op`, dentry-returning `vfs_mkdir`, new `renamedata` shape, `vfs_mmap`, and the parent-inode + qstr `d_revalidate` signature). Appliance kernel-pin policy: latest stable that OpenZFS DKMS supports — currently 6.18 LTS with OpenZFS 2.4.1 from upstream sources (Debian's `zfs-dkms 2.3.2` is too old, capping at 6.14).
- **Object identity per Phase 0.1**: 128-bit UUIDv7 allocator, persisted in `trusted.smoothfs.oid` xattr on the lower file. Generation counter in `trusted.smoothfs.gen`. Synthesised inode_no = `xxh64(oid) | (1<<63)`, stable across stat calls.
- **Placement log per Phase 0.2**: per-pool append-only log at `<fastest_tier>/.smoothfs/placement.log`, 64-byte fixed records with the schema in `placement.c`. Created at mount; populated by movement (Phase 2).
- **Lower-fs capability gate per Phase 0.6**: mount-time probe by `s_magic`. Phase 1 accepts XFS and ZFS; any other lower returns `EOPNOTSUPP` with a `dmesg` line. Round-trip xattr/ACL/mmap probes deferred to Phase 1.5.
- **Generic netlink family `smoothfs`** registered with the command/attribute schema in `uapi_smoothfs.h`. Inbound commands (REGISTER_POOL, RECONCILE, INSPECT, QUIESCE, REPROBE, MOVE_PLAN, POLICY_PUSH, TIER_DOWN) validate via `nla_policy` and currently ack `-ENOSYS`. Outbound multicast events are no-ops until Phase 2 wires the listener side.
- **Aggregate `statfs(2)`** across all tier targets per §POSIX semantics.
- **Out-of-tree DKMS build** — `Kbuild`, `Makefile`, `dkms.conf`, `debian/postinst`, `debian/prerm`. Module-signing-for-appliance is one of the three open Phase 0.10 blockers.
- **Schema migration** at `tierd/internal/db/migrations/00002_smoothfs_objects.sql` per Phase 0 Appendix B (table + indexes + format/state-vocabulary triggers for smoothfs-backed namespaces).
- **`tierd-cli smoothfs`** subcommand surface (inspect / adopt / scrub / reconcile / quiesce / tier resume) ships as Phase 1 stubs that exit `EX_TEMPFAIL` until netlink is wired.
- **Migration system rewrite** (bundled with Phase 1): replaced the 53-step numeric `var migrations = []string{}` slice + four side-channel `MigrateShares`/`MigrateBackups`/`MigrateBackupRuns`/`mdadm.Migrate` helpers + per-package `migrate()` calls in smart with `goose` driven by `embed.FS` of versioned `.sql` files under `tierd/internal/db/migrations/`. Single `00001_baseline.sql` baseline; new schema lands as new files. `main.go` collapses to one `store.Migrate()` call.

End-to-end validation on the SmoothNAS test server (Debian 13 / kernel 6.12.74):

| Operation | Result |
|---|---|
| `mount -t smoothfs -o pool=test,tiers=/fast:/slow none /mnt` | OK; `dmesg` shows `mounted pool 'test' with 2 tier(s)` |
| `df` | Aggregate 896M = 2×448M across tier targets |
| write/read 16 MiB random data | md5sum matches between smoothfs view and lower file (byte-correct passthrough) |
| `mkdir`/`rmdir`/`rename`/`unlink` | OK |
| `symlink`/`readlink` | OK |
| `link` (hardlink) | OK; both names share inode_no with nlink=2 |
| `setfattr user.tag=hello` via smoothfs | Visible on lower; `trusted.smoothfs.oid` carries valid UUIDv7 |
| `stat` repeated | inode_no stable across calls |
| `umount` + `rmmod smoothfs` | Clean; refcount returns to zero, `dmesg` shows `unloaded` |

What did **not** ship in Phase 1 (still gating later work):

- **Phase 0.10 operational blockers remain open**: named VFS / stacked-filesystem reviewer not yet identified; performance baseline against the current FUSE stack not yet measured; signed-module pipeline for the appliance not yet stood up.
- **Capability probe is structural only.** Phase 1 accepts/rejects by `s_magic` and assumes the contract values from Phase 0.6. Round-trip probes (write+read xattrs, ACL set+get, mmap-coherence stress, sparse `SEEK_DATA`/`SEEK_HOLE` round-trips, `FS_IOC_GETVERSION`, etc.) land in Phase 1.5.
- **No automatic promotion/demotion.** The placement log is created but no records are written by the create path; `smoothfs_objects` rows aren't populated. Heat sampling counters exist on the inode (`open_count`, `read_bytes`, `write_bytes`, `last_access_ns`) but nothing drains them. All Phase 2.
- **Netlink in/out is stubbed.** Inbound commands ack `-ENOSYS`; outbound events are no-op. tierd has no listener yet. `tierd-cli smoothfs` subcommands print what they would do and exit `EX_TEMPFAIL`.
- **No conformance suite run.** `xfstests`, `pjdfstest`, the bespoke crash-injection suite, `cthon04`, `smbtorture`, LIO self-tests — none integrated into CI yet. Validation above is hand-run, not regression-gated.
- **No protocol exports.** NFS/SMB/iSCSI are Phases 4–6.
- **No movement, snapshots, fanotify-to-Samba, or mdadm-log state-vocabulary tightening** beyond the trigger in `00002_smoothfs_objects.sql`.

### Phase 2 — Heat Engine and Journaled Movement — **landed**

What shipped:

- **Kernel-side heat drain** (`src/smoothfs/heat.c`): per-pool `delayed_work` walks the sb's inode list every 30 s, computes `(current - last_drained)` deltas across the four counters from §0.5 (`open_count`, `read_bytes`, `write_bytes`, `last_access_ns`), and emits up to 256 packed 48-byte `heat_sample_record`s per `SMOOTHFS_EVENT_HEAT_SAMPLE` multicast netlink message.
- **Kernel-side movement state machine** (`src/smoothfs/movement.c`): `MOVE_PLAN` / `MOVE_CUTOVER` / `movement_abort` handlers. Plan refused if `pin_state != none`, `nlink > 1`, any writable shared mapping, or any open fd (Phase 0 §0.4 first cut — the per-fd reissue protocol that lifts the open-fd restriction is a Phase 2.1 follow-up). Cutover atomically swaps `lower_path` on the inode, bumps `cutover_gen`, and emits `SMOOTHFS_EVENT_MOVE_STATE`. Hardlink pin propagates correctly through `link(2)`/`unlink(2)`.
- **Real multicast netlink emit** in `src/smoothfs/netlink.c`. Phase 1 made the emit path a no-op because no multicast group was registered (would WARN); Phase 2 registers group `events` and the kernel's emits go through.
- **Userspace control plane** at `tierd/internal/tiering/smoothfs/`:
  - `client.go` — generic-netlink wrapper (`mdlayher/genetlink`), joins the `events` group, exposes `MovePlan` / `MoveCutover` / `Inspect` / `Quiesce` / `Reconcile` typed methods.
  - `heat.go` — EWMA aggregator. Per-object weighted score (`read=1×`, `write=2×`, `open=4 KiB`-equivalent), merged into `smoothfs_objects.ewma_value` with the gap-since-last-sample exponentially decayed at the configured half-life (24 h default per §0.5).
  - `planner.go` — periodic per-pool walk (15 min default). Sorts placed objects per tier by EWMA, picks the bottom 20 % for demotion / top 20 % for promotion, applies anti-thrash gates (min residency, cooldown, hysteresis, pin states). Emits `MovementPlan` items onto a channel.
  - `worker.go` — consumes plans. Issues `MOVE_PLAN`, copies the file via `io.Copy` with sha256 streaming, re-reads dest for verify, issues `MOVE_CUTOVER`, unlinks source. Logs every state transition into `smoothfs_movement_log`.
  - `recovery.go` — on tierd start, scans `smoothfs_objects` for non-terminal `movement_state` and applies the deterministic Phase 0 §0.8 algorithm (pre-cutover → rollback to `placed`; at/after cutover → forward to `placed` on dest tier).
  - `service.go` — top-level Service started from `cmd/tierd/main.go`. `ErrNotLoaded` surfaces "kernel module not loaded" cleanly so tierd keeps running with smoothfs disabled.
- **Schema migration `00003_smoothfs_movement.sql`** adding `ewma_value` / `last_heat_sample_at` / `last_movement_at` / `failure_reason` columns plus the `smoothfs_movement_log` audit table; seeds `control_plane_config` with the §0.5 anti-thrash defaults.
- **`tierd-cli smoothfs`** subcommands wired to real netlink: `inspect` / `promote` / `demote` / `cutover` (operator override) / `quiesce` / `reconcile`.

End-to-end validation on the test server (Debian 13 / kernel 6.18.22 / OpenZFS 2.4.1):

| Step | Result |
|---|---|
| Mount smoothfs with two XFS lowers + pool UUID | OK |
| Write `mnt/greet.txt` | object_id allocated, valid UUIDv7 in `trusted.smoothfs.oid` |
| `tierd-cli smoothfs inspect` | returns kernel state via netlink |
| `tierd-cli smoothfs promote --to 1 --seq 1` | state goes `placed` → `plan_accepted`, `intended_tier=1` |
| Manual `cp` to dest tier (worker simulation) | OK |
| `tierd-cli smoothfs cutover --seq 1` | state goes → `cutover_in_progress` → `switched`, `current_tier=1`, `cutover_gen=1` |
| `cat mnt/greet.txt` after cutover | content readable via smoothfs (now from dest tier) |

What did **not** ship in Phase 2:

- Operationally not validated this session (structurally sound, needs a running tierd against the kernel for the full closed-loop check): auto planner cycle, heat sample propagation into `ewma_value`, crash recovery against a real mid-move kill.
- Per-fd reissue protocol — Phase 2 refuses movement when any fd is open. Phase 2.1 lifts this with the cutover-gen check on each I/O syscall.
- `MAP_SHARED|PROT_WRITE` revocation during forced movement (the kernel currently refuses such moves outright per the conservative Phase 2 cut).
- `RelPath` tracking on `smoothfs_objects` — the worker currently uses the object_id hex as a placeholder rel-path (Phase 2.1 wires the real path).

### Phase 2.1 — Carve-outs from Phase 2 — **landed**

What shipped:

- **`compat.h`** at `src/smoothfs/`: every `#if LINUX_VERSION_CODE` shim moved into one header. `SMOOTHFS_KERNEL_FLOOR_MAJOR`/`_MINOR` macros are the single knob; bumping to a newer kernel is editing those + sweeping dead pre-floor branches + adding `#if LINUX_VERSION_CODE >= KERNEL_VERSION(N,0,0)` blocks for any new-API adoption. Helpers cover: `set_default_d_op`, `vfs_iter_read/write`, `vfs_mmap`, `lookup_one`, `vfs_mkdir` replacement-dentry handling, `d_revalidate` parent-inode signature, `renamedata` field shape.
- **Schema migration `00004_smoothfs_rel_path.sql`** adds `rel_path TEXT` to `smoothfs_objects` with an index on `(namespace_id, rel_path)`.
- **Kernel `rel_path` emission via `INSPECT`**: `dentry_path_raw()` against `sb->s_root` returns the namespace-relative path; new `SMOOTHFS_ATTR_REL_PATH` attribute carries it back to userspace.
- **Userspace worker uses real `rel_path`**: replaces the Phase 2 placeholder where the worker would have copied to dest with the oid-hex as filename. Worker now calls `Inspect()` to refresh `rel_path` when the planner snapshot lacks one and persists the result.
- **Per-fd reissue protocol** (Phase 0 §0.4 marquee): each smoothfs file struct's `private_data` carries a `smoothfs_file_info` with `lower_file`, `lower_gen`, `open_flags`, `open_cred`, and a per-file mutex. `smoothfs_lower_file(file)` is the single accessor every file_op uses; if the inode's `cutover_gen` has advanced since open, it lazily reopens against the current `lower_path`. `movement.c` lifts the no-open-fd restriction for read-only opens (checks `i_writecount > 0` instead of `open_count > 0`).
- **Granular writable-mmap tracking**: a wrapper `vm_operations_struct` chain installed at `smoothfs_mmap` time forwards open/close to the lower's `vm_ops` while incrementing/decrementing `si->writable_shared_mappings` for `VM_SHARED|VM_WRITE` VMAs. `MOVE_PLAN` refuses immediately if any such mapping exists; `smoothfs_mmap` refuses new ones while a movement is in flight.
- **Netlink command-vs-subscription split** in `tierd/internal/tiering/smoothfs/client.go`: separate `genetlink.Conn` for unicast Send (no multicast subscription) and for event Receive (joined to `events` group). Fixes the strict-sequence validator tripping when multicast events landed in the receive buffer of an outstanding command. The void-reply handlers in `netlink.c` now just `return 0` and let the kernel's auto-ACK do its job.

End-to-end validated on test server (Debian 13 / 6.18.22): `INSPECT` returns correct `rel_path` for files at root, one level deep, and three levels deep. With `mnt/root.txt` held open for read on `fd 9`, `MOVE_PLAN promote 0→1` succeeds (Phase 2 would have refused with `EBUSY`), `MOVE_CUTOVER` commits the swap, `cat mnt/root.txt` returns content from the new tier, and the originally-held `fd 9` continues to read successfully via lazy reissue.

### Phase 2.2 — Writable-fd movement — **landed**

What shipped:

- **Write-barrier drain at cutover** (`src/smoothfs/movement.c`, `file.c`): `write_iter` bookends each write with `inflight_writes` inc/dec; cutover sets `CUTOVER_IN_PROGRESS`, drops `inode_lock`, waits on `cutover_wq` for `inflight_writes == 0` (5s bounded), re-takes lock, validates state, swaps `lower_path`, wakes waiters. New writes during the drain sleep on `cutover_wq`. Drain timeout/signal rolls back to `COPY_VERIFIED` so tierd retries.
- **Writers no longer block `MOVE_PLAN`** — `i_writecount > 0` check removed from `smoothfs_can_move`. Any open file with no writable shared mmap can now be moved.
- **`SMOOTHFS_CMD_REVOKE_MAPPINGS`** (=14) new netlink command. Handler calls `unmap_mapping_range` on the smoothfs inode; PTE zap forces faults that re-enter via the lower's `vm_ops`, and our wrapper close hooks dec `writable_shared_mappings` as VMAs tear down. Admin override per Phase 0 §0.4.
- **mtime-stable cutover** (`tierd/internal/tiering/smoothfs/worker.go`): worker records source mtime+size before copy, re-stats after copy+verify, aborts with `errSourceRaced` if either changed. tierd's planner re-picks the object on its next cycle.
- **Bug fix**: cutover left the smoothfs dentry's `d_fsdata` pointing at the freed old lower dentry; `d_release` at umount then `dput`'d a stale pointer, tripping a `WARN_ON` in `dput`. Cutover now updates `d_fsdata` via `d_find_alias` and dputs the old fsdata pointer in the same critical section.
- **Bug fix**: `vm_ops` chain installed by `smoothfs_mmap` was leaked at VMA close; `smoothfs_vma_close` now restores `vma->vm_ops` to the lower's ops and `kfree`s the chain.

End-to-end validated on the test server (Debian 13 / 6.18.22):

| Scenario | Result |
|---|---|
| Hold writable fd via `exec 8> mnt/writer.txt`, run promote+cutover | PLAN accepted (Phase 2.1 would have refused), cutover committed |
| Write through held fd post-cutover | Content lands on the new tier (verified via direct lower read) |
| Tight-loop writer (200 iter `echo "iter-N" > mnt/race.txt`) during promote+cutover | Cutover succeeded; final content `iter-59` shows mid-stream cutover with subsequent writes targeting new tier |
| Clean `umount` + `rmmod` post-cutover | No `WARN` (Phase 2.2 first pass had a `dput` WARN; fixed) |

What did **not** ship in Phase 2.2:

- Operator-side `tierd-cli smoothfs revoke <oid>` for the revocation override (kernel command exists; CLI wiring lands in Phase 2.3).
- Per-VMA private state for the `vm_ops` chain — `fork()`d mmaps with shared chain pointers may double-close. Worth fixing under `vma->vm_private_data` in Phase 2.3.
- Closed-loop heat → EWMA propagation test against running tierd (kernel emit + tierd consume both shipped, the integration is structurally sound but not loop-tested this session).
- Mid-move-kill crash recovery test against a running tierd (recovery code shipped; not exercised end-to-end).

### Phase 2.3 — SmoothKernel extraction + carry-overs — **landed**

What shipped:

- **`RakuenSoftware/smoothkernel`** repo populated with the kernel-build harness that SmoothNAS used through Phase 1/2/2.1/2.2. Now the source of truth for any future Smooth* appliance OS that needs to build its own kernel + OpenZFS:
  - `recipes/build-kernel.sh` — kernel.org tarball → `bindeb-pkg`, parameterized by `KERNEL_VERSION` / `LOCALVERSION` / `CONFIG_SOURCE`. Strips DEBUG_INFO_BTF / DWARF-4/5 / trusted-keys by default; verifies sha256 against kernel.org's signed sums.
  - `recipes/build-zfs.sh` — OpenZFS source → `deb-utils` + `deb-dkms`, parameterized by `ZFS_VERSION`. Output kernel-independent; DKMS rebuilds on the appliance.
  - `templates/dkms.conf.in`, `debian-postinst.in`, `debian-prerm.in` — Debian/DKMS skeleton for any out-of-tree module.
  - `templates/compat.h.in` — the kernel-version shim header pattern smoothfs proved out in Phase 2.1, parameterized by `MODULE_NAME` / `MODULE_PREFIX` / `KERNEL_FLOOR_MAJOR`/`MINOR`.
  - `docs/bumping-kernel.md` — runbook for moving the kernel pin (point bumps vs cross-LTS vs major), including the OpenZFS-Linux-Maximum coordination rule.
  - `docs/per-os-config.md` — naming conventions and per-OS layout so adding SmoothHTPC / SmoothRouter is mechanical.
  - `docs/signing.md` — placeholder for the cross-Smooth* signing pipeline (Phase 0.10 blocker work).
- **Per-VMA private state for the mmap wrapper** (`src/smoothfs/file.c`): replaced the Phase 2.2 shared-chain pattern with `vma->vm_private_data` carrying a `smoothfs_vma_priv` per VMA. `fork()` triggers our `.open` which allocates a fresh chain for the child VMA so two `.close` calls don't double-free. Wraps the lower's `.fault` and `.page_mkwrite` too, swapping `vm_private_data` back to the lower's around each forwarded call. **Known limitation (fixed in Phase 2.4):** the wrapper set `vma->vm_ops` after `vfs_mmap` returned, which conflicts with lowers that use the newer `mmap_prepare` API (XFS in 6.18).
- **`tierd-cli smoothfs revoke --pool --oid`** wired to `SMOOTHFS_CMD_REVOKE_MAPPINGS` (the kernel command shipped in Phase 2.2; Phase 2.3 makes it operator-callable).

What did **not** ship in Phase 2.3 (carry-overs to Phase 2.4):

- mmap wrapper that cleanly interoperates with `mmap_prepare`-using lowers
- Closed-loop heat → EWMA propagation test against running tierd
- Mid-move-kill crash recovery validation against running tierd
- Migration of SmoothNAS's documented build steps to actually consume `smoothkernel` recipes

### Phase 2.4 — Close out Phase 2 — **landed**

What shipped:

- **mmap rewritten around `vma_set_file`** (`src/smoothfs/file.c`): `smoothfs_mmap` now rebinds `vma->vm_file` to the lower via `vma_set_file()` and forwards through `vfs_mmap()`. Two earlier attempts — a shared `vm_operations_struct` chain (Phase 2.2) and a per-VMA `vm_private_data` chain (Phase 2.3) — both set `vma->vm_ops` after the lower's `.mmap`/`.mmap_prepare` ran and were unsafe against lowers that use the newer `mmap_prepare` API (XFS in 6.18). The new path matches overlayfs' `ovl_mmap` pattern: rebind the backing file, let the lower install its own `vm_ops`, and rely on the kernel's own counters for any per-inode accounting. Since `vma_set_file()` links the VMA into the lower's `i_mmap`, movement now gates on `mapping_writably_mapped()` of the lower's mapping — `i_mmap_writable` is the authoritative writable-shared-mmap count, automatically correct across `mmap_prepare`-using lowers, `fork()`, and `munmap()`. The per-inode `writable_shared_mappings` atomic is removed; `smoothfs_revoke_mappings` now zaps the lower inode's `i_mapping` (where the VMAs actually live after `vma_set_file`).
- **`HeatAggregator` unit tests** (`tierd/internal/tiering/smoothfs/heat_test.go`): first-sample scoring (`read + 2×write + 4096×opens`), one-half-life decay merge, unknown-object skip (reconcile-first invariant from Phase 2 §0.5), empty-batch no-op. Covers the heat → EWMA propagation path without requiring a running kernel.
- **`Recover` unit tests** (`tierd/internal/tiering/smoothfs/recovery_test.go`): pre-cutover states (`plan_accepted`..`copy_verified`) roll back to `placed` on source with `intended_tier_id` cleared; post-cutover states (`cutover_in_progress`, `switched`, `cleanup_in_progress`) roll forward to `placed` on destination and bump `last_committed_cutover_gen`; terminal states (`placed`, `cleanup_complete`, `failed`, `stale`) are untouched; post-cutover rows missing `intended_tier_id` are left as-is. Covers the mid-move-kill recovery path at the DB-state layer.
- **Kernel + ZFS build migrated to `smoothkernel`** (`docs/OPERATIONS.md` §Kernel and OpenZFS): SmoothNAS's operations guide now points at `recipes/build-kernel.sh` and `recipes/build-zfs.sh` in the `smoothkernel` repo for the kernel + OpenZFS DKMS build rather than duplicating the commands inline. The bump-the-kernel-pin runbook is the external `smoothkernel/docs/bumping-kernel.md`.

What did **not** ship in Phase 2.4 (carry-overs to Phase 2.5):

- End-to-end closed-loop heat and mid-move-kill tests driven against an actually-running tierd against a real smoothfs mount.
- Revoke that forces holders off the mapping without operator intervention.

### Phase 2.5 — Auto-discovery + revoke + E2E fixture — **landed**

What shipped:

- **Pool auto-discovery from `SMOOTHFS_EVENT_MOUNT_READY`** (`src/smoothfs/netlink.c`, `tierd/internal/tiering/smoothfs/{events,service}.go`): mount-ready multicast now carries the mounted tier list (rank + lower path + caps). tierd decodes that payload, looks up the matching `managed_namespaces` row by `backend_ref = <pool-uuid>` (with pool-name fallback), resolves the placement-domain `tier_targets`, and self-registers the pool into the planner without an operator-side `RegisterPool()` call.
- **Quiesced-mmap revoke path** (`src/smoothfs/{file,movement,netlink}.c`, `tierd/cmd/tierd-cli/main.go`): `SMOOTHFS_CMD_REVOKE_MAPPINGS` now sets a per-inode `mappings_quiesced` bit before zapping the lower mapping's PTEs. `smoothfs_mmap()` refuses new writable shared mappings with `-EBUSY` while that bit is set; `MOVE_PLAN` success and pool-wide `reconcile` clear it. On the operator side, `tierd-cli smoothfs revoke` inspects the object's current lower path and tears down processes still holding writable shared mappings of that file by scanning `/proc/*/maps`, sending `SIGTERM`, then escalating to `SIGKILL` after a short grace period. This gives an operator-free drain while preserving the Phase 2.4 `vma_set_file()` design.
- **Host-gated Go E2E fixture** (`tierd/internal/tiering/smoothfs/e2e_test.go`): `go test -tags=e2e ./internal/tiering/smoothfs` now contains a real single-host harness that, when run as root with `SMOOTHFS_KO=/path/to/smoothfs.ko` and the usual loop/XFS tools installed, stands up two XFS loopback lowers, loads `smoothfs.ko`, mounts the pool, starts tierd's smoothfs `Service` against a scratch SQLite DB, and asserts that mount-ready auto-discovery actually registers the pool.

### Phase 2.6 — Live validation on top of the E2E fixture — **landed**

What shipped:

- **Live heat → EWMA → planner E2E** (`tierd/internal/tiering/smoothfs/e2e_test.go`: `TestE2EHeatFlowsIntoPlanner`): the host-gated fixture now creates a real file through the mounted smoothfs namespace, seeds the corresponding `smoothfs_objects` row from the lower file's `trusted.smoothfs.oid`, forces an immediate kernel heat drain via `RECONCILE`, waits for `HeatAggregator.Apply` to land a positive `ewma_value`, then asserts the live planner/worker cycle moves the object from the fast tier to the slow tier. This exercises the live netlink path rather than only the unit-tested endpoints.
- **Mounted-pool restart recovery + retry E2E** (`tierd/internal/tiering/smoothfs/e2e_test.go`: `TestE2ERestartReplayPreCutoverRollback`): the fixture now restarts the smoothfs `Service` against an existing mounted pool after issuing a real pre-cutover `MOVE_PLAN`, verifies remount-time convergence back to `placed` on the source tier, and then proves a retry move succeeds end-to-end against the live mount.

Carry-over note:

- The literal **mid-move-kill + kernel placement-log replay** scenario from the original Phase 2.6 text was re-scoped into Phase 2.7 because the missing work was a kernel recovery feature rather than additional test harnessing. That follow-up is now landed.

### Phase 2.7 — Kernel placement-log replay + crash-recovery parity — **landed**

What shipped:

- **Mount-time `placement.log` replay in the kernel** (`src/smoothfs/placement.c`, `src/smoothfs/super.c`): mount now parses the append-only placement log, keeps the latest record per `object_id`, normalizes non-terminal states using the same crash-repair rules as userspace (`plan_accepted`..`copy_verified` roll back to source; `cutover_in_progress`..`cleanup_in_progress` roll forward to destination), scans all lower tiers recursively for files carrying `trusted.smoothfs.oid`, and preloads authoritative inodes into the in-memory OID map. Normalized records are re-appended as `placed` so the log converges on a post-recovery truth.
- **Replayed-object namespace recovery** (`src/smoothfs/{inode,netlink,super}.c`): replayed inodes now cache a namespace-relative `rel_path`. `INSPECT` falls back to that cached path when no live dentry alias exists yet, and `lookup()` uses the cached `rel_path` map when the fastest-tier directory tree no longer contains the authoritative file after remount.
- **Crash-recovery parity E2E coverage** (`tierd/internal/tiering/smoothfs/e2e_test.go`): the host-gated fixture now covers both replay directions against a live smoothfs mount.
  - `TestE2ERestartReplayPreCutoverRollback` issues a real `MOVE_PLAN`, seeds the matching DB row as `plan_accepted`, restarts/remounts, and verifies both kernel `INSPECT` state and SQLite converge back to `placed` on the source tier before a retry move succeeds.
  - `TestE2ERestartReplayPostCutoverForward` stages a destination copy, issues `MOVE_CUTOVER` without source cleanup, seeds the DB row as `cleanup_in_progress`, restarts/remounts, and verifies kernel + SQLite converge to `placed` on the destination tier while namespace reads come from the authoritative destination copy even with the stale source copy still present.

Validation completed for Phase 2.7:

- The live kernel-module replay cases were compiled and executed on the prepared SmoothNAS host at `admin@192.168.0.214` (matching `smoothnas-lts` headers present under `/lib/modules/$(uname -r)/build`). Both `TestE2ERestartReplayPreCutoverRollback` and `TestE2ERestartReplayPostCutoverForward` passed there against the built `smoothfs.ko`.

### Phase 3 — Lower-FS Compatibility Expansion — **landed**

What shipped:

- **Mount-time compatibility-gate expansion** (`src/smoothfs/probe.c`): the lower-fs capability probe no longer blanket-rejects everything outside the Phase 1 pair. The smoothfs pool-lower matrix now explicitly includes `xfs`, `ext4`, and `btrfs` for Phase 3 validation, while filesystems outside the declared compatibility matrix still fail closed.
- **Full functional compatibility validation completed for the current pool-filesystem set**: on the prepared host at `admin@192.168.0.214`, smoothfs-over-`xfs`, smoothfs-over-`ext4`, and smoothfs-over-`btrfs` all completed the live mount/create/read/stat/rename/unlink matrix using loopback-backed lowers and the built `smoothfs.ko`.
- **In-tree compatibility coverage expanded** (`tierd/internal/tiering/smoothfs/e2e_test.go`): the host-gated E2E harness now parameterizes lower filesystems and carries explicit matrix coverage for `xfs`, `ext4`, and `btrfs`, plus a `btrfs`-specific reflink/subvolume scenario.
- **`btrfs` reflink + subvolume path fixed and hardware-validated** (`src/smoothfs/file.c`, `tierd/internal/tiering/smoothfs/e2e_test.go`): smoothfs now exposes a real `remap_file_range` hook so `FICLONE` unwraps to the lower files before the VFS clone path runs. The live `TestE2EBtrfsReflinkAndSubvolume` case now passes on `admin@192.168.0.214`.
- **Rename path cleaned up for all validated pool filesystems** (`src/smoothfs/inode.c`): lower rename now follows the kernel stacked-filesystem locking protocol (`lock_rename_child` + parentage validation before `vfs_rename`), and the live `xfs` / `ext4` / `btrfs` compatibility reruns completed without the earlier `smoothfs_rename` → `d_move` kernel warning.
- **Phase 3 non-functional harness landed in-tree** (`tierd/internal/tiering/smoothfs/e2e_test.go`): `TestE2EPhase3NonFunctionalTargets` now measures CREATE p99, steady-state sequential read/write throughput, metadata cache-hit latency, and bulk-copy CPU overhead against the native fast-tier XFS baseline through the live mount. The case is host-gated behind `SMOOTHFS_PERF=1` so it can be used as a deliberate live validation gate instead of running in every e2e pass.
- **Memory-budget guard is now compile-time enforced** (`src/smoothfs/module.c`): module build now asserts `sizeof(struct smoothfs_inode_info) <= 4096`, so the smoothfs-owned per-inode state cannot silently drift past the Phase 3 budget ceiling.

Validation completed for Phase 3:

- First live-host non-functional pass on 2026-04-17 against `6.18.22-smoothnas-lts-smoothnas-lts`.
- The STAT p99 gate was missed at 28.56x; root-caused to a dcache-invisibility bug in `smoothfs_compat_lookup` that fed `lookup_one` the upper dentry's `d_name`, which `lookup_noperm_common` then rewrote to the *lower* parent's hash. Every path walk's `__d_lookup_rcu` missed the cached upper dentry, and every stat re-entered `smoothfs_lookup`. A latent `smoothfs_rename` `d_fsdata` bug surfaced once the dcache actually worked (`d_move(old, new)` keeps `old_dentry` as the surviving dentry; we had been clearing its `d_fsdata`).
- Both fixes landed in PR #216 ("smoothfs: fix STAT p99 dcache invisibility + Phase 4-prep rhashtable"), which also landed the Phase 4-prep rhashtable OID map, `vfs_getattr_nosec` passthrough, `d_revalidate` fast-path, `.permission` removal, per-sb SRCU drain, and a lower-inode → ino_no cache.
- Re-run on a quieter 16 vCPU / 32 GiB lab VM with the SmoothNAS sysctl tuning drop-in (`90-smoothnas-net.conf`: BBR/FQ, `vm.dirty_*` bounded), 10 runs, middle-6 trimmed means:

| Metric | Target | Pre-fix (2026-04-17) | Post-fix / post-tuning |
|---|---|---|---|
| CREATE p99 | ≤ 2.0x | 1.02x PASS | 0.838x PASS |
| Sequential read | ≥ 0.95x | 1.06x PASS | 0.967x PASS |
| Sequential write | ≥ 0.90x | 0.92x PASS | 1.018x PASS |
| STAT p99 | ≤ 1.5x | **28.56x FAIL** | **0.993x PASS** |
| CPU ratio | ≤ 1.10x | 1.07x PASS | 1.070x PASS |

All 5 gates pass. The STAT p99 change is the only structurally meaningful move; CREATE / read / write / CPU deltas are within measurement noise on tmpfs- vs loopback-backed runs.

Two Phase 4 carry-overs that were raised in PR #216 and investigated under PR #217:

- **Read ratio**: further investigation via ftrace + a direct C read benchmark (64 MiB sequential reads, 5 runs per side) showed smoothfs and native XFS within noise — smoothfs even came out ~5% ahead on some runs (spurious). Per-call `smoothfs_read_iter` overhead measured at ~0.3 µs against `vfs_iter_read`'s ~55 µs per 1 MiB call (< 1% structural). The `~0.03–0.07` gap seen in the Phase 3 Go harness is test-harness variance (GC pauses leaking between the warm and timed pass; interleaved native/smooth measurement would flatten it). Not worth a kernel-side optimization pass.
- **Defer `write_oid_xattr` on CREATE**: ftrace on steady-state CREATE shows `__vfs_setxattr` at ~0.9 µs of a ~5.5 µs total smoothfs_create (vs `vfs_create` at 2–3 µs and the rest of iget at ~2–3 µs). Deferring to a background worker would save ~1 µs at the cost of changing Phase 0 §0.1's "OID is persisted" contract to "OID is eventually persisted with post-crash replay" (non-trivial for hardlink consistency and NFS handle stability, forthcoming in Phase 4). Not worth the contract change for ~1 µs; left as-is.

Current compatibility matrix:

| smoothfs pool filesystem | Current status | Validation currently in-tree |
|---|---|---|
| `xfs` | Phase 3 functionally validated | host-gated mount/create/read/stat/rename/unlink coverage on live hardware |
| `ext4` | Phase 3 functionally validated | host-gated mount/create/read/stat/rename/unlink coverage on live hardware |
| `btrfs` | Phase 3 functionally validated | host-gated mount/create/read/stat/rename/unlink plus explicit reflink/subvolume coverage on live hardware |
| `zfs` | Baseline supported lower from Phase 1/2 | existing baseline support; not part of the new Phase 3 expansion set |

Separate SmoothNAS tier-backend matrix:

| Tier backend | Role |
|---|---|
| `zfs` | individual tier backend |
| `mdadm` | individual tier backend |
| `btrfs` | individual tier backend |

### Phase 4 — **landed (4.0–4.5)**

NFS export support up through a clean `cthon04` run, plus connectable filehandles:

- **4.0** — `s_export_op` stub wiring, Phase 4 addenda in the Phase 0 contract.
- **4.1** — end-to-end `encode_fh` / `fh_to_dentry` round-trip against nfsd; OID-based resolution via the rhashtable built in Phase 4-prep.
- **4.2** — `NFS` read/write correct across `MOVE_CUTOVER`; open fds survive the swap per the Phase 2 movement invariants.
- **4.3** — filehandles survive server umount and module reload (OID is the only durable key).
- **4.4** — `cthon04` clean run (basic + general + special, `NFSv3` and `NFSv4.2`). Two correctness fixes fell out of the run: `nfsd GETATTR NULL-deref after NFS UNLINK` (keep `si->lower_path` alive until `evict_inode`), and `drop_nlink` instead of `clear_nlink` on unlink so hardlinks observe the correct transition and `vfs_link`'s `i_nlink == 0` guard stops refusing subsequent links.
- **4.5** — connectable filehandles. New wire type `FILEID_SMOOTHFS_OID_CONNECTABLE` (`0x54`, 40 bytes: `fsid | object_id | gen | parent_object_id`), emitted when `encode_fh` is called with `parent != NULL`. `fh_to_parent` and `get_parent` wired: `fh_to_parent` resolves the trailing 16 bytes back through the sb's OID rhashtable, `get_parent` walks the lower dentry's parent and re-`iget`s the smoothfs inode. Round-trip verified by `src/smoothfs/test/connectable_fh.c` (`name_to_handle_at(AT_HANDLE_CONNECTABLE)` → `open_by_handle_at`).

### Phase 5 — **landed (5.0, 5.1, 5.2, 5.3-kernel, 5.4-baseline, 5.5-triage, 5.6, 5.7-citations, 5.8.0-build-env, 5.8.1-skeleton, 5.8.2-lease-pin, 5.8.3-fanotify-watcher, 5.8.4-fileid)**

SMB support, staged so Samba VFS-module work lands against a proven kernel-side contract:

- **5.0** — kernel-side SMB identity and lease-pin surface. Two reserved xattrs go live:
  - `trusted.smoothfs.fileid` (read-only, 12 bytes: `inode_no (u64 LE) | gen (u32 LE)`) — the Phase 0 contract's SMB FileId source, computed from `si` without a lower round-trip so Samba never observes an inconsistent FileId.
  - `trusted.smoothfs.lease` (1-byte 0/1 toggle; removexattr also clears) — flips `si->pin_state` between `PIN_NONE` and `PIN_LEASE`, and only between those two so unrelated pins (`PIN_HARDLINK`, `PIN_LUN`, heat-derived pins) aren't clobbered. The movement scheduler already short-circuits on any non-`PIN_NONE` pin, so no scheduler change was needed.
  - Round-trip verified by `src/smoothfs/test/smb_identity_pin.c` (15 assertions: size, EPERM on fileid write, PIN_LEASE round-trip via both setxattr and removexattr, value-validation rejection).

- **5.1** — stock Samba share over a smoothfs mount works end-to-end: `smbd` standalone on a loopback-only port, `smbclient` + `mount.cifs` both round-trip `put`/`get`/rename/mkdir/rmdir, and `trusted.smoothfs.fileid` is byte-identical before and after rename through both client paths. Driven by `src/smoothfs/test/smb_roundtrip.sh` (17 assertions). No smoothfs Samba VFS module yet — anything that works here must continue to work once the module lands.

- **5.2** — SMB-side movement invariants: analog of Phase 4.2 for CIFS. `TestE2ESMBMovementAcrossOpenFD` holds a long-lived CIFS fd across a `MOVE_PLAN`+`MOVE_CUTOVER` driven from Go via netlink, confirms reads continue to return the right bytes through the same fd, and confirms post-cutover writes land on the new tier (verified at slow-tier, smoothfs-mount, and CIFS-stat observation points). `TestE2ESMBLeasePinSkipsMovement` sets `trusted.smoothfs.lease`, verifies `MOVE_PLAN` is refused without advancing `movement_state` / `intended_tier`, then clears the lease and verifies the pin transitions back to `PIN_NONE` — the runtime side of the Phase 5.0 contract that the Samba VFS module will toggle. Both tests reuse the existing Phase 4 `e2eEnv` / `openE2EClient` infrastructure plus a new `smbShareSetup` helper that stands up an isolated `smbd` on loopback:8445 alongside a CIFS mount.

- **5.3 (kernel half)** — forced-move + fsnotify lease-break signal. New netlink `SMOOTHFS_ATTR_FORCE` (u8 boolean) on `MOVE_PLAN`; `force=true` is the *only* way to bypass a pin, and only `PIN_LEASE` is bypassable (HARDLINK / LUN / heat-derived pins keep refusing the plan). On the subsequent cutover, `smoothfs_movement_cutover` clears the `PIN_LEASE` back to `PIN_NONE` and fires `fsnotify(inode, FS_MODIFY)` so any fanotify/inotify listener sees a lease-break signal before the new tier's bytes become client-visible. `tierd-cli smoothfs promote --force` exposes the switch. Userspace reference implementation `src/smoothfs/test/lease_break_agent.c` stands up a fanotify watcher on the mount that `removexattr trusted.smoothfs.lease` on every event; `TestE2ESMBForcedMoveBreaksLease` drives the full path (set lease → spawn agent → unforced plan refused → forced plan accepted → stage + cutover → agent clears lease → `pin_state == PIN_NONE`, `movement_state == switched`).

- **5.3 (Samba VFS module half)** — deferred. Scoped standalone in [`smoothfs-samba-vfs-module.md`](../completed/smoothfs-samba-vfs-module.md): Debian `samba-dev` ships public APIs only; the VFS SDK requires Samba's full `source3/` tree and a waf build. The reference agent shipped in 5.3-kernel is exactly the behaviour the module will plug into `SMB_VFS_SET_LEASE` + a tevent-integrated fanotify watcher. `TestE2ESMBForcedMoveBreaksLease` is the regression gate the module must keep passing.

- **5.4 (baseline)** — `smbtorture` harness landed: `src/smoothfs/test/smbtorture.sh` stands up the same isolated `smbd` + smoothfs topology as 5.1 and runs a curated MUST_PASS set of the Samba `smbtorture` catalog (15 tests covering SMB1 `base.*`, SMB1 `raw.*`, and SMB2 file-op groups). **All 15 pass against the current kernel + stock Samba — no smoothfs fixes required.** Five `KNOWN_ISSUES` tests fail and are documented inline in the harness with per-test triage: `raw.rename::directory-rename` (Samba-level path quirk with open children), `raw.search::ea-list` (EA round-trip through SEARCH — needs the VFS module), `smb2.rw::invalid` (expects `DISK_FULL`; quota plumbing is Phase 7), `smb2.getinfo`/`smb2.setinfo` (change_time round-trip through SMB2 SET/GET_INFO — targeted smoothfs fix blocked on the VFS module for a clean end-to-end). A "VFS_MODULE_REQUIRED" skip list (leases, oplocks, ACLs, charset) is also documented for visibility; those move under the [Samba VFS module proposal](../completed/smoothfs-samba-vfs-module.md).

- **5.5** — triage of Phase 5.4's `KNOWN_ISSUES`. `src/smoothfs/test/smbtorture_xfs_baseline.sh` runs the same five tests against a plain XFS Samba share (no smoothfs stacking). Result: four of the five (`raw.rename`, `smb2.rw`, `smb2.getinfo`, `smb2.setinfo`) fail on plain XFS too — Samba/Linux limitations, not smoothfs bugs — and the fifth, `raw.search::ea list`, passes on plain XFS and fails through smoothfs: a confirmed smoothfs-specific bug. The `KNOWN_ISSUES` comment in `smbtorture.sh` is rewritten to match this reality, and the `raw.search::ea list` bug is scoped in a dedicated investigation note ([`smoothfs-raw-search-ea-list.md`](../completed/smoothfs-raw-search-ea-list.md)) that rules out the simple listxattr-passthrough hypothesis and names the readdir-plus-EAs fast path as the probable defect site.

- **5.6** — `raw.search` fix + promotion to `MUST_PASS`. Root cause turned out to be simpler than Phase 5.5's investigation suspected: smoothfs's `inode_operations.listxattr` was `generic_listxattr`, which only emits names from xattr handlers that declare a fixed `.name`. Our handlers are all `.prefix`-only for passthrough (`user.`, `trusted.`, `security.`), so the kernel default silently returned an empty list for every file. `getxattr`-by-name still worked (the prefix handlers' `.get` forwarded to the lower), which is why every other `smbtorture` subtest passed and this one didn't — it was the only member of the Phase 5.4 `MUST_PASS`/`KNOWN_ISSUES` set that actually enumerates a file's EAs. Fix: add `smoothfs_listxattr` that delegates to `vfs_listxattr` on the lower dentry, and wire it into all four smoothfs `inode_operations` variants. `raw.search` now passes and is promoted from `KNOWN_ISSUES` into `MUST_PASS` (16 tests). The remaining four `KNOWN_ISSUES` are Samba/Linux limitations, reconfirmed by the same XFS baseline script.

- **5.7** — Samba-upstream `knownfail` citations on the four remaining `KNOWN_ISSUES`. Paper-trail-only change to `smbtorture.sh`; each entry now points at the exact `^samba3./^samba4.` regex in Samba 4.23's `selftest/knownfail`. Confirms these are Samba-side and closes the triage loop.

- **5.8.0** — Samba source/build env. `src/smoothfs/samba-vfs/build.sh` pulls the matching `samba=2:4.22.8+dfsg-0+deb13u1` source via `apt-get source`, lays the module into `source3/modules/`, patches `source3/wscript`'s `default_shared_modules`, runs `./buildtools/bin/waf configure` with the Debian vendor suffix (`--vendor-suffix=Debian-4.22.8+dfsg-0+deb13u1`), and builds + installs `/usr/lib/x86_64-linux-gnu/samba/vfs/smoothfs.so`. The vendor-suffix detail is load-bearing — the installed `libsmbd-base-private` carries private-symbol versions keyed on the full Debian version, and a module built without the matching suffix fails to load at runtime with `version SAMBA_X.Y.Z_PRIVATE_SAMBA not found`.

- **5.8.1** — transparent-passthrough skeleton (`src/smoothfs/samba-vfs/vfs_smoothfs.c`). Overrides only `connect_fn` (to surface a `DBG_NOTICE` on share connect); every other op falls through to `SMB_VFS_NEXT_*`. Regression-safe: `smb_vfs_module.sh` runs the Phase 5.1 `smb_roundtrip.sh` assertion set with `vfs objects = smoothfs` added to `smb.conf` and all 9 assertions pass, including `trusted.smoothfs.fileid` stability across rename. Phase 5.1's and Phase 5.4's harnesses still pass unchanged (they don't load the module).

- **5.8.2** — `linux_setlease_fn` override that mirrors Samba's kernel-oplock lifecycle onto `trusted.smoothfs.lease`. When `smb.conf` has `kernel oplocks = yes` and Samba's SMB2 lease/oplock grant path calls `SMB_VFS_LINUX_SETLEASE(fsp, F_WRLCK|F_RDLCK)`, the hook calls `NEXT_LINUX_SETLEASE` first (preserving whatever the stock stack returns) and then `fsetxattr(trusted.smoothfs.lease, "\x01")` on the lower fsp — the same xattr contract Phase 5.0 defined for `SMOOTHFS_PIN_LEASE`. `F_UNLCK` triggers `fremovexattr`. Xattr errors never propagate: the setlease result returned to Samba is identical to stock behaviour, the pin is additive metadata. The module probes `trusted.smoothfs.fileid` on the share root at `connect_fn` time, caches a per-connection `is_smoothfs` flag on `handle->data`, and skips the pin toggle on non-smoothfs lowers so any Samba admin enabling `vfs objects = smoothfs` on a share whose path isn't a smoothfs mount sees pure passthrough. `smb_vfs_module.sh` is extended with a lease-lifecycle assertion (hold a CIFS fd open with `cache=strict`; poll the lease xattr through the smoothfs mount; drop the fd; poll for removal) in addition to the Phase 5.8.1 passthrough assertions. With 5.8.2 in place the *acquire* half of the Phase 5.3 contract no longer needs the reference userspace agent — Samba itself sets the pin.



- **5.8.3** — tevent-integrated fanotify watcher inside the module, landing the *break* half of the Phase 5.3 contract. At `connect_fn` time (after the smoothfs probe succeeds) the module opens a `FAN_CLASS_NOTIF | FAN_CLOEXEC` fanotify fd, marks the share mount with `FAN_MARK_MOUNT | FAN_MODIFY`, sets `O_NONBLOCK`, and registers the fd with smbd's `sconn->ev_ctx` via `tevent_add_fd`. When the kernel's `smoothfs_movement_cutover` fires `fsnotify(FS_MODIFY)` on a forced cutover — or any other non-self modify on the mount — the handler runs on the smbd event loop, drops events whose `ev->pid == getpid()` (this smbd child's own client writes would otherwise self-break every lease), `sys_fstat`s the event fd to get the smoothfs inode's `file_id`, walks `file_find_di_first/next` for this sconn, and for each fsp with `oplock_type != NO_OPLOCK` or `fsp->lease != NULL` posts `MSG_SMB_KERNEL_BREAK` via `break_kernel_oplock`. That's the same message the SIGIO path posts on a kernel-lease break, so Samba's oplock dispatcher sends the SMB-level break PDU to the client through the usual machinery. `trusted.smoothfs.lease` is then `removexattr`'d on the event fd's path as hygiene — the kernel has already cleared `pin_state` to `PIN_NONE` on cutover, this just syncs the xattr view. Setup failure (missing `CAP_SYS_ADMIN`, kernel without `FAN_MARK_MOUNT`, etc.) degrades gracefully: `fan_fd` stays at `-1`, the share still works, and deployments that need the break signal fall back to the reference `lease_break_agent`. Teardown is talloc-destructor based so the tevent fd and fanotify fd release cleanly on disconnect without an explicit `disconnect_fn`. `smb_vfs_module.sh` gets a 5.8.3 block: hold a CIFS fd while an out-of-smbd `sh -c 'echo foreign >> $path'` fires the watcher, grep the smbd log for the `fanotify event:` notice, confirm the lease pin drops to 0. With 5.8.3 landed `lease_break_agent.c` becomes a reference-only artifact for non-Samba integrations — the module supersedes it in every SMB deployment.

- **5.8.4** — `file_id_create_fn` reading `trusted.smoothfs.fileid` for a stable SMB FileId extid. The xattr is 12 bytes — u64 LE `inode_no` + u32 LE `gen` — and `file_id_create_fn` is handed only an `SMB_STRUCT_STAT`, no fd or path, so the gen has to be cached earlier. `fstat_fn` is the earliest hook with a real io fd; `openat_fn` is too early (Samba sometimes opens with `O_PATH` for dentry probes and `fgetxattr` returns `EBADF` on those). The fstat override calls `SMB_VFS_NEXT_FSTAT` then `SMB_VFS_NEXT_FGETXATTR(trusted.smoothfs.fileid)` on the fsp, parses the blob, and updates a per-connection linked list keyed on `inode_no`. `file_id_create_fn` starts from `SMB_VFS_NEXT_FILE_ID_CREATE(sbuf)` (preserves stock dev+ino), then walks the cache for a matching ino and copies `gen` into `key.extid`. Non-smoothfs shares short-circuit; cache misses keep `extid=0` (stock fallback). Memory is O(distinct inodes opened per connection) × ~40 B, lives on the connection's talloc hierarchy. On today's kernel `si->gen` is always 0 (gen bumps land with a later kernel phase that handles oid reuse) so `extid=0` and the wire-observable FileId matches stock — the module is write-now / read-later wiring, delivering 0 regression today and the full extid signal once the kernel side bumps gen. Test asserts the cache hits by greping the smbd log for `smoothfs: file_id ino=<decimal inode_no>` after an smbclient round-trip, where `<decimal inode_no>` is decoded from the first 8 bytes of the file's `trusted.smoothfs.fileid` xattr via a python helper (bash's signed 64-bit arithmetic can't represent the smoothfs inode_no — its MSB is always set).

### Phase 6 — **landed (6.0, 6.1, 6.2, 6.3, 6.5)**

iSCSI support for file-backed LUNs over smoothfs, staged the same way Phase 4 / 5 staged their protocols:

- **6.0** — O_DIRECT conformance. LIO's `fileio` backend opens its backing file with `O_DIRECT` by default, and the kernel's `do_dentry_open` O_DIRECT gate refuses the open unless `FMODE_CAN_ODIRECT` is set on the upper file. Before 6.0 smoothfs did not propagate the flag, so any `open(..., O_DIRECT)` on a smoothfs mount returned `EINVAL` and a LIO target pointed at a smoothfs-backed LUN failed to come up. `smoothfs_open` now mirrors `FMODE_CAN_ODIRECT` from the lower file (every Phase 3-validated lower — xfs, ext4, btrfs, zfs — sets it) onto the upper after `smoothfs_open_lower` completes. Reads and writes already forward `IOCB_DIRECT` through `vfs_iter_{read,write}` on the lower, which handles direct I/O natively — smoothfs itself owns no pages. `src/smoothfs/test/odirect.sh` covers the surface: aligned 1 MiB `dd oflag=direct conv=fsync` + `iflag=direct` round-trip through the smoothfs mount, byte-identical sha256 between the smoothfs view and the fast-tier native file, and a 513-byte unaligned write via a python helper confirming `EINVAL` (regression guard — a silent buffered-IO fallback would break LIO's write-through ordering contract).

- **6.1** — stock LIO file-backed LUN over smoothfs, end-to-end. Driven by `src/smoothfs/test/iscsi_roundtrip.sh`: stand up a 2-tier XFS smoothfs, `truncate` a 64 MiB backing file on the smoothfs mount, `targetcli-fb` creates a `fileio` backstore pointing at that file, publishes it as an ACL-gated target on `127.0.0.1:3260` (the default `0.0.0.0:3260` portal is swapped for loopback-only so CI and lab runs never collide with a real target on the host), and `iscsiadm` discovers + logs in using the host's default `/etc/iscsi/initiatorname.iscsi`. Eleven assertions cover: backstore creation (the Phase 6.0 O_DIRECT gate is exercised on the very first LIO open — without Phase 6.0 the backstore create would have failed), portal bind, discovery, login, `/sys/class/iscsi_session` → `/dev/sdX` device surfacing, 8 MiB block-level `dd oflag=direct conv=fsync` write, session logout+relogin, byte-identical `dd iflag=direct` readback, and sha256 equality against the fast-tier native file (confirming LIO wrote through the smoothfs mount to the real lower, not any intermediate cache). No smoothfs-specific target config required — `iscsi.go` in tierd already drives block-backed targets; file-backed LUN orchestration lives in the shell harness for now and will move into tierd as part of Phase 7's share-management wiring.

- **6.2** — LUN pin contract via `trusted.smoothfs.lun`. Phase 5.0 reserved `SMOOTHFS_PIN_LUN` (=5) in the pin_state enum but never wired a setter; 6.2 adds the xattr handler in `src/smoothfs/xattr.c` using the same shape as `trusted.smoothfs.lease`: 1-byte 0/1 value, not persisted to the lower, flips `si->pin_state` between `PIN_NONE` and `PIN_LUN`, and returns `EBUSY` if some other pin already owns the inode. The pin is *not* overridable by `force=true` on `MOVE_PLAN` — that bypass is reserved for `PIN_LEASE` per the Phase 5.3 contract; §iSCSI in Phase 0 rules that LUN movement is administrative only (operator must quiesce the target, `removexattr trusted.smoothfs.lun`, then plan). `src/smoothfs/test/iscsi_pin.sh` covers the surface with 11 assertions, including cross-pin exclusion (setting `.lease` while `.lun` is held must `EBUSY`, and vice versa) so the two pins can't silently overwrite each other. Actual auto-toggling from LIO is a userspace concern (tierd-side, part of Phase 7's share-management wiring); the kernel contract is stable now.

- **6.3** — target restart / reconnect correctness. `src/smoothfs/test/iscsi_restart.sh` reuses the 6.1 topology and exercises two target-side churn scenarios a production deployment has to survive: (A) `targetctl save → clear → restore` — the same round-trip systemd's `target.service` drives on every boot via `/etc/rtslib-fb-target/saveconfig.json`; if that path is broken, LIO doesn't come back after a reboot. The test logs in, writes 8 MiB, logs out, saves the config, clears every target, confirms the backing file on the smoothfs mount is still present and sha256-unchanged across the clear, restores, re-logs in, and reads back the same bytes. (B) `iscsid` bounce on the target host: stop the service, restart it, re-log in, re-read. Doesn't touch smoothfs but proves session restart doesn't depend on any smoothfs-side state. 12 assertions in total. With 6.3 landed, the Phase 0 §iSCSI scope (active-write durability, LUN backing-file pin policy, target restart / reconnect correctness, `O_DIRECT` conformance) is fully covered.

- **6.5** — tierd-side file-backed LUN orchestration with automatic `PIN_LUN` toggling. Phase 6.2 stood up the kernel contract for `trusted.smoothfs.lun` but nothing ever set it — `tierd/internal/iscsi/iscsi.go` only spoke to block-backed targets (ZFS zvol / LVM LV). 6.5 adds `tierd/internal/iscsi/fileio.go`: `CreateFileBackedTarget(iqn, path)`, `DestroyFileBackedTarget(iqn, path)`, `IsOnSmoothfs(path)` (statfs probe against `SMOOTHFS_MAGIC`, exported as `SmoothfsMagic = 0x534D4F46`), `PinLUN`/`UnpinLUN`, and `ValidateBackingFilePath`. Create always runs through `PinLUN` before telling LIO about the file, so there's no window where LIO has an open handle with no pin. Destroy is symmetric and best-effort: it tears down the target, then the backstore, then clears the pin (ENODATA is silently accepted), and returns the earliest error only after every step has run. On any non-smoothfs lower — operators can still run a fileio LUN on stock XFS alongside smoothfs-backed ones — the pin helpers are silent no-ops, so a single call path works for all lowers. `tierd/internal/iscsi/fileio_test.go` covers the path validator + the non-smoothfs no-op semantics + the `SmoothfsMagic` constant. `tierd/internal/iscsi/fileio_e2e_test.go` (`//go:build linux && e2e`, gated by `SMOOTHFS_KO`) stands up a 2-tier XFS smoothfs, drives `CreateFileBackedTarget` end-to-end through real targetcli, and asserts the backing file's `trusted.smoothfs.lun` goes `0 → 1` on create and `1 → 0` on destroy. **The v1 "LUN backing files are pinned by default" ruling from Phase 0 §iSCSI is now enforced on every file-backed target tierd creates.** With 6.5 the Phase 6 scope is closed; active-LUN movement (operator quiesce → unpin → move → re-pin) is lifted out of Phase 6 entirely and becomes its own **Phase 8 — Active-LUN Movement Model**. 6.5's lifecycle hooks are the only smoothfs-side prerequisite Phase 8 inherits.

### Phase 7 — **landed (7.0–7.10)**

Appliance integration and hardening, staged the same way Phase 4 / 5 / 6 were staged:

- **7.0** — `smoothfs-dkms` Debian package. Adds the missing `debian/control` / `debian/rules` / `debian/changelog` / `debian/copyright` / `debian/smoothfs-dkms.install` under `src/smoothfs/debian/` so `dpkg-buildpackage -us -uc -b` produces an installable `smoothfs-dkms_0.1.0-1_all.deb`. The package sits the source tree at `/usr/src/smoothfs-0.1.0/`, lands the Phase 1 dkms recipe alongside it as `/usr/src/smoothfs-0.1.0/dkms.conf` (via `dh_dkms`, which substitutes the `#MODULE_VERSION#` placeholder with the package version), and runs DKMS `add → build → install` against every installed kernel-headers package at install time. `BUILD_EXCLUSIVE_KERNEL` in the dkms recipe (introduced in Phase 1, unchanged here) gates the build to 6.18 LTS+ which matches the VFS API the module compiles against. The existing `postinst` / `prerm` keep their smoothfs-specific safety hooks (auto-modprobe on install; refuse to unload when a smoothfs mount is live) but now coexist with `dh_dkms`-injected DKMS calls via the `#DEBHELPER#` marker. End-to-end verified on VM 120 (Debian 13, kernel 6.18.22-smoothnas-lts): `apt install ./smoothfs-dkms_0.1.0-1_all.deb` rebuilds via DKMS, installs `/lib/modules/<kver>/updates/dkms/smoothfs.ko.xz`, auto-loads the module, mount passes reads/writes byte-correct to the lower. `apt remove` deletes the DKMS module, restores any archived `extra/smoothfs.ko` (from the pre-Phase-7 manual build path), and depmod'd the result. **This closes the first of the three Phase 0.10 operational blockers (the "packaging is a first-class concern" ruling);** signed-module and named-reviewer remain open.

- **7.1** — `smoothfs-samba-vfs` Debian package. Wraps the Phase 5.8 `vfs_smoothfs.so` build into a deb-installable form. `src/smoothfs/samba-vfs/debian/rules` invokes the existing `build.sh` with `SMOOTHFS_VFS_OUTPUT` set, so build.sh emits the built `.so` into the package staging dir instead of the system `/usr/lib` tree; `dh_auto_install` then drops it under `debian/smoothfs-samba-vfs/usr/lib/x86_64-linux-gnu/samba/vfs/smoothfs.so`. Samba's vendor-suffixed private-symbol ABI is the hard constraint: the module links against libraries whose symbol versions include the Debian build's suffix (`SAMBA_4.22.8_DEBIAN_4.22.8_DFSG_0_DEB13U1_PRIVATE_SAMBA`), so the binary package pins `Depends: samba (= ${samba:Version})` via `dh_gencontrol -- -Vsamba:Version=$(dpkg-query ...)`, which apt enforces bit-for-bit on install — a mismatched Samba version can never run the module. `dh_shlibdeps` and `dh_makeshlibs` are explicitly skipped; the `.so` is Samba-loadable, not a library other code links against, and shlibdeps would otherwise choke on the in-tree build-time paths into `/tmp/samba-<ver>/bin/shared/`. End-to-end verified on VM 120 against the installed `samba 2:4.22.8+dfsg-0+deb13u1`: `dpkg-buildpackage -us -uc -b` produces `smoothfs-samba-vfs_0.1.0-1_amd64.deb` with `Depends: samba (= 2:4.22.8+dfsg-0+deb13u1)`; `apt install` lands the `.so` at the right path, `smb_vfs_module.sh` passes (17/17 assertions); `apt remove` removes it cleanly with no residue.

- **7.2** — Module signing pipeline. Closes the third Phase 0.10 operational blocker. DKMS's built-in signing infrastructure already signs every module it builds using the `mok_signing_key` / `mok_certificate` pair at `/var/lib/dkms/mok.{key,pub}` (autogenerated on first use if absent, per the DKMS framework-conf default). What 7.2 adds is the appliance-operator glue that turns that into a real secure-boot story: `src/smoothfs/scripts/enroll-signing-cert.sh` wraps `mokutil --import /var/lib/dkms/mok.pub` with safety checks (root, mokutil present, cert readable) and a post-enroll runbook; `src/smoothfs/test/module_signing.sh` is a regression gate that verifies the installed `smoothfs.ko` carries a `PKCS#7` signature with algorithm `sha256`, signed by the DKMS key, and that the module is kernel-loaded (6 assertions). Both scripts are shipped in the `smoothfs-dkms` deb at `/usr/share/smoothfs-dkms/` so an operator can drive enrollment + verification without a source checkout. End-to-end on VM 120: `modinfo smoothfs | grep sig_id` returns `PKCS#7`, signer `DKMS module signing key`, and the in-tree harness passes 6/6. Real-secure-boot verification (shim MOK enrollment + reboot + smoothfs load under lockdown) is still a manual operator step — documented in the enroll helper's runbook, not regression-tested because the VM isn't under secure boot. Production key strategy (one offline-managed signing key rather than per-host DKMS autogen, so a single enrolled cert trusts modules built on any build host) is a Phase 7.3/CI concern.

- **7.3** — Kernel upgrade / rollback harness. The upgrade flow rests on two DKMS mechanisms we inherit: (1) autoinstall hooks fire whenever `linux-headers-<kver>` is installed, rebuilding smoothfs against the new kernel if `BUILD_EXCLUSIVE_KERNEL` in dkms.conf matches; (2) DKMS module trees are per-kernel, so a failed build on kernel B doesn't disturb kernel A's working `.ko` — GRUB can always fall back. `src/smoothfs/test/kernel_upgrade.sh` is a static-correctness check for the post-condition both mechanisms have to leave behind: for every kernel with headers installed, DKMS either reports smoothfs as installed with a signed `.ko` at `/lib/modules/<kver>/updates/dkms/smoothfs.ko.xz` (for eligible versions) or cleanly skips the kernel (for older ones the `BUILD_EXCLUSIVE_KERNEL` regex excludes), never a half-built or failed state. Shipped under `/usr/share/smoothfs-dkms/kernel_upgrade.sh` in the 7.0 deb so operators run it after every `apt upgrade`. Demonstrated end-to-end on VM 120: three kernels installed (Debian 6.12, smoothnas-lts 6.18.22, Debian bpo 6.19.10); `apt install linux-headers-6.19.10` triggered DKMS autoinstall which built + signed smoothfs for 6.19.10 in one step; the harness then confirmed both eligible kernels carry signed modules and the ineligible 6.12 was cleanly skipped. Production CI signing pipeline (single offline-managed signing key across all build hosts so one enrolled cert trusts every module) is still open.

- **7.4** — `tierd` Debian package. Ships the Go control-plane daemon and its operator CLI as a conventional Debian binary package so the appliance can `apt install tierd` and get a running service unit in one step. `tierd/debian/rules` drives `go build` via `override_dh_auto_build` against the source tree (no vendoring, the build host fetches modules at build time), producing `debian/bin/tierd` and `debian/bin/tierd-cli`; `tierd/debian/tierd.install` maps them to `/usr/sbin/tierd` (daemon) and `/usr/bin/tierd-cli` (operator). `tierd/debian/tierd.service` is installed at `/usr/lib/systemd/system/tierd.service` and `dh_installsystemd --name=tierd` wires the `multi-user.target.wants` symlink so a fresh install brings the daemon up on boot automatically (no extra operator step). `override_dh_dwz` is a no-op because Go binaries carry pre-compressed DWARF that `dwz` can't re-squeeze; `override_dh_strip --no-automatic-dbgsym` keeps the in-binary debug symbols so stack traces remain legible without a separate `-dbgsym` deb. `smoothfs-dkms` and `smoothfs-samba-vfs` are declared as `Recommends`, not `Depends` — tierd runs (degraded) on a host without smoothfs installed, which matters for day-one appliance provisioning where no pool exists yet. End-to-end verified on VM 120 (kernel 6.18.22, Go 1.24 from Debian): `dpkg-buildpackage -us -uc -b` produces `tierd_0.1.0-1_amd64.deb` (≈4.3 MB, ships a 12 MB stripped tierd + 2.3 MB tierd-cli); `apt install` brings the service up cleanly on port 8420, `/api/health` returns 200, `tierd-cli` exits with usage; `apt remove` stops the service and removes both binaries + the systemd unit.

- **7.5** — tierd share-management wiring for file-backed LUNs on smoothfs pools. Phase 6.5 landed `iscsi.CreateFileBackedTarget` / `DestroyFileBackedTarget` with auto-pin of `PIN_LUN`, but no caller inside tierd exercised it — the REST API and CLI only knew about block-backed targets. 7.5 hooks both surfaces up: `POST /api/iscsi/targets` now accepts a `backing_file` field (absolute path) as an alternative to `block_device`, the handler dispatches to the file-backed builder when that field is set, and the persisted row carries a new `backing_type` column (added by migration `00005_iscsi_backing_type.sql` with a `'block'` default so pre-7.5 rows migrate in place). `DELETE /api/iscsi/targets/<iqn>` looks up the row's backing type before tearing down, so a file-backed target's `PIN_LUN` is cleared via `DestroyFileBackedTarget` rather than leaked. `tierd-cli iscsi create-fileio` / `destroy` subcommands expose the same surface for operators. End-to-end verified on VM 120: `tierd-cli iscsi create-fileio --iqn ... --file /mnt/smoothfs/lun.img` stood up a LIO target and the backing file's `trusted.smoothfs.lun` xattr went `0 → 1`; `tierd-cli iscsi destroy` tore it down and the xattr returned to `0`. REST-side validation is covered by the package's existing unit tests (`go test ./internal/{db,api,iscsi}`); the actual REST round-trip is gated behind tierd's PAM authentication and isn't exercised here — it's covered by the existing router auth tests.

- **7.6** — tierd-ui: file-backed LUN creation surface. Extends the existing `IscsiTargets` page (`tierd-ui/src/pages/IscsiTargets/IscsiTargets.tsx`) with a `Block device` / `File-backed (LIO fileio)` radio toggle; the file-backed branch posts `backing_file` instead of `block_device` and surfaces the Phase 6.5 auto-pin contract to operators with an inline hint. The table grows a `Backing` badge column (`Block` vs `File`) so mixed deployments are identifiable at a glance. No new backend or API surface — this is the UI half of Phase 7.5. What 7.6 deliberately does **not** do is a smoothfs pool-creation flow: smoothfs pools are registered kernel-side by mount-time auto-discovery (Phase 2.5), and the "create a new pool" operation is fundamentally `mount -t smoothfs -o pool=...,uuid=...,tiers=...`, which needs a separate REST surface (systemd mount unit / fstab drop-in) before a UI can drive it. That surface is the next deferred chunk of the Phase 7 roadmap.

- **7.7** — smoothfs pool-creation REST + systemd mount-unit management. Fills the "other direction" of the Phase 2.5 auto-discovery loop: Phase 2.5 added mount-event hooks so tierd notices kernel mounts and registers them with the planner, but nothing lets an operator *ask* tierd to provision a new mount. 7.7 adds that surface. `smoothfs.CreateManagedPool` writes a systemd mount unit at `/etc/systemd/system/<escaped>.mount` with the right `Options=pool=...,uuid=...,tiers=<a>:<b>` line, enables + starts it (`systemctl daemon-reload` + `systemctl enable --now`), and lets Phase 2.5's auto-discovery do the registration — the mount unit becomes the authoritative source of truth across reboots, so tierd needs no separate mount-persistence layer. `smoothfs.DestroyManagedPool` reverses: `systemctl disable --now`, remove the unit, reload. A new `smoothfs_pools` table (migration `00006_smoothfs_pools.sql`) persists `uuid / name / tiers / mountpoint / unit_path / created_at` so `ListSmoothfsPools` / `GetSmoothfsPool` / `DeleteSmoothfsPool` survive tierd restarts. REST surface `POST|GET|DELETE /api/smoothfs/pools[/<name>]` dispatches to the store + library pair; `create` posts the systemd unit first (side-effecting) then the DB row, rolling the unit back on a DB conflict so no orphan state persists. Unit tests (`pools_test.go`) cover the name + tiers validators, the unit-filename escape, and a golden render against a fixed UUID — 14 subtests, all green. `smoothfs_pools_test.go` covers the CRUD round-trip plus the duplicate-name and not-found contracts. No test on the VM against systemd because actually mounting a smoothfs through it would leak mount units between test runs; the library's systemctl wrapper is mock-friendly for any future live harness. What's deliberately out of 7.7: CLI (`tierd-cli smoothfs create-pool`) and UI — both lift off the REST surface this commit lands and are 7.8's scope.

- **7.8** — smoothfs pool CLI + UI surfaces. `tierd-cli smoothfs create-pool --name <n> --tiers <a:b:c> [--uuid] [--mount-base]` / `destroy-pool` / `list-pools` add to the existing tierd-cli; create/destroy drive `smoothfs.CreateManagedPool` / `DestroyManagedPool` directly (same pattern as the Phase 7.5 `iscsi create-fileio` CLI — library-first, no DB writes — which keeps the CLI usable on hosts where tierd isn't yet running and sidesteps sqlite lock coordination), and `list-pools` enumerates systemd's active smoothfs mount units via `systemctl list-units --type=mount` + `systemctl cat | grep Type=smoothfs`. The tierd-ui gets a new page at `/smoothfs-pools` (`tierd-ui/src/pages/SmoothfsPools/SmoothfsPools.tsx`) with a create form (name / tier paths / optional UUID) and a list table (name / UUID / tiers / mountpoint / destroy action); the tier-paths textarea accepts either newline- or colon-separated input so operators can paste the literal `tiers=` option string unchanged. A new Storage-section nav entry `smoothfs Pools` routes there. REST goes through the Phase 7.7 handler (POST/GET/DELETE `/api/smoothfs/pools`), so the UI picks up PAM auth automatically. Bundle delta: +4 KB minified, well under the existing chunk warning. No new backend or library code in 7.8; it's purely the operator-surface layer that lifts off 7.7.

- **7.9** — observability + repair surface: Quiesce / Reconcile buttons + movement-log view. Three new REST routes hang off the Phase 7.7 handler: `GET /api/smoothfs/movement-log?limit=N&offset=M` (reads `smoothfs_movement_log`, newest-first, capped at 500 per page to keep a runaway UI from pinning tierd), `POST /api/smoothfs/pools/<name>/quiesce` (resolves pool name → UUID → opens a `smoothfs.Client` → sends `SMOOTHFS_CMD_QUIESCE`), and `POST /api/smoothfs/pools/<name>/reconcile` (same flow with an optional `{"reason": "..."}` body that flows into the kernel's netlink attr for audit). The SmoothfsPools page grows matching surface: every pool row gains Quiesce / Reconcile action buttons alongside Destroy, and a "Movement log" section below the pool list renders the newest 100 transitions as a table (timestamp / seq / truncated object_id / from→to state / source→dest tier). Reconcile prompts the operator for a reason string via `window.prompt` so the audit row is useful after the fact. No new backend library code — everything lifts off existing `smoothfs.Client` methods (Phase 2) and the movement-log table (Phase 2's `00003_smoothfs_movement.sql`); the only new Go code is the REST dispatch + the one new store helper (`ListSmoothfsMovementLog`). Bundle delta: +2 KB minified.

- **7.10** — support matrix + operator runbook. Closes Phase 7. Two new top-level docs: `docs/smoothfs-support-matrix.md` pins the three independently-versioned debs (`smoothfs-dkms`, `smoothfs-samba-vfs`, `tierd`) against Debian 13 / kernel 6.18+ / OpenZFS 2.4.1 / Samba 4.22.8+dfsg-0+deb13u1 / the four tested lower filesystems (xfs / ext4 / btrfs / zfs) and spells out which protocols (NFS / SMB / iSCSI file-backed) are supported vs. deferred-to-Phase-8 (active-LUN movement). `docs/smoothfs-operator-runbook.md` covers the day-0 install flow (three apt-installs + MOK enrollment + tierd health check), pool creation via UI and CLI, share creation (NFS / SMB / iSCSI), the quiesce/reconcile maintenance cycle, kernel-upgrade + rollback via DKMS's per-kernel trees, Samba-upgrade rebuild, eight named troubleshooting paths (EOPNOTSUPP on mount, VFS version mismatch, wedged movement, pool-not-found, missing MOK keys, tierd.db corruption, "active mounts" prerm message, module-signing gaps), and an escalation checklist with a ready-made `tar czf` command for diagnostic bundles. Both files reference the Phase 7.0–7.9 commits + scripts by name so operators can always get back to the code path that created whatever they're looking at.

### Phase 8 — **deferred** (split out of Phase 6)

Phase 8 (active-LUN movement model — operator quiesce → clear `PIN_LUN` → journaled move → re-pin) was originally the open "6.4 — active-LUN movement model not relaxed until proven in test" carry-over inside Phase 6. With 6.5 landing the tierd-side pin lifecycle, Phase 6 closes cleanly; the remaining work is big enough (operator quiesce protocol, journaled move with a new pre-cutover gate, crash recovery, correctness + fault-injection tests) to stand on its own. It is now tracked by [`smoothfs-active-lun-movement.md`](../completed/smoothfs-active-lun-movement.md) and remains gated by Phase 6 soaking in production.

---

## Context

The existing FUSE-based tiering stack (`src/fuse-ns/` + `tierd`) works, but every throughput improvement we make still runs into the same structural floor: FUSE kernel↔daemon round-trips on metadata-heavy workloads. The CREATE fast-path proposal (`docs/proposals/pending/fuse-ns-create-fast-path.md`) improves that floor, but it does not remove it.

This proposal takes the other path: build a native Linux stacked filesystem, `smoothfs`, that sits between the VFS and one or more lower filesystems and performs tier-aware placement in kernel context.

This is a **greenfield implementation proposal**, not a migration plan from existing managed namespaces. But it is **not** a no-movement design. The intended end state is a live tiering filesystem where files move up and down a pool based on heat, while remaining usable through standard NAS protocols.

Several invariants in this proposal (persistent `object_id`, single authoritative placement record, kernel-exposed movement transactions) are **departures from today's on-disk model**, not descriptions of existing behaviour. Phase 0 is where those departures get pinned down before any kernel code is written.

---

## Problem

The current FUSE stack has three costs that compound:

1. **Per-op RTT floor.** `LOOKUP`, `CREATE`, `SETATTR`, `FLUSH`, and `RELEASE` all cross `/dev/fuse` and synchronously wait for the daemon. `CREATE` additionally crosses a second RTT from the C daemon to the Go `tierd` process over a Unix socket (measured ~150–300 µs/file, per `fuse-ns-create-fast-path.md`).
2. **Userspace daemon complexity.** `fuse_ns.c` (~2.9 kloc) carries its own inode cache, directory snapshot protocol, fd tracking, and fallback machinery. `tierd/internal/tiering/fuse/` adds another ~1.1 kloc of Go IPC plumbing.
3. **Patch-carrying roadmap.** The remaining performance path depends on kernel/libfuse behavior outside our direct control.

Those costs are consequences of the architecture, not isolated bugs. A native stacked filesystem removes the FUSE daemon from the hot path entirely.

Note: heat collection is **currently disabled** in the tierd tree — `meta.Record.HeatCounter` and `LastAccessNS` are explicit placeholders. The dominant per-op cost today is tierd CREATE RTT plus intrinsic FUSE RTT, not heat sampling. smoothfs removes the former and re-homes the latter into the kernel.

---

## Current Implementation Baseline

This is what exists in the tree today. Phase 0 and later phases need to state, per area, whether smoothfs preserves, replaces, or re-homes each piece.

**FUSE data/metadata path** (`src/fuse-ns/`):

- `fuse_ns.c` — FUSE low-level daemon with kernel passthrough (6.9+) and non-passthrough fallback.
- `fuse_passthrough_fixup.c` — passthrough helper.
- Op table (`fuse_ns.c:2707–2726`): `lookup`, `getattr`, `setattr`, `open`, `create`, `read`, `write`, `release`, `unlink`, `mkdir`, `rmdir`, `rename`, `fsync`, `opendir`, `readdir`, `releasedir`. Notably absent today: `symlink`, `link`, `readlink`, `mknod`, `statfs`, `access`, xattr ops, `fallocate`, lock ops, `copy_file_range`. smoothfs will need these on day one.

**tierd (Go control plane)** (`tierd/`):

- `internal/tiering/fuse/` — Unix socket IPC with the C daemon.
- `internal/tiering/mdadm/` — file-level adapter that routes opens across per-tier XFS-on-LV mounts (`adapter.go`, `placement.go`, `migrate.go`, `target_cache.go`). Package doc: "each tier gets its own VG/LV/mount, and a FUSE daemon routes file opens to the correct backing tier via HandleOpen."
- `internal/tiering/zfsmgd/` — file-level adapter over ZFS datasets. Already implements plan/reserve/copy/verify/cutover/cleanup with crash recovery (`adapter.go:1211+`, `recovery_test.go`, `quiesce.go`, `snapshot.go`). smoothfs Phase 2 inherits this design, not invents it.
- Terminology split: the **pool** is the combined-tier namespace that smoothfs exposes, while a **tier** is one backing storage member inside that pool. In the current support story, pool filesystems are `xfs`, `ext4`, or `btrfs`, while individual tier backends are `zfs`, `mdadm`, or `btrfs`.
- `internal/tiering/meta/` — per-pool metadata store. `record.go` defines a 32-byte record keyed by inode with fields `version`, `pin_state`, `tier_idx`, `namespace_id`, `heat_counter` (**placeholder; currently zero — heat collection is disabled**), `last_access_ns` (placeholder). Records are **per-tier-local**, not globally authoritative; `store.go:30–65` calls this out explicitly.
- `internal/tiermeta/` — separate tier-metadata store.
- `internal/db/` — SQLite control-plane tables (`tier_pools`, `tiers`, `managed_volumes`, `movement_jobs`, `placement_intents`, `managed_objects`). This remains the authoritative durable store for control-plane state.

**NAS protocols (already present)** (`tierd/internal/{nfs,smb,iscsi}/`): thin orchestrators over kernel `nfsd`, Samba, and `targetcli`. They produce shares/targets over the existing FUSE namespaces. smoothfs's Phase 4/5/6 work is *not* bringing up protocols for the first time — it is proving the filesystem invariants those protocols already rely on, and then pointing the existing export flows at `smoothfs` mounts instead of FUSE mounts.

**What does not exist today and this proposal creates:**

- Persistent, movement-stable `object_id`. Today's records are inode-keyed and per-tier.
- A single authoritative placement record per file. Today placement lives on whichever tier holds the file plus SQLite state in `tierd`.
- A kernel-side VFS stacking layer. Today all VFS work is in the FUSE daemon.
- A netlink control channel. Today control is a Unix socket.

---

## Relationship to In-Flight Work

smoothfs is a long arc. It must not stall or duplicate work already in flight.

| Proposal / Feature | Status today | Relationship to smoothfs |
|---|---|---|
| `fuse-ns-create-fast-path.md` | Pending | **Retained as the near-term performance path.** Delivers the CREATE-RTT improvement without waiting for smoothfs. Decommissioned only after smoothfs Phase 3 ships and the affected adapters cut over. |
| `unified-tiering-04b-zfs-managed-adapter.md` | Pending | **Delivered on the current FUSE daemon.** Remains the authoritative managed-ZFS adapter until smoothfs reaches parity in Phase 3. No divergence of placement semantics between the two. |
| `unified-tiering-06-coordinated-snapshots.md` | Pending | **Not superseded.** Its quiesce/atomicity constraints feed the smoothfs snapshot model (see §Snapshot Model). Single-pool ZFS namespaces remain the only supported atomic snapshot path until smoothfs defines its own. |
| `mdadm-heat-engine-*` (done + pending UI) | Active | **Orthogonal.** mdadm heat engine is block-level (LVM PE / region) tiering *below* the filesystem. smoothfs is file-level *above* the filesystem. A SmoothNAS pool can use either, but not both on the same volume. |
| Existing mdadm adapter (`internal/tiering/mdadm/`) | Shipped | File-level adapter that already routes opens across per-tier XFS-on-LV mounts via the FUSE daemon. smoothfs replaces the FUSE layer here in Phase 3 by addressing a smoothfs mount whose lowers are the same per-tier XFS-on-LV mounts. |
| Existing zfs-managed adapter (`internal/tiering/zfsmgd/`) | Shipped | Same pattern: smoothfs replaces the FUSE layer by stacking over the same per-tier ZFS datasets. The movement workers stay. |
| `disk-spindown.md` | Pending | **Consumes smoothfs's metadata-on-fastest-tier guarantee.** smoothfs already pins placement records to the fastest lower; spindown's Metadata-on-SSD Invariant relies on that. Spindown's active-window scheduler reuses `SMOOTHFS_CMD_QUIESCE` so snapshots and reconciles do not wake spundown disks. |

---

## What smoothfs is

A Linux kernel module registered via `register_filesystem()`. It mounts over a namespace and forwards real IO to lower filesystems selected by policy:

```text
mount -t smoothfs -o pool=backups,tiers=/mnt/nvme-backups:/mnt/hdd-backups none /mnt/backups
```

The mounted filesystem owns no disk. Its lowers are pre-mounted filesystems or datasets managed by `tierd`. `smoothfs` provides one virtual namespace; the lower tiers hold the actual file data.

`tierd` remains the control plane. It continues to own:

- pool lifecycle
- tier registration
- policy computation
- movement planning
- reconciliation and repair
- REST / CLI / UI integration
- share management for `NFS`, `SMB`, and `iSCSI`

Control-plane communication with `smoothfs` is over netlink. SQLite in `tierd` remains the authoritative durable store for control-plane state; the kernel's on-disk placement record is a hot-path cache that Phase 0.2 formalises.

---

## Glossary

The existing codebase uses "pool", "tier", "tier target", and "namespace" in specific ways. This proposal keeps the existing meanings.

- **Pool** — a tiering unit with a single exposed namespace, a policy domain, and an ordered set of tier targets. Matches `tier_pools` in SQLite.
- **Tier target** — one storage destination within a pool, described by `tierd/internal/tiering/model.go:TierTarget`. Has a rank, capacity, fill policy, capabilities.
- **Pool filesystem** — the filesystem shape used for the combined-tier namespace that smoothfs exposes. This is the compatibility matrix for smoothfs itself: `xfs`, `ext4`, and `btrfs` in the current Phase 3 path.
- **Tier backend** — the storage/backend type of an individual tier inside the pool. This is a separate matrix from the pool-filesystem set. The intended tier-backend set is `zfs`, `mdadm`, and `btrfs`.
- **Namespace** — one exposed path on top of a pool. smoothfs mounts one namespace per pool in v1.
- **Object** — one tiered file. Identified by persistent `object_id`.

"smoothfs pool" is the same pool concept; "smoothfs tier" means one tier target plus its lower filesystem.

---

## Goals

1. Eliminate FUSE round-trips from the steady-state data and metadata path.
2. Support real heat-based tiering: files move up and down tiers over time.
3. Preserve stable object identity across rename and movement.
4. Ship Phase 1 support for the two lower-filesystem classes SmoothNAS already deploys in production: **ZFS datasets** (used by `zfs-managed`) and **XFS-on-LV** (used by `mdadm`). These are the bare-minimum lowers — smoothfs is not useful until both work.
5. Extend the smoothfs **pool-filesystem** compatibility matrix in Phase 3 to **`ext4`** and **`btrfs`**, chosen by testable capability contract rather than blanket VFS compliance.
6. Keep the SmoothNAS **tier-backend** matrix explicit and separate: **`zfs`**, **`mdadm`**, and **`btrfs`** describe individual tiers inside a pool, not the pool-filesystem compatibility set.
6. Support `NFS`, `SMB`, and `iSCSI` on top of `smoothfs`, but only after the filesystem proves the invariants those protocols require.
7. Keep rollout opt-in and pool-scoped. Existing FUSE-based pools continue to work until explicitly converted.

---

## Non-goals

- Replacing lower filesystems or their native storage features.
- Per-block or per-extent tiering in v1. `smoothfs` tiers at file granularity.
- Replacing mdadm block-level (region/PE) tiering. That path stays in LVM and remains supported.
- Mirrored or multi-tier replicated placement in v1. Each object lives on exactly one tier at a time.
- In-place conversion of existing non-smoothfs namespaces in v1. Conversion requires creating a new smoothfs pool and migrating data.
- Mainline upstreaming in v1.
- Multi-host / clustered semantics in v1.
- Assuming every VFS-compliant filesystem is safe without testing.

---

## Core Invariants

1. **Stable object identity.** A file keeps the same logical identity across rename and movement. This is a **new field**; today's meta records are keyed by inode.
2. **Single authoritative placement record.** At any point, one durable record says where the file lives now and where it intends to live next. This is a **departure from today's per-tier-local records in `tierd/internal/tiering/meta/`**; Phase 0.2 defines which side (kernel or tierd SQLite) is authoritative.
3. **Journaled movement.** Promotion and demotion are replayable state transitions, not best-effort copies. The zfsmgd adapter's existing copy/verify/cutover/cleanup logic is the reference implementation; smoothfs lifts the cutover into kernel coordination without discarding the userspace machinery.
4. **Crash-safe cutover.** Reboot or daemon restart during movement must never leave the file in an ambiguous authoritative state.
5. **Protocol-safe semantics.** `NFS`, `SMB`, and `iSCSI` are only enabled once their required invariants are proven for the current phase.
6. **Compatibility is explicit.** Lower filesystems are supported by tested capability class, not by marketing claim.

---

## Architecture Overview

### Namespace model

Each `smoothfs` mount represents one managed pool namespace. The pool has:

- one exposed namespace path
- an ordered set of lower tiers
- a persistent metadata area
- a policy domain
- a movement queue

### Persistent identity

Each file has a persistent `object_id`. That identity is distinct from both:

- the current lower inode number
- the current path

The filesystem may expose a stable inode number derived from persistent metadata, but the durable source of truth is the `object_id`.

### Placement metadata

Per-file placement metadata must persist at least:

- `object_id`
- current tier
- intended tier
- placement state
- generation / sequence number
- last completed movement transaction
- protocol/export identity fields when enabled

### Control-plane split

Kernel responsibilities:

- VFS operations
- lower-file dispatch
- object identity exposure
- local movement coordination
- netlink event emission
- heat sample aggregation and export

Userspace responsibilities:

- policy
- heat analysis (consumer of kernel samples)
- movement planning
- repair and reconciliation
- share/export orchestration
- durable control-plane state in SQLite

---

## Heat-Based Tiering Model

Heat collection is **currently disabled** in the tierd tree: `meta.Record.HeatCounter` and `LastAccessNS` are placeholders. smoothfs re-introduces heat sampling as a kernel responsibility rather than re-activating the legacy FUSE collection.

Files are expected to move both upward and downward in the tier set based on observed heat.

### Heat inputs

The design should support weighted heat from:

- open frequency
- read bytes
- write bytes
- recency
- optional policy boosts such as pin-hot or pin-cold

### Anti-thrash rules

The policy engine must include:

- minimum residency time on a tier
- promotion threshold
- demotion threshold
- cooldown after movement
- hysteresis between adjacent tiers
- upper-tier fullness rules

### Placement states

The movement engine should model explicit states:

- `placed`
- `promote_pending`
- `promoting`
- `demote_pending`
- `demoting`
- `cutover_pending`
- `cleanup_pending`
- `stale`
- `error`

This is the minimum level of explicitness needed to support bidirectional tiering safely.

---

## Movement Semantics

Bidirectional tiering is the hard part. The proposal must treat movement as a journaled state machine, not as a hidden implementation detail.

The zfsmgd adapter (`tierd/internal/tiering/zfsmgd/adapter.go:1211+`) already implements plan/reserve/copy/verify/cutover/cleanup with crash recovery (`recovery_test.go`). smoothfs Phase 2 carries that design forward: the kernel owns the live cutover and I/O redirection; the copy/verify/cleanup workers and the durable journal remain in `tierd`.

### Required movement pipeline

Each promotion or demotion should follow a durable sequence:

1. plan
2. reserve destination
3. copy
4. verify
5. cut over authoritative placement
6. clean up source
7. finalize transaction

### Open file semantics

The design must explicitly define what happens if a file is:

- open for read during movement
- open for write during movement
- mmapped during movement
- renamed during movement
- unlinked during movement
- hard-linked during movement
- locked during movement

Default Phase 1 stance for any case not explicitly supported: **forbidden with a defined errno**, not silently wrong. See §POSIX Semantics for per-case rulings.

### Crash recovery

On restart, `smoothfs` plus `tierd` must be able to distinguish at least:

- source authoritative, destination partial
- source authoritative, destination verified but not cut over
- destination authoritative, source cleanup incomplete
- stale or conflicting placement record

Repair tooling must be able to reconcile these states deterministically.

---

## POSIX Semantics

A stacked file-level tiering filesystem exposes POSIX semantics that the current FUSE stack does not fully implement. Each item below must get a Phase 0 answer of "supported", "forbidden with explicit errno", or "deferred to phase X with this interim behaviour". Any case not explicitly supported is forbidden, not silently wrong.

### Hardlinks

Hardlinks are the worst case for file-granularity tiering: all links to one inode must physically live on one lower filesystem, because POSIX `link(2)` cannot cross filesystems.

- v1 default: a hardlinked object is **pinned to its current tier**. The scheduler skips promotion/demotion for any object with `nlink > 1`. A future policy override (`allow_hardlink_migration`) moves the whole link-set atomically as one transaction.
- `link(2)` across directories within the same smoothfs namespace is allowed and creates a link on the same lower as the existing inode.
- `link(2)` across pools is `EXDEV`.

### mmap

- `MAP_PRIVATE`: safe during movement. The mapping is backed by the source lower's pages until the process drops it.
- `MAP_SHARED` for read: safe. The mapping is torn down and re-established at cutover.
- `MAP_SHARED` for write: **forbidden during active movement in v1.** The scheduler refuses to promote or demote any object with a writable shared mapping open; the override path is an admin quiesce (fsync + mapping revocation) before starting the transaction.
- Phase 0 specifies the exact revocation mechanism (unmap + fault re-resolution vs. explicit copy-up).

### xattrs and ACLs

- `user.*` xattrs: passed through to the lower.
- `trusted.smoothfs.*`: reserved for smoothfs placement metadata; writable only by root.
- `security.*` (SELinux labels, `security.capability`): passed through; labels are preserved across movement as part of the copy/verify contract.
- `system.posix_acl_access` / `system.posix_acl_default`: passed through; the lower filesystem must support POSIX ACLs (capability-probed).
- Movement copies xattrs in the same transaction as data; verification includes a set comparison.

### POSIX / BSD / OFD locks

- `flock(2)` and OFD locks (`fcntl(F_OFD_*)`): held in the kernel smoothfs inode, not the lower. Survive movement.
- Advisory `fcntl(F_SETLK)` locks: same.
- Mandatory locks: not supported (matches Linux direction).

### Quotas

- Tier-local quotas (XFS project quotas, ZFS dataset quotas) are enforced by the lower and reflected at write time.
- **Pool-wide per-user/group quotas are out of scope for v1.** Operators see aggregate pool usage via `statfs` and per-tier reporting; cross-tier per-user quotas require a dedicated proposal.

### Sparse files and reflinks

- Sparse files: preserved across movement using `SEEK_DATA`/`SEEK_HOLE`; lowers must advertise sparse support (capability-probed).
- Reflinks: **not preserved across movement in v1.** If source and destination lowers both support reflinks and the operator opts in via `preserve_reflinks`, the copy path uses `copy_file_range` with the reflink flag where available.

### Directories

Directory trees exist per-lower. smoothfs presents a unioned namespace, but every lower holds its own directory skeleton so an object can be created on any tier without cross-tier coordination on every `mkdir`.

- `mkdir(2)` creates the directory on the **canonical tier for that namespace**, default fastest. Child objects are created on whichever tier the policy selects at create time; the child's parent directory is lazily materialised on that lower via a directory-shadow record.
- `rmdir(2)` verifies no objects remain on any lower. It is a cross-tier consistency point.
- Rename within one pool is a pure metadata op unless the destination parent does not yet exist on the source tier.

### Timestamps

- `atime`, `mtime`, `ctime` preserved across movement. The cutover commits new-lower timestamps to match the source.
- `relatime` / `noatime` honour the smoothfs pool mount option regardless of individual lower mount options.

### statfs

`statfs(2)` reports aggregate free/used across all tier targets in the pool. Per-tier breakdown is surfaced via procfs (see §Observability).

### Case sensitivity

smoothfs is case-sensitive. Case-insensitive matching for `SMB` clients is the responsibility of the Samba VFS module (§Phase 5), not smoothfs.

### fscrypt

- If a lower uses fscrypt, smoothfs passes through. Movement between fscrypt-enabled lowers sharing a policy is allowed.
- Cross-lower movement where source and destination use different encryption policies is **forbidden** in v1.

---

## Snapshot Model

NAS operators expect snapshots. Leaving this unaddressed is not an option.

### v1 baseline (available in Phase 3)

- smoothfs itself does not own snapshots.
- For zfs-managed pools where all tier targets live in one zpool, coordinated snapshots are taken via a single `zfs snapshot -r` as defined in `unified-tiering-06-coordinated-snapshots.md`. smoothfs's contribution is a quiesce point: freezing new movement transactions and flushing in-flight cutovers before the `zfs snapshot` call.
- For multi-pool ZFS backing and for XFS-on-LV backing, **no atomic coordinated snapshot is possible in v1.** smoothfs exposes per-tier snapshot orchestration only; operators get a set of non-atomic snapshots with an explicit warning surfaced in the UI.

### Phase 5+ option

A smoothfs-native snapshot (per-pool journal checkpoint + lower snapshots) is a candidate follow-on. This proposal does not commit to it; Phase 0.2 records that the placement record schema leaves room to support one later.

### Quiesce semantics

The snapshot quiesce must:

- drain all in-flight movement cutovers to a stable state
- block new cutovers until snapshot completes
- allow in-flight `read`/`write` to complete normally
- be driven by the new `SMOOTHFS_CMD_QUIESCE` netlink command

---

## Lower Filesystem Support

The project goal is broad support across major Linux filesystems, but that must be delivered as a compatibility matrix.

### Initial support principle

Do not claim "any VFS-compliant filesystem is valid." Instead:

- define a required lower-fs capability contract
- probe those capabilities at mount time
- reject unsupported combinations

### Required lower-fs capability areas

Each lower filesystem needs validation for:

- xattr behavior (`user.*`, `trusted.*`, `security.*`, `system.posix_acl_*`)
- rename semantics (atomicity, crossing directories within the lower)
- hard-link behavior (same-lower only)
- mmap coherence under concurrent writers
- direct IO behavior
- fsync / durability expectations
- ACL behavior (POSIX ACL preservation)
- sparse-file semantics (`SEEK_DATA`/`SEEK_HOLE`)
- quota interactions
- inode reuse edge cases and generation counter availability for stable NFS handles
- reflink / `copy_file_range` capability (optional, tracked per-lower)
- fscrypt support and policy preservation (optional)

### Phase 1 lower-fs set — the bare minimum

smoothfs is not useful until it ships the two lower classes SmoothNAS already uses in production:

- **`xfs` on LV** — the lower used by the `mdadm` tiering adapter.
- **`zfs` datasets** — the lower used by the `zfs-managed` tiering adapter.

Phase 1 and Phase 2 land with these two classes only. Any additional lower is rejected at mount time with a descriptive error.

### Phase 3 compatibility expansion — stated expansion targets

After Phase 1/2 invariants are proven, Phase 3 broadens the **smoothfs pool-filesystem compatibility matrix** to:

- **`xfs`** — retained as a first-class validated lower.
- **`ext4`** — target lower for compatibility with legacy deployments.
- **`btrfs`** — target lower, with reflink and subvolume interactions explicitly tested.

Each Phase 3 lower must pass the full capability contract before claiming support. If a lower's upstream state changes a contract (for example, an xattr or ACL behaviour change), it is dropped from the supported set until the mismatch is resolved.

This is separate from the **SmoothNAS tier-backend matrix**, which remains:

- **`zfs`**
- **`mdadm`**
- **`btrfs`**

### Lowers explicitly out of scope

- Network filesystems as lowers (`NFS`-on-`NFS`, `SMB`-on-smoothfs-over-`NFS`). Too many conflicting invariants.
- FUSE filesystems as lowers. smoothfs exists to replace FUSE, not to recurse on it.
- Read-only filesystems as writable lowers.

---

## NAS Protocol Support

`NFS`, `SMB`, and `iSCSI` are in scope for the end state, but they are not automatic consequences of "a filesystem that mounts." The orchestrators in `tierd/internal/{nfs,smb,iscsi}/` already exist — what Phase 4/5/6 prove is that the filesystem underneath is safe for them.

### NFS

`NFS` requires stable export semantics:

- file handles must remain resolvable across reconnect and restart
- the handle's `fs_id` must be stable across tier movement and module reload
- the handle's per-object generation counter must increment only when identity is unlinked and reused, never on mere movement
- rename and movement must not break handle stability
- exported identity must survive cutover between tiers

`NFS` support therefore depends on a durable export identity model, not just path lookup. The `object_id` is the natural basis for the NFS file-handle body; the generation counter must be separately persisted (Phase 0.1).

`statfs` reported via `NFS` shows pool aggregate, matching local `statfs(2)`.

### SMB

`SMB` depends on coherent file identity and mutation behavior:

- rename semantics
- change notification behavior (fanotify from smoothfs to Samba's VFS)
- share mode / lease behavior
- stable identity during movement
- case-insensitive matching handled by the Samba VFS module — smoothfs itself remains case-sensitive

Background tier movement must not violate those assumptions. In particular, lease break on movement must be driven by smoothfs emitting events a Samba VFS module can subscribe to.

### iSCSI

For `iSCSI`, the main issue is durability and active-write semantics for backing files used as LUN storage:

- write ordering
- fsync / barrier behavior
- sparse allocation behavior
- `O_DIRECT` support for LIO's `fileio` backend
- movement while the backing file is serving block IO

LUN backing files are a bad fit for background movement. The v1 position:

- LUN backing files are **pinned** by default. Phase 6.5 enforces this in tierd: every file-backed LUN created by `CreateFileBackedTarget` installs `PIN_LUN` via `trusted.smoothfs.lun=1` before LIO opens the file, and `DestroyFileBackedTarget` clears it after teardown.
- Pin can be lifted only by quiescing the LIO target (administrative operation, not automatic).

Phase 6 does not relax that. An active-LUN movement model — operator quiesces the target, clears `PIN_LUN`, runs a journaled move, re-pins — is scoped out of v1 and lives under **Phase 8 — Active-LUN Movement Model** (§Phased Delivery), which will only start once the Phase 6 pin contract has seen production time.

---

## Non-Functional Targets

A proposal whose entire premise is a latency floor must name targets. These are directional and will be re-baselined after Phase 0.

- **CREATE p99** within 2× native XFS on the fastest tier, measured by the same 10×1000-file rsync harness used in `fuse-ns-create-fast-path.md`.
- **Steady-state sequential read throughput** within 95% of native lower throughput for files >1 MiB.
- **Steady-state sequential write throughput** within 90% of native lower throughput for files >1 MiB.
- **Metadata-op latency (LOOKUP / GETATTR cache hit)** within 1.5× native.
- **Memory budget** under 4 KiB per in-core object at steady state.
- **CPU overhead** under 10% over native for bulk copy workloads.

These are gating for Phase 3 completion, not Phase 1. Phase 1 must establish the measurement harness against both Phase 1 lowers.

---

## Control Plane

One generic netlink family, `"smoothfs"`, multiplexed across pools.

Illustrative commands:

| Command | Direction | Purpose |
|---|---|---|
| `SMOOTHFS_CMD_REGISTER_POOL` | tierd → kernel | Register a pool and ordered lower tiers. |
| `SMOOTHFS_CMD_POLICY_PUSH` | tierd → kernel | Push tiering policy inputs and limits. |
| `SMOOTHFS_CMD_MOVE_PLAN` | tierd → kernel | Submit a planned promotion/demotion transaction. |
| `SMOOTHFS_CMD_TIER_DOWN` | tierd → kernel | Quarantine a failed or draining tier. |
| `SMOOTHFS_CMD_RECONCILE` | tierd → kernel | Trigger repair / replay reconciliation. |
| `SMOOTHFS_CMD_QUIESCE` | tierd → kernel | Drain in-flight cutovers for snapshot. |
| `SMOOTHFS_EVENT_HEAT_SAMPLE` | kernel → tierd | Export activity observations. |
| `SMOOTHFS_EVENT_MOVE_STATE` | kernel → tierd | Report movement state transitions. |
| `SMOOTHFS_EVENT_TIER_FAULT` | kernel → tierd | Report lower-tier failures. |

The exact message schema can evolve, but the architecture needs this level of lifecycle coverage.

### Durable placement authority

SQLite in `tierd/internal/db/` remains the authoritative durable store for control-plane state. The kernel's on-disk placement record is a cache for hot-path lookups; on crash, `tierd` replays authoritative placement from SQLite into the kernel via `SMOOTHFS_CMD_RECONCILE`. Phase 0.2 either confirms this or changes it.

---

## Observability

Without inspection tooling, kernel-side placement becomes a black box for support.

- `/proc/smoothfs/pools` — one line per registered pool with aggregate stats.
- `/proc/smoothfs/<pool>/tiers` — per-tier fill, hit rate, move counters.
- `/sys/kernel/debug/smoothfs/<pool>/objects/<object_id>` — per-object placement, heat, open count (debug builds).
- Metrics exported to `tierd` via `SMOOTHFS_EVENT_*` and surfaced through the existing `tierd/internal/monitor` path.
- Kernel trace points (`trace_event`) for movement transitions.
- CLI: `tierd-cli smoothfs inspect <path>` returns the full placement record.

---

## Coexistence and Migration

- smoothfs is **opt-in per pool**. Existing FUSE-based pools continue to work unchanged.
- **Dual use on one volume is forbidden**: a pool is either smoothfs or legacy-FUSE. No gradual conversion inside one pool.
- **New pool = new data.** In-place conversion of an existing FUSE pool to smoothfs is out of scope for v1. Migration procedure: create a new smoothfs pool, `rsync` or `zfs send` data across, cut over shares.
- **Downgrade** from a smoothfs pool back to a flat lower mount is supported: on decommission, smoothfs relinquishes the namespace and the operator remounts the fastest lower directly. Any data on slower tiers must be pulled up first.
- **mdadm block-level tiering and smoothfs must not be enabled on the same volume.** The control plane rejects this combination; the UI warns.

---

## Phase 0 Contract

Phase 0 exists to lock the semantics before kernel implementation starts. The
output of this phase is not code; it is a correctness contract and a matching
conformance plan.

### 0.1 Object Identity

Phase 0 must define all three identity layers explicitly:

- `object_id`: the durable identity of the file across rename and movement
- exported inode identity: the stable identity visible through the mounted namespace
- protocol export identity: the handle identity used by `NFS`, `SMB`, and later `iSCSI`-relevant backing-file policy

The document produced in Phase 0 must answer:

- how `object_id` is created
- where it is persisted
- how it is recovered after crash
- how inode identity is derived from it
- what generation / version field prevents stale-handle reuse
- how the NFSv3 filehandle generation counter is persisted and updated only on identity reuse (not on movement)

### 0.2 Placement Authority

Phase 0 must define one authoritative placement record per file. At minimum it must contain:

- `object_id`
- current tier
- intended tier
- movement state
- transaction or sequence number
- last committed cutover generation

The Phase 0 contract must state whether the authoritative record lives:

- in kernel-managed persistent metadata
- in `tierd`-owned durable metadata
- or in a hybrid design with one side authoritative and the other cached

This must be unambiguous. The current proposal's working assumption is the **hybrid model**: `tierd`'s SQLite is the durable truth, the kernel's on-disk record is a hot-path cache. Phase 0 either confirms or changes this.

### 0.3 Movement Transaction Model

Phase 0 must define the exact transaction boundaries for:

1. plan accepted
2. destination reserved
3. copy started
4. copy verified
5. cutover committed
6. source cleanup committed
7. transaction finalized

For each transition, the contract must say:

- what durable record is written
- whether the old or new copy is authoritative
- what replay does after crash
- what user-visible state is allowed at that point

### 0.4 Concurrency Semantics

Phase 0 must define exact behavior for these cases:

- file open for read during promotion
- file open for write during promotion
- file open for write during demotion
- file mmapped (`MAP_PRIVATE`, `MAP_SHARED` read, `MAP_SHARED` write) during movement
- file renamed during movement
- file unlinked during movement
- hard link created or removed during movement
- advisory or mandatory lock held during movement
- lease / oplock held during movement (`SMB`)
- `O_DIRECT` open during movement

If early phases forbid any case, the Phase 0 contract must say so explicitly and define the syscall result or deferral behavior.

### 0.5 Heat and Policy Contract

Phase 0 must define:

- heat inputs collected by the kernel
- aggregation done by `tierd`
- decay model
- promotion threshold
- demotion threshold
- minimum residency
- cooldown after movement
- fullness override behavior
- tie-breaking rules

Without this, "moves up and down based on heat" is too vague to implement or test.

### 0.6 Lower-FS Capability Contract

Phase 0 must publish the required capability matrix fields for a lower filesystem (see §Lower Filesystem Support). The contract must state, for Phase 1's two lowers (**XFS-on-LV** and **ZFS**), the specific values expected for each capability bit. This is the contract used by mount-time compatibility probing in later phases.

### 0.7 Protocol Invariants

Phase 0 must define the filesystem invariants required before enabling each protocol:

For `NFS`:

- stable export handle identity (`object_id`-backed)
- reconnect-safe handle resolution
- rename-safe handle behavior
- movement-safe handle behavior
- deterministic generation counter that does not increment on movement

For `SMB`:

- stable file identity
- rename correctness
- lease / oplock correctness expectations
- notification behavior under movement
- Samba VFS integration model for case-insensitive matching and lease-break events

For `iSCSI`:

- durability requirements for active backing files
- pin policy for active LUN backing files
- write-ordering expectations
- fsync / barrier expectations
- `O_DIRECT` support in the Phase 1 lower-fs set

### 0.8 Failure and Repair Model

Phase 0 must define repair behavior for:

- destination partial copy after crash
- verified destination with no committed cutover
- committed cutover with source cleanup incomplete
- stale movement intent after policy change
- tier disappears mid-transaction
- metadata record and on-disk reality disagree
- kernel module reload mid-transaction

The output must include a deterministic reconcile algorithm, not just a statement that "tierd repairs it."

### 0.9 Conformance Test Plan

Before Phase 1 starts, Phase 0 must produce the test plan that future phases are required to pass.

Framework choice: **`xfstests`** as the POSIX/filesystem conformance baseline plus a bespoke smoothfs suite for movement and crash injection. `pjdfstest` is retained as a supplementary sanity layer.

At minimum:

- POSIX-focused functional tests (`xfstests generic/` + `shared/`)
- crash-replay tests at every movement transition (bespoke)
- mmap tests, including `MAP_SHARED` write refusal during movement
- hard-link and rename tests
- lower-fs compatibility tests against XFS-on-LV and ZFS in Phase 1; xfs, btrfs, and ext4 expanded and re-validated in Phase 3
- protocol tests for `NFS`, `SMB`, and `iSCSI` in their later phases (`cthon04`, `smbtorture`, LIO self-tests)
- performance regression harness matching `fuse-ns-create-fast-path.md`'s 10×1000-file rig

### 0.10 Phase 0 Exit Criteria

Phase 1 may not start until all of the following are true:

- the Phase 0 contract document exists, is reviewed, and is accepted by a named engineering reviewer with VFS / stacked-filesystem experience
- the conformance test plan exists and has a named owner for harness bring-up
- the performance regression harness exists and produces a baseline number against the current FUSE stack
- the kernel version matrix and DKMS / packaging plan (see §Operational Delivery) is agreed
- no open questions remain in §0.1–0.8

---

## Operational Delivery

smoothfs is an out-of-tree kernel module on a Debian-based appliance (see `iso/` and `README.md`). Kernel delivery is a first-class concern, not Phase 7 cleanup.

The kernel-build harness lives in [`RakuenSoftware/smoothkernel`](https://github.com/RakuenSoftware/smoothkernel) (Phase 2.3), shared with any future Smooth* appliance OS. SmoothNAS's per-OS bits are: a `.config` (seed from a known-good test box), `LOCALVERSION=-smoothnas-lts`, the `smoothfs` out-of-tree module under `src/smoothfs/`. The bump-the-kernel-pin runbook is at [`smoothkernel/docs/bumping-kernel.md`](https://github.com/RakuenSoftware/smoothkernel/blob/main/docs/bumping-kernel.md).

### Kernel version matrix

- Target appliance kernel: Debian-based LTS, 6.x series, pinned per SmoothNAS release.
- Supported kernel range pinned per smoothfs release; kernels outside the range refuse to load the module.
- Kernel upgrade path: each appliance release ships a new module build targeted at the new kernel; smoothfs is blocked from loading until the matching module is present.

### Packaging

- **DKMS package** for developer and debug builds.
- **Prebuilt signed kernel module** for appliance releases. Signed with the SmoothNAS module-signing key; refuses to load on lockdown-enforced kernels without a matching signature.
- Module source shipped alongside the appliance image.

### Upgrade and rollback

- The appliance updater verifies a matching module build is present for every pinned kernel before completing an upgrade.
- Downgrade: the previous appliance build's module is retained for one release. Rolling back the appliance unloads the current module and loads the previous build's.
- **Fail-safe:** if the module refuses to load for any reason, affected smoothfs pools stay un-mounted and the UI surfaces a clear status. Shared data on those pools becomes unavailable; data on the fastest lower remains reachable via a documented emergency-mount procedure.

### Build and CI

- CI builds the module against every pinned kernel in the supported matrix on every merge to main.
- CI runs `xfstests` + the smoothfs bespoke suite against at least one lower (XFS initially) on every merge.
- Nightly CI expands to ZFS and to the crash-injection suite.

---

## Phased Delivery

### Phase 0 — Contract and Conformance Spec

Define the semantics before implementation.

Deliverables:

- object identity model
- authoritative placement model
- movement transaction model
- concurrency semantics for open/write/mmap/rename/unlink/link/lock during movement
- heat and policy contract
- lower-fs capability contract
- protocol invariants for `NFS`, `SMB`, and `iSCSI`
- crash / repair model
- conformance test plan
- Phase 0 exit criteria satisfied (§0.10)

### Phase 1 — Core Stacked Filesystem

Deliver the basic filesystem on the bare-minimum lower-fs set: **XFS-on-LV** and **ZFS**.

Scope:

- mount / unmount
- the Phase 1 VFS op surface: lookup / create / read / write / setattr / readdir / symlink / readlink / link / unlink / rename / fsync / statfs / xattr / ACL / lock
- stable object identity (`object_id`)
- durable placement metadata
- passthrough data path (no copy on read/write)
- no automatic promotion/demotion yet
- no protocol exports yet
- packaging: DKMS build working; signed-module build pipeline stood up

### Phase 2 — Heat Engine and Journaled Movement

Add real up/down tiering.

Scope:

- heat sampling (re-homed from the placeholder fields in `meta.Record`)
- policy with hysteresis
- promotion and demotion planning
- copy / verify / cutover / cleanup, reusing the zfsmgd movement workers where possible
- crash recovery and reconciliation
- degraded-state reporting
- `MAP_SHARED` write refusal during movement
- hardlink-set pinning

### Phase 3 — Lower-FS Compatibility Expansion

Broaden support beyond the Phase 1 bare minimum.

Scope:

- **`xfs`** re-validation as part of the broader published matrix
- **`btrfs`** (with reflink and subvolume handling)
- **`ext4`**
- lower-fs capability probe at mount time
- compatibility matrix publication

Each additional lower-fs class must pass capability and correctness tests before claiming support.

### Phase 4 — NFS

Add `NFS` export support only after identity and movement semantics are proven.

Scope:

- stable export handles based on `object_id`
- deterministic generation counter
- reconnect / reboot correctness
- movement-safe handle resolution
- `cthon04` clean run

### Phase 5 — SMB

Add `SMB` support on top of the proven namespace.

Scope:

- share-mode / lease validation
- rename and movement behavior under Samba workloads
- notification and identity correctness
- Samba VFS module integration for case-insensitive matching and lease-break events
- `smbtorture` clean run

### Phase 6 — iSCSI

Add `iSCSI` support for file-backed LUNs on `smoothfs`.

Scope:

- active-write durability validation
- LUN backing-file pin policy (default pinned)
- target restart / reconnect correctness
- `O_DIRECT` conformance in the LUN path

Active-LUN movement while the backing file is serving block IO is explicitly **out** of Phase 6 — see §Phase 8 below.

### Phase 7 — Appliance Integration and Hardening

Integrate fully into SmoothNAS as a supported product capability.

Scope:

- pool creation / management UI (new-pool flow only; migration is manual)
- policy and observability UI
- repair and reconcile tooling
- update / rollback flow for the kernel module (end-to-end)
- support matrix publication
- Samba VFS module shipped and enabled for `SMB` shares on smoothfs pools
- operator runbook

### Phase 8 — Active-LUN Movement Model

Lift the "LUN backing files are pinned by default, and only an administrative target quiesce clears the pin" ruling from Phase 0 §iSCSI so tierd can move a live LUN between tiers. This deferred work now lives in [`smoothfs-active-lun-movement.md`](../completed/smoothfs-active-lun-movement.md). Phase 6.5's `CreateFileBackedTarget` / `DestroyFileBackedTarget` lifecycle and Phase 6.2's `trusted.smoothfs.lun` pin contract are the only smoothfs-side prerequisites; everything else in Phase 8 is new.

Scope:

- **operator quiesce protocol** — a tierd command (REST / CLI) that stops the initiator path for a given LUN, then clears `PIN_LUN` via `UnpinLUN`. Must be visible to the target's clients (quiesce should either fail the request with a clear error or cleanly drain outstanding I/O before unpinning).
- **journaled active-LUN move** — reuses the Phase 2 cutover state machine (`plan_accepted → copy_in_progress → copy_verified → cutover_in_progress → switched`) but with an extra gate: `MOVE_PLAN` on a file that was `PIN_LUN` at quiesce time must refuse unless the quiesce state is still held. If the admin re-opens the LUN before cutover, the move rolls back to the pinned-and-live state.
- **re-pin on cutover** — after `SMOOTHFS_MS_SWITCHED`, tierd re-installs `PIN_LUN` on the dest-tier file before re-enabling the target. No window where LIO opens a file that isn't pinned.
- **crash recovery** — if tierd crashes mid-quiesce or mid-move, the recovery path (already in place for Phases 2 and 5.3) must either finish the move and re-pin, or roll back and re-pin, per the Phase 0 §0.8 algorithm. No state where the backing file is unpinned with a live LIO target on top.
- **correctness tests** — analog of Phase 5's `TestE2ESMBForcedMoveBreaksLease`: hold an open SCSI session, drive the quiesce, move the backing file, reopen, verify zero data loss and correct read bytes post-move. Plus a fault-injection run that kills tierd mid-move and checks the recovery story.

Gating: Phase 8 does not start until Phase 6 has seen production time on at least one real pool and no LUN-adjacent correctness bugs have surfaced. The intentional gap between Phase 6 and Phase 8 is the "soak period" the Phase 0 §iSCSI ruling implies — the contract exists to keep operators out of trouble; Phase 8 only relaxes it once we have evidence the contract is well-formed.

---

## Risks

| Risk | Why it matters | Mitigation |
|---|---|---|
| Kernel filesystem complexity | Stacked VFS correctness is hard even before tiering | Do not start without a VFS-fluent engineer and a conformance-first plan (§0.10) |
| Movement semantics under concurrency | Open/write/mmap/rename/unlink during movement can corrupt semantics | Make movement a journaled state machine with explicit restrictions by phase; reuse zfsmgd's existing copy/verify/cutover workers |
| Lower-fs compatibility sprawl | Broad support claims hide semantic mismatches | Publish and enforce a tested compatibility matrix; Phase 1 bare minimum is XFS-on-LV + ZFS only |
| Protocol assumptions | `NFS`, `SMB`, and `iSCSI` depend on stable semantics | Phase protocol support after filesystem invariants are proven |
| Appliance kernel delivery | Out-of-tree kernel modules complicate update and rollback on a signed-kernel appliance | Treat packaging, signing, and kernel matrix as first-class (§Operational Delivery), not cleanup |
| Divergence from parallel work | fuse-ns-create-fast-path, unified-tiering-04b, unified-tiering-06, and mdadm heat engine are all in flight | §Relationship to In-Flight Work names the boundary for each; no semantics divergence allowed |
| Skill acquisition | No current contributor has production kernel-filesystem experience | Phase 0 exit criteria requires a named VFS reviewer; Phase 1 blocked until the role is filled |
| Performance targets not met | The whole premise is a latency floor; failing to beat FUSE kills the case | §Non-Functional Targets are gating for Phase 3; Phase 0 establishes the harness baseline |

---

## Plan

1. Land this rewritten proposal for discussion.
2. Write the Phase 0 contract document before kernel implementation starts.
3. Satisfy Phase 0.10 exit criteria (named reviewer, harness baseline, kernel matrix agreed).
4. Build Phase 1 against the bare-minimum lower-fs set: XFS-on-LV and ZFS.
5. Add journaled promotion/demotion in Phase 2, reusing zfsmgd movement workers where possible.
6. Phase 3 expands the smoothfs lower-fs matrix to xfs, btrfs, and ext4 only after the capability probe gates pass and the compatibility matrix is validated.
7. Protocol phases 4–6 only after §0.7 invariants are proven against the relevant lowers.
8. Phase 7 wires appliance integration, the Samba VFS module, and the kernel update/rollback path.
9. Phase 8 only starts after Phase 6 has soaked in production: lifts the "LUN backing files pinned by default" ruling by adding operator quiesce + journaled active-LUN move + cutover re-pin, reusing Phase 6.5's pin lifecycle and Phase 2's movement state machine.

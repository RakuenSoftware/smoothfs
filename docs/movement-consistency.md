# smoothfs movement consistency argument

This document records the consistency argument for smoothfs file movement. It is
intended to make review boundaries explicit: what the current kernel and
control-plane code guarantees, what conditions userspace must satisfy, and what
risks remain outside the proof.

## Claim

For a successful normal movement:

1. The destination bytes copied by userspace match a stable source snapshot.
2. No data-changing operation commits between userspace verification and the
   kernel lower-path swap.
3. The smoothfs inode publishes the destination lower path atomically by
   switching `lower_path` and incrementing `cutover_gen`.
4. Existing file descriptors lazily reopen on the new lower path after
   `cutover_gen` advances.

If any required condition fails, movement must abort or retry rather than
publish a stale destination.

The claim applies to regular files. Direct active-LUN movement is excluded from
normal background planning and uses the explicit quiesced-LUN path.

## Actors

- Kernel movement state machine: `src/smoothfs/movement.c`
- Lower-file reissue: `src/smoothfs/lower.c`
- Data-change accounting: `src/smoothfs/file.c`, `src/smoothfs/inode.c`, and
  helpers in `src/smoothfs/smoothfs.h`
- Range staging and replay: `src/smoothfs/file.c`, `src/smoothfs/range_staging.c`
- Userspace worker: `controlplane/worker.go`
- Planner and active-LUN opt-in path: `controlplane/planner.go`,
  `controlplane/lun_plan.go`

## Normal Movement Sequence

1. The planner selects a `smoothfs_objects` row in `movement_state = 'placed'`.
2. The worker calls `INSPECT` and records the current kernel `write_seq` when
   the kernel reports it.
3. The worker sends `MOVE_PLAN` with pool UUID, object id, destination tier, and
   transaction sequence.
4. The kernel accepts the plan only if the inode is still placed, the
   destination differs from the current tier, the pool is not quiesced, the
   object is regular, and movement blockers are absent.
5. The worker copies source bytes to the destination lower path.
6. The worker verifies the destination hash, then rechecks source size, source
   mtime, source hash, and kernel `write_seq`.
7. The worker sends `MOVE_CUTOVER`, including the expected `write_seq` when
   available.
8. The kernel transitions to `cutover_in_progress`, drains in-flight data
   changes with SRCU, revalidates the transaction sequence and `write_seq`,
   resolves the destination by `rel_path`, swaps `lower_path`, bumps
   `cutover_gen`, and publishes `switched`.
9. The worker removes the old source and finalizes the database row back to
   `placed` on the destination tier.

## Kernel Acceptance Gates

`MOVE_PLAN` refuses movement when:

- the pool is quiesced
- the destination rank is invalid or equals the current rank
- the inode is not in `placed`
- the inode is not a regular file
- `pin_state` is not `none`, except forced movement may bypass only `pin_lease`
- the observed link count is greater than one
- the current lower mapping has writable shared mappings

`MOVE_CUTOVER` refuses or fails when:

- the pool is quiesced
- the object id no longer resolves
- the intended tier is invalid
- the transaction sequence differs
- the state is not a permitted pre-cutover state
- writable shared mappings are still present
- the expected `write_seq` no longer matches
- the destination lower dentry cannot be resolved

## Data-Change Coverage

Data-changing paths are covered by two mechanisms:

- an SRCU read-side critical section that lets cutover drain in-flight changes
- a per-inode `write_seq` increment after successful changes

Covered paths include:

| Path | Barrier | Sequence update |
| --- | --- | --- |
| buffered and direct `write_iter` | `cutover_srcu` in `smoothfs_write_iter` | `smoothfs_note_data_change` on positive write |
| range-staged buffered write | `cutover_srcu` in `smoothfs_write_iter` | `smoothfs_note_data_change` on positive staged write |
| `splice_write` | `smoothfs_begin_data_change` | `smoothfs_note_data_change` on positive write |
| `fallocate` | `smoothfs_begin_data_change` | `smoothfs_note_data_change` on success |
| `remap_file_range` / clone range | `smoothfs_begin_data_change` | `smoothfs_note_data_change` on success |
| clone ioctls | `smoothfs_begin_data_change` | `smoothfs_note_data_change` on success |
| truncate / size-changing setattr | `smoothfs_begin_data_change` in inode setattr | `smoothfs_note_data_change` on success |

The worker performs an independent source hash re-read before cutover. This
catches same-size, same-mtime source mutations before the final kernel
`write_seq` check.

## mmap Handling

smoothfs delegates mmap to the lower file and rebinds `vma->vm_file` to the
lower. As a result, writable shared VMAs are visible on the lower mapping and
`mapping_writably_mapped(lower_inode->i_mapping)` is the movement gate.

Normal movement is refused while a writable shared mapping exists. The admin
override path `REVOKE_MAPPINGS` zaps PTEs and marks the inode
`mappings_quiesced`, but movement remains blocked until holders unmap and the
lower mapping is no longer writably mapped. New writable shared mmaps are
refused while `mappings_quiesced` is set.

Private mappings are not a movement blocker because private dirty pages do not
commit to the shared lower file.

## O_DIRECT Handling

O_DIRECT opens are allowed when the lower supports direct I/O and the smoothfs
file mirrors `FMODE_CAN_ODIRECT`.

Direct I/O goes through `write_iter`, so it participates in the SRCU/write-seq
movement barrier. When an inode has active range-staged bytes, direct reads and
writes return `-EBUSY` because the direct path cannot merge the staged overlay
with lower bytes.

## Range-Staging Interaction

Range staging stores buffered writes to a fastest-tier sidecar and overlays
those ranges on buffered reads. The stage is persisted so remount can recover
the staged ranges before drain.

Movement consistency has an additional precondition when `range_staged` is
true:

- Either drain the staged ranges to the authoritative lower path before
  movement, or copy from the smoothfs namespace/merged view rather than copying
  directly from the source lower path.

The runtime harness `write_staging_direct_cutover.sh` exercises the merged-copy
case by copying through the smoothfs mount path before `MOVE_CUTOVER`.

The kernel `INSPECT` response reports whether an object currently has active
range-staged bytes. The in-repository `controlplane.Worker` copies from lower
paths directly, so it refuses movement when `INSPECT` reports `range_staged`
before `MOVE_PLAN` and rechecks the flag after copy before `MOVE_CUTOVER`.
Custom integrations that copy from the smoothfs namespace/merged view may still
exercise the merged-copy path directly.

## Active-LUN Interaction

LUN backing files are protected by `pin_lun`. Normal planner movement skips all
non-`none` pin states, and the worker rejects a kernel inspect result that still
reports `pin_lun`.

The only supported active-LUN movement path is:

1. Stop and drain the target that owns the backing file.
2. Clear `trusted.smoothfs.lun` on the source.
3. Verify kernel inspect reports `pin_state = none` while the DB row still
   records `pin_lun`.
4. Build a movement plan with `RePinLUN`.
5. Move and cut over the file.
6. Verify destination placement.
7. Set `trusted.smoothfs.lun` on the destination.
8. Verify the destination pin is visible.
9. Resume the target.

If failure happens before cutover, the worker attempts to re-pin the source and
resume the target. If failure happens after cutover, the DB row is marked failed
while preserving the destination tier as current.

## Existing File Descriptors

Each open smoothfs file stores:

- the opened lower file
- the `cutover_gen` observed when the lower was opened
- the credentials and flags needed to reopen the destination lower

After cutover increments `cutover_gen`, the next `smoothfs_lower_file` lookup
lazily reopens the lower file against the new `lower_path`.

If destination reopen fails, `smoothfs_lower_file` returns an error pointer and
file operations propagate the error. This fails the existing fd closed instead
of serving or writing through the stale source lower after cutover.

## Failure and Recovery

Pre-cutover failures leave the current tier authoritative. Recovery rolls
`plan_accepted`, `destination_reserved`, `copy_in_progress`, `copy_complete`,
and `copy_verified` rows back to `placed` on `current_tier_id`.

Post-cutover states have already made the destination authoritative. Recovery
forwards `cutover_in_progress`, `switched`, and `cleanup_in_progress` rows to
`placed` on `intended_tier_id` when the destination tier is present.

Kernel placement records are useful for observability and replay, but the proof
does not depend on every asynchronous placement write reaching disk before the
worker commits the database row.

## Remaining Review Items

- Add runtime coverage for existing writable fd behavior across cutover,
  including destination reopen failure if it can be induced safely.

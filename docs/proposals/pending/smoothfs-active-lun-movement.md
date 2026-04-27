# Proposal: smoothfs — Active-LUN Movement Model

**Status:** In progress
**Parent:** [`smoothfs-stacked-tiering.md`](../completed/smoothfs-stacked-tiering.md)

---

## Purpose

Lift the v1 rule that LUN backing files are pinned by default and moveable
only after administrative quiesce. Phase 6.5's file-backed target lifecycle
and Phase 6.2's `trusted.smoothfs.lun` pin contract are prerequisites; the
active movement model is separate work gated on production soak.

## Scope

- **Operator quiesce protocol:** a tierd REST / CLI command stops the
  initiator path for a LUN, then clears `PIN_LUN` via `UnpinLUN`. The quiesce
  must either drain outstanding I/O cleanly or fail with an explicit error.
- **Journaled active-LUN move:** reuse the Phase 2 cutover state machine
  (`plan_accepted -> copy_in_progress -> copy_verified ->
  cutover_in_progress -> switched`) with an extra gate: `MOVE_PLAN` on a file
  that was `PIN_LUN` at quiesce time refuses unless the quiesce state is still
  held.
- **Re-pin on cutover:** after `SMOOTHFS_MS_SWITCHED`, tierd reinstalls
  `PIN_LUN` on the destination-tier file before re-enabling the target.
- **Crash recovery:** if tierd crashes mid-quiesce or mid-move, recovery must
  either finish the move and re-pin or roll back and re-pin. There must be no
  state where a live LIO target sits on an unpinned backing file.
- **Correctness tests:** hold an open SCSI session, quiesce, move the backing
  file, reopen, and verify byte-identical reads. Add a fault-injection run
  that kills tierd mid-move and verifies recovery.

## Progress

- Worker-side movement admission refuses `PIN_LUN` objects before issuing
  `MOVE_PLAN`, returning `ErrLUNQuiesceRequired`. This closes the bypass where
  a direct worker caller could submit a LUN movement even though the planner
  already skips pinned objects.
- Movement plans can now request destination re-pin for a quiesced LUN move.
  The worker installs `trusted.smoothfs.lun` on the destination file after
  `MOVE_CUTOVER` succeeds and before cleanup/finalization marks the move
  complete.
- Quiesced LUN movement now has a dedicated plan builder. It requires the
  kernel pin to be cleared, requires the DB row to still record `pin_lun` as
  the re-pin obligation, and emits only an opt-in plan with `RePinLUN` set.
- The control-plane library now exposes idempotent `ClearLUNPin` cleanup for
  the administrative quiesce path, clearing `trusted.smoothfs.lun` after the
  target has been stopped or drained.
- Quiesced LUN plan preparation now composes pin clear plus plan build and
  fails closed: if plan construction cannot proceed after clearing the xattr,
  the helper re-installs `trusted.smoothfs.lun` before returning the error.
- The control-plane preparation path now requires a `LUNTargetQuiescer`
  implementation to stop or drain the target before the pin is cleared, and
  resumes the target if plan preparation fails.
- Prepared LUN movement plans now carry the target ID through to the worker,
  letting the worker resume the target only after destination cutover and
  `trusted.smoothfs.lun` re-pin have completed.
- Workers now expose a `NewWorkerWithLUNResumer` wiring point and fail prepared
  LUN moves with `ErrLUNResumeRequired` if a target ID is present but no
  resumer is installed, avoiding silent completion with the target still
  stopped.
- Recovery now preserves failed `pin_lun` rows that already have an intended
  destination: the object is left failed for operator attention, but
  `current_tier_id` is advanced to the destination so the DB reflects the
  already cut-over, re-pinned backing file.
- Worker pre-cutover failures for prepared LUN moves now roll back the quiesce
  window by re-installing `trusted.smoothfs.lun` on the source file and
  resuming the target before reporting the movement failure.
- The rollback path now keeps the target stopped if source re-pin fails,
  preventing a live LIO target from reopening an unpinned backing file.
- Quiesced LUN plan building now rejects stale placement when kernel
  inspection reports a current tier rank that does not match the DB source
  tier.
- Prepared LUN moves now re-inspect placement after cutover and before
  re-pin or target resume, failing closed if the kernel still reports a stale
  destination tier or rel_path.
- Destination re-pin is now verified with a final kernel inspect before the
  worker records `pin_lun` or resumes the target, so a successful xattr write
  is not trusted until `PIN_LUN` is visible.
- Worker-side prepared LUN moves now require the DB row to still record a
  placed `pin_lun` object on the plan source tier before `MOVE_PLAN`, closing
  the direct-worker bypass around the quiesced LUN plan builder.
- Prepared LUN worker admission now also checks the kernel source tier rank
  against the plan source rank before `MOVE_PLAN`, so stale placement is
  rejected even for direct worker callers.
- Prepared LUN worker admission now also requires kernel `PinNone` before
  `MOVE_PLAN`, rejecting any non-quiesced pin state for direct worker callers.
- Prepared LUN worker admission now rejects source rel_path drift between
  kernel inspect and plan input before `MOVE_PLAN`, preventing stale-path
  direct worker submissions.
- Prepared LUN worker admission now also verifies kernel `CurrentTierPath`
  against the computed source file path before `MOVE_PLAN`, blocking
  stale-path direct worker submissions even when rel_path strings match.
- Post-cutover destination verification now also checks kernel
  `CurrentTierPath` against the computed destination file path before re-pin
  or resume, preventing stale destination path hints from passing.

## Gating

Phase 8 does not start until Phase 6 has seen production time on at least one
real pool and no LUN-adjacent correctness bugs have surfaced. Until then,
active-LUN movement remains unsupported in v1, as documented in
`docs/smoothfs-support-matrix.md`.

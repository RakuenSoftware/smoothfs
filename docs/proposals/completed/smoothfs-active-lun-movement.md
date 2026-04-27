# Proposal: smoothfs — Active-LUN Movement Model

**Status:** Complete on 2026-04-27 in `#58`.
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
- Recovery also re-applies `trusted.smoothfs.lun` during startup for in-flight
  LUN rows after moving state is reconciled, so resumed service enforces
  pin-based safety on the final tier path used by recovery.
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
- Prepared LUN worker admission now rejects no-op destination tiers before
  `MOVE_PLAN`, so direct worker callers cannot bypass the builder's
  destination-tier validation.
- Prepared LUN worker admission now checks DB `rel_path` against non-empty
  plan `RelPath` before `MOVE_PLAN`, rejecting stale direct-worker plan paths
  even when kernel inspect omits a path hint.
- Prepared LUN worker admission now requires an explicit plan `RelPath` when
  DB `rel_path` is already known, preventing direct-worker callers from
  bypassing DB path consistency checks with an empty rel_path.
- Prepared LUN worker admission now requires explicit non-placeholder
  `RelPath` for all `RePinLUN` plans before `MOVE_PLAN`, so direct worker
  callers cannot fall back to kernel inspect rel_path.
- Pre-cutover rollback now verifies that source re-pin is visible in kernel
  inspect before target resume, preventing resume onto an unpinned source file
  when xattr writes succeed but pin state does not converge.
- Prepared LUN worker admission now requires explicit source and destination
  lower directories before `MOVE_PLAN`, preventing direct-worker plan payloads
  from falling back to implicit relative paths.
- Prepared LUN worker admission now requires those lower directories to be
  absolute paths, rejecting relative direct-worker payloads before
  `MOVE_PLAN`.
- Prepared LUN worker admission now requires distinct source and destination
  lower directories before `MOVE_PLAN`, preventing same-path direct-worker
  payloads from issuing a destructive no-op copy plan.
- Prepared LUN worker admission now rejects missing source or destination tier
  IDs before `MOVE_PLAN`, preventing malformed direct-worker tier payloads.
- Prepared LUN worker admission now enforces normalized relative `RelPath`
  before `MOVE_PLAN`, rejecting traversal-like or non-normalized direct-worker
  path payloads.

## Gating

Phase 8 is implemented as an operator-gated movement flow. It is rollout-gated
outside controlled production soak and remains subject to the same safety checks
before re-enabling active targets as documented in
`docs/smoothfs-support-matrix.md`.

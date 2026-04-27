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

## Gating

Phase 8 does not start until Phase 6 has seen production time on at least one
real pool and no LUN-adjacent correctness bugs have surfaced. Until then,
active-LUN movement remains unsupported in v1, as documented in
`docs/smoothfs-support-matrix.md`.

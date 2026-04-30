# smoothfs architecture

This document captures how the moving parts fit together in SmoothNAS deployments.

## 1) System boundary

smoothfs is split across three execution domains:

1. **Kernel filesystem driver** (`src/smoothfs`)
2. **Userspace control plane** (`controlplane`)
3. **Appliance integration** (outside this repo, currently `smoothnas`)

The kernel driver owns file resolution and movement state transitions.
The userspace control plane owns policy, planning cadence, and orchestration.
The appliance owns UI/API policy entry points and operational guardrails.

## 2) Core components

### Kernel driver (`src/smoothfs`)

- Provides stacked filesystem mount type `smoothfs`.
- Maintains object identity and per-object metadata in persistent xattrs.
- Exposes lifecycle events and command families via generic netlink.
- Implements copy/cutover-ready movement primitives.
- Supports write staging, range staging and movement-gating behavior required by
  higher layers.

### controlplane (`controlplane`)

- Inspects mounted pools and refreshes local cache.
- Computes movement candidates using EWMA heat + policy constraints.
- Issues planned movement commands to kernel (`MovePlan`, `MoveCutover`).
- Handles retries and terminal state transitions for non-terminal moves on startup.
- Persists:
  - pool and object state
  - movement history
  - movement policies and operator reconciliation reasons

### SmoothNAS integration surface

- Uses `tierd` API surface and shared auth/authorization flows.
- Exposes quiesce/reconcile, pool management, and movement logs in UI and CLI.
- Handles protocol-specific integration details:
  - SMB VFS module loading and event handling
  - iSCSI fileio pin behavior (`trusted.smoothfs.lun`)
  - install / upgrade / rollback choreography

## 3) Normal movement flow

```text
Heat sampling / policy evaluation
             │
             ▼
      controlplane planner
             │ emits plan
             ▼
    controlplane worker (copy + verify)
             │
      kernel MoveCutover
             │
       source cleanup + state log
```

Important control point: movement state is explicit and observable.

## 4) Active-LUN flow

Active-LUN backing files are not moved through normal background planner flow.
They use an operator-gated flow:

- target quiesced/drained
- source pin cleared
- prepared movement plan built with re-pin intent
- cutover + destination pin verification
- target resume

This flow exists to avoid exposing an unpinned active LUN to I/O clients.

## 5) Failure and restart behavior

- Any in-flight movement is reconciled on startup.
- Cutover-partial states are completed or rolled back deterministically.
- Pin state is re-applied where required before serving resumed clients.
- Movement logs remain available for diagnostics and escalation.

## 6) Observability

Operational visibility includes:

- movement state transitions in `smoothfs_movement_log`
- reconcile/quiesce operations
- tier and policy configuration in `control_plane_config`
- runtime kernel health and tests in repo scripts

## 7) Security and lifecycle

- Module signing and MOK helper scripts are part of DKMS packaging.
- Kernel and userspace versions are aligned by the support matrix.
- Upgrade/rollback behavior relies on per-kernel DKMS trees and explicit health checks.

## 8) Where to read next

- Operator execution: [`smoothfs-operator-runbook.md`](smoothfs-operator-runbook.md)
- Platform limits: [`smoothfs-support-matrix.md`](smoothfs-support-matrix.md)
- Control-plane schema contract: [`control-plane-schema.md`](control-plane-schema.md)
- UAPI compatibility policy: [`uapi-compatibility.md`](uapi-compatibility.md)
- Movement consistency: [`movement-consistency.md`](movement-consistency.md)
- Kernel/test matrix: [`kernel-test-matrix.md`](kernel-test-matrix.md)
- Release checklist: [`release-checklist.md`](release-checklist.md)
- Runtime implementation detail: [`../src/README.md`](../src/README.md)

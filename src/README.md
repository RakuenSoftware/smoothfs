# smoothfs runtime and kernel integration deep dive

This directory contains the runtime implementation that makes SmoothNAS-level
policies visible in the kernel.

## High-level architecture

`controlplane` is the policy engine. `src/smoothfs` is the filesystem driver and
system integration glue.

```text
Client I/O (NFS / SMB / iSCSI fileio)
        │
        ▼
smoothfs kernel module (stacked namespace)
        │
  per-file xattrs, placement, movement
  and lower-tier path resolution
        │
Lower filesystems (XFS / ext4 / btrfs / zfs)
```

### Responsibilities by layer

- **Kernel layer (`src/smoothfs`)**
  - Presents a synthetic filesystem (`smoothfs`) with stacked lower files.
  - Resolves namespace objects from persistent metadata:
    `trusted.smoothfs.oid` (UUIDv7), generation helpers, cached inode metadata.
  - Enforces movement behavior via movement state transitions in VFS operations.
  - Exposes control-plane attributes through a generic netlink family (`uapi_smoothfs.h`).
  - Implements data-path specializations such as heat sampling and write staging.

- **Userspace layer (`controlplane`)**
  - Discovers mounted pools and reconciles planner state.
  - Computes movement candidates from heat and policy.
  - Executes cutover in a journaled sequence:
    `plan_accepted -> copy_in_progress -> copy_verified -> cutover_in_progress -> switched`.
  - Performs startup recovery for in-flight non-terminal states.
  - Persists and exposes movement/audit records in SQLite.

- **Samba VFS integration (`src/smoothfs/samba-vfs`)**
  - Handles lease pin/notification semantics required for SMB correctness during
    movement scenarios.

- **LIO/iSCSI integration (`controlplane` + `tierd` consumers)**
  - Uses `trusted.smoothfs.lun` (`PIN_LUN`) for LUN backing-file safety.
  - Applies active-LUN movement only through operator-controlled flow.

## Main data path

1. A file in a smoothfs mount is identified by object metadata.
2. File opens and operations are served from the current tier's lower file.
3. Planner evaluates heat and intent; if policy allows, it emits a movement plan.
4. Worker performs copy/verify/cutover state transitions.
5. Kernel swaps per-object lower path at `cutover` and source is cleaned up.

## Control plane contract points

The kernel and userspace contracts share these command families:

- register/inspect/reprobe pool
- reconcile/quiesce/reasoned reconcile
- move planning and cutover
- policy push and tier-down controls

These are surfaced in `uapi.go` and `client.go` and implemented in the kernel
netlink handlers.

## Recovery and failure behavior

- non-terminal moves are replayed deterministically on startup
- if cutover reached success states, recovery advances to `placed` on destination
- if failure occurs pre-cutover, recovery rolls back to stable source state
- active-LUN specific moves preserve quiesced target state and require explicit
  re-pin+resume gating
- movement logs record terminal and failed transitions for post-mortem

## Why this matters for SmoothNAS

- operations can survive service restarts without leaving orphaned movement states
- active movement is explicit and audited instead of implicit background behavior
- protocol-facing services (NFS/SMB/iSCSI) remain integrated through stable pins,
  movement events, and recovery invariants

## Build and test pointers

This directory is built through DKMS packaging in `smoothfs/debian`.

For kernel-level test scripts:

- `smoothfs/test/kernel_upgrade.sh`
- `smoothfs/test/module_signing.sh`
- protocol and movement suites in `smoothfs/test/*.sh`

## Further reading

- [`uapi_smoothfs.h`](uapi_smoothfs.h): netlink schema
- [`controlplane/contract.go`](../controlplane/contract.go): internal movement contracts
- [`../docs/architecture.md`](../docs/architecture.md): cross-component architecture overview

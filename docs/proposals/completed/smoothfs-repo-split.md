# Proposal: split smoothfs into its own project

**Status:** Complete

Completed in this repository as the standalone `github.com/RakuenSoftware/smoothfs`
module. The repo now owns the kernel module, Samba VFS module, control-plane
package, smoothfs-specific docs, Debian packaging metadata, root `make test`
entry point, and GitHub Actions Go contract check. SmoothNAS consumes this
surface as an appliance integrator.

---

## Purpose

`smoothfs` has outgrown the "feature inside SmoothNAS" shape.

Today one repository owns all of these at once:

- the `smoothfs` kernel module in `src/smoothfs`
- the Samba VFS module in `src/smoothfs/samba-vfs`
- the smoothfs netlink client, planner, worker, and recovery code in `tierd/internal/tiering/smoothfs`
- the smoothfs API surface and UI pages in `tierd/internal/api/smoothfs.go` and `tierd-ui/src/pages/SmoothfsPools`
- smoothfs packaging, support docs, and operator runbooks

That makes it harder to:

- version the filesystem independently from the appliance
- release non-appliance consumers of `smoothfs`
- keep kernel-facing and appliance-facing review boundaries clean
- make ownership obvious across packaging, support, and CI

This proposal defines the split to a dedicated repository under `RakuenSoftware/smoothfs`, while keeping SmoothNAS as the appliance integrator under `RakuenSoftware/smoothnas`.

## Target Repository Boundary

### New canonical repo: `RakuenSoftware/smoothfs`

This repo should own source and release artifacts that are intrinsic to the filesystem product itself:

- `src/smoothfs`
- `src/smoothfs/samba-vfs`
- `tierd/internal/tiering/smoothfs`
- any future exported Go package for the smoothfs control plane client or service
- smoothfs-specific deb packaging and CI for:
  - `smoothfs-dkms`
  - `smoothfs-samba-vfs`
  - any standalone `smoothfsctl` or library artifacts
- smoothfs-specific docs:
  - support matrix
  - operator runbook
  - protocol / netlink contract
  - kernel compatibility policy

### Existing appliance repo: `RakuenSoftware/smoothnas`

This repo should own appliance assembly, distribution, and product integration:

- installer and first-boot flow in `iso/`
- appliance daemon and API outside smoothfs-specific packages
- React UI outside smoothfs-specific pages
- nginx/systemd deployment assets
- updater/release plumbing for the appliance image
- docs that describe SmoothNAS as a whole appliance

## Required Contract Between Repos

The split only works if SmoothNAS stops importing smoothfs internals directly from its own tree and instead consumes a stable contract.

The contract should be:

- a versioned Go module published by the new `smoothfs` repo for:
  - netlink client
  - pool lifecycle helpers
  - planner / service bootstrap, if SmoothNAS continues to host the control-plane process
- versioned Debian packages built by the new repo and consumed by SmoothNAS packaging/install flows
- versioned REST/UI integration points on the SmoothNAS side that depend on the published Go package, not copied source
- docs that clearly separate:
  - filesystem release notes
  - appliance release notes

The control-plane boundary matters most. SmoothNAS should either:

1. keep running smoothfs orchestration in-process inside `tierd`, but import it from `github.com/RakuenSoftware/smoothfs/...`, or
2. split smoothfs orchestration into its own daemon and have `tierd` talk to that daemon over a stable RPC/API boundary.

Option 1 is the lower-risk first move and is the assumed path for this proposal.

## Current Coupling Inventory

The extraction has to account for these existing dependencies.

### Source ownership mixed into SmoothNAS

- kernel module and DKMS packaging live under `src/smoothfs`
- Samba VFS module lives under `src/smoothfs/samba-vfs`
- smoothfs Go service code lives under `tierd/internal/tiering/smoothfs`
- SmoothNAS API handlers import that package directly
- `tierd/cmd/tierd/main.go` wires the smoothfs service directly into appliance startup

### Data model and API coupling

- SmoothNAS SQLite migrations already contain smoothfs-specific tables
- the REST surface is currently hosted in `tierd/internal/api/smoothfs.go`
- the UI contains a dedicated `SmoothfsPools` page and movement-log views

### Packaging and operational coupling

- the runbook assumes appliance-local installation of `smoothfs-dkms`, `smoothfs-samba-vfs`, and `tierd`
- support docs currently describe the appliance and filesystem as one release train

## Proposed Extraction Phases

### Phase 1: establish the standalone source tree

Create `RakuenSoftware/smoothfs` with these top-level areas:

- `kernel/` or `src/smoothfs/` for the module
- `samba-vfs/`
- `go/` or module-root Go package for netlink client + service code
- `packaging/`
- `docs/`

Initial move policy:

- preserve git history where possible
- keep file paths close to current names to reduce churn
- publish a Go module path under `github.com/RakuenSoftware/smoothfs`

Deliverables:

- new repo exists
- code is moved there
- standalone CI builds the kernel module, VFS module, and Go packages

### Phase 2: turn SmoothNAS into a consumer

Update `RakuenSoftware/smoothnas` to stop owning smoothfs source.

Concrete changes:

- remove in-tree `src/smoothfs`
- replace `tierd/internal/tiering/smoothfs` with imports from the new module
- keep SmoothNAS API handlers and UI pages, but make them depend on the published package
- pull smoothfs debs from smoothfs release artifacts instead of building them from the appliance repo

Deliverables:

- SmoothNAS builds without local smoothfs source
- appliance integration tests install smoothfs artifacts from published releases

### Phase 3: cleanly separate release trains

After SmoothNAS consumes published smoothfs artifacts, split release responsibilities:

- `smoothfs` repo owns kernel/VFS/control-plane release cadence and compatibility matrix
- `smoothnas` repo pins known-good smoothfs versions per appliance release
- updater, install, and support docs in SmoothNAS reference explicit smoothfs version pins

Deliverables:

- compatibility table maintained in one place
- appliance releases specify exact supported smoothfs versions

## Ownership After The Split

### smoothfs repo responsibilities

- kernel-facing changes
- protocol and movement-state semantics
- Samba VFS behavior
- smoothfs-specific packaging and signing pipeline
- filesystem CI, compatibility, and performance validation

### smoothnas repo responsibilities

- exposing smoothfs capabilities in the appliance UX
- appliance db migrations and API routes that model appliance state
- system integration with sharing, updater, installer, and auth
- appliance-level end-to-end validation

## Risks

### Split without a published Go contract

If SmoothNAS copies the Go package instead of importing it, the split becomes cosmetic and versioning stays broken.

### Split packaging after code moves

If the repo move lands before package publication is automated, SmoothNAS will lose its current install path and release automation will regress.

### Cross-repo schema drift

SmoothNAS owns the appliance database. smoothfs-owned Go code must not silently assume migrations that the appliance has not yet applied. Schema expectations need explicit version checks.

## Decision

`smoothfs` should become its own repository under `RakuenSoftware/smoothfs`.

The first implementation step is not "move files around"; it is to publish a stable `smoothfs` Go/package contract that SmoothNAS can consume. Once that contract exists, the kernel module, Samba VFS module, control-plane code, packaging, and smoothfs-specific docs can move without leaving SmoothNAS in a half-owned state.

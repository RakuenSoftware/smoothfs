# smoothfs

smoothfs is the storage engine behind SmoothNAS tiering. It gives SmoothNAS one
place to keep fast and slow data together while moving files between tiers
automatically.

What people use it for:

- keep hot data on SSD/NVMe and cold data on slower media
- preserve familiar NFS/SMB/iSCSI behavior while storage placement is optimized
- reduce cost without changing NAS workflows
- provide safer movement with explicit state transitions and recovery paths

For SmoothNAS operators this means:

- better performance where it matters
- simpler storage growth on mixed hardware
- predictable operations because active-LUN movement and recovery flows are explicit

## Why it is useful

- **One namespace, multiple media classes**: one mount path and one view for users.
- **Policy driven**: heat + policy determines movement while preserving POSIX
  correctness.
- **Production-aware controls**: quiesce/reconcile, movement logs, and explicit
  active-LUN workflow prevent surprise behavior.
- **Integrated package path**: kernel module, userspace daemon, and support
  scripts are delivered in Debian artifacts for SmoothNAS deployments.

## For SmoothNAS

SmoothNAS should consume this repository as an external component instead of
duplicating its implementation.

- kernel filesystem + movement engine: `src/smoothfs`
- userspace control-plane: `controlplane`
- API contracts: [`client.go`](client.go), [`events.go`](events.go),
  [`uapi.go`](uapi.go), [`pools.go`](pools.go)
- operator docs and phased delivery notes: [`docs`](docs)

## Documentation

English:

- [support matrix](docs/smoothfs-support-matrix.md)
- [operator runbook](docs/smoothfs-operator-runbook.md)
- [architecture](docs/architecture.md)
- [deep technical readme in `src`](src/README.md)

Dutch:

- [ondersteuningsmatrix](docs/smoothfs-support-matrix.nd.md)
- [runbook](docs/smoothfs-operator-runbook.nd.md)
- [architectuur](docs/architecture.nd.md)
- [technische uitleg in `src`](src/README.nd.md)

## Verification

```bash
go test ./...
```

Runtime behavior and kernel packaging tests are in [`src/smoothfs/test`](src/smoothfs/test)
and are intended for prepared Linux hosts with matching kernel/filesystem/Samba/iSCSI
prerequisites.

## Repo layout

- `src/smoothfs`: kernel module, DKMS, Samba VFS integration, and test harnesses
- `controlplane`: planner/worker/recovery service logic
- `docs`: operator and design documentation
- `tierd` directory in SmoothNAS consumes these contracts


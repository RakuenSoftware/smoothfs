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


### Storage modes

`mode=tier` (the default) preserves normal placement: each file has one
permanent tier, and promotion/demotion moves that authoritative copy.

`mode=cache` treats the **last, slowest tier as permanent storage**. The first,
fastest tier supplies temporary write staging and disposable read replicas:

```sh
mount -t smoothfs -o pool=media,mode=cache,tiers=/mnt/cache:/mnt/base none /mnt/media
```

New names and directories are created on base storage. Buffered writes use the
existing range-staging machinery while the fast tier has room, then drain into
the base inode. A full cache falls back to base writes; it does not add permanent
capacity. Background drain runs on the heat-drain interval (currently 30 seconds).
`fsync`, synchronous writes, `syncfs`, and clean unmount drain pending writes;
a failed base write retains dirty staging and reports the error. Pending writes
are required data until drained and must not be deleted as if they were clean
read replicas.

Buffered reads can populate anonymous fast-tier chunk replicas. Their validity
is tracked in memory and invalidated by writes, truncation, placement changes,
and writable mappings. Losing/evicting these replicas only causes another base
read. Unsupported temporary-file caching falls back to reading base storage.
Direct IO, splice, and mmap use base storage; dirty ranges are drained or the
operation is refused rather than bypassed.

`/sys/fs/smoothfs/<uuid>/storage_mode` reports the mounted mode. Writing `1` to
`drop_read_cache` evicts all clean read replicas without touching base files or
pending writes. Cache mode reCache mode reports permanent capacity from base storage only.

Do not switch an existing tiered pool in place: consolidate and verify its
contents on the base layer first. Managed creation rejects upper-layer names
without a matching base entry. Cache mode exposes only the base namespace;
stale named replicas above it do not resurrect deleted files. The fastest
layer provides caching and the last layer provides permanent storage;
intermediate configured layers are not used for cache admission.

## Licensing

The kernel module in `src/smoothfs/` is licensed under the GPL-2.0-or-later,
because code that runs inside the Linux kernel must be GPLv2-compatible. Its
userspace header `uapi_smoothfs.h` carries the Linux syscall note. The full
text is in [`src/smoothfs/LICENSE`](src/smoothfs/LICENSE).

Everything else is licensed under the [AGPL-3.0](LICENSE): the Go control
plane and client, the Samba VFS module, the test programs and the packaging
scripts. Each source file states its license in an SPDX header.

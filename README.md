# smoothfs

`smoothfs` is the standalone repository for RakuenSoftware's stacked
tiering filesystem and its userspace control-plane contract.

This repo owns:

- the Go module `github.com/RakuenSoftware/smoothfs`
- the `controlplane` package used by appliance consumers such as
  `RakuenSoftware/smoothnas`
- the kernel module and DKMS packaging under [`src/smoothfs`](src/smoothfs)
- the Samba VFS module under [`src/smoothfs/samba-vfs`](src/smoothfs/samba-vfs)
- smoothfs-owned operator and design docs under [`docs`](docs)

## Layout

- [`client.go`](client.go), [`events.go`](events.go), [`uapi.go`](uapi.go),
  [`pools.go`](pools.go): low-coupling userspace contract
- [`controlplane`](controlplane): planner, worker, recovery, service wiring
- [`src/smoothfs`](src/smoothfs): out-of-tree kernel module, DKMS package,
  Samba VFS module, shell harnesses
- [`docs`](docs): support matrix, operator runbook, historical phase docs

Documentation is maintained in English with Dutch translation files available:

- `docs/smoothfs-support-matrix.md` / [`docs/smoothfs-support-matrix.nd.md`](docs/smoothfs-support-matrix.nd.md)
- `docs/smoothfs-operator-runbook.md` / [`docs/smoothfs-operator-runbook.nd.md`](docs/smoothfs-operator-runbook.nd.md)

## Consumer contract

Appliance repos should import published smoothfs packages instead of
copying implementations in-tree.

Primary consumer paths in `RakuenSoftware/smoothnas`:

- `github.com/RakuenSoftware/smoothfs`
- `github.com/RakuenSoftware/smoothfs/controlplane`

SmoothNAS-specific end-to-end coverage stays in the appliance repo and
imports this module directly.

## Verification

Go packages:

```bash
go test ./...
```

Kernel / packaging / Samba harnesses live under [`src/smoothfs/test`](src/smoothfs/test)
and are intended to run on a prepared Linux host with the required
kernel, filesystem, Samba, and iSCSI prerequisites.

## Release notes

- Debian packaging metadata lives under [`src/smoothfs/debian`](src/smoothfs/debian)
  and [`src/smoothfs/samba-vfs/debian`](src/smoothfs/samba-vfs/debian).
- The appliance integration docs remain in `RakuenSoftware/smoothnas`;
  this repo owns the smoothfs implementation and implementation-specific
  support material.

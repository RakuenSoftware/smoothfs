# smoothfs kernel build and test matrix

This matrix records the kernel and runtime coverage expected before a smoothfs
change is promoted into a SmoothNAS release.

## Kernel Targets

| Target | Purpose | Command or CI job | Required before merge |
| --- | --- | --- | --- |
| Debian current headers (amd64) | Compile against the current Debian amd64 kernel header surface | CI `Kernel module compile (ubuntu-latest)`, local `make kernel-build-debian` on amd64 | Yes |
| Debian current headers (arm64) | Compile against the current Debian arm64 kernel header surface | CI `Kernel module compile (ubuntu-24.04-arm)`, local `make kernel-build-debian` on arm64 | Yes |
| Host native headers | Compile against the developer host kernel | `make kernel-build KDIR=/lib/modules/$(uname -r)/build` | Required only when headers are installed |
| Kernel upgrade path | DKMS skip/build behavior across unsupported and supported kernels | `SMOOTHFS_RUNTIME_SUITE=ops make runtime-harnesses` | Required before release |
| Secure Boot signing | DKMS module signing and MOK enrollment helper behavior | `module_signing.sh` in the ops runtime suite | Required before release |

The repository CI uses Debian headers because they are reproducible in GitHub
Actions and catch kernel API drift before merge. The `kernel-build-debian` target
selects `linux-headers-$(dpkg --print-architecture)` so the same recipe builds on
amd64 and arm64 hosted runners. smoothfs ships as DKMS source against any
matching upstream Linux kernel headers; there is no smoothfs-specific
"appliance kernel" target enforced by this repo.

## Static and Unit Gates

Run before every PR:

```sh
make verify
git diff --check
aimee git verify
```

`make verify` covers:

- `gofmt` drift
- `go vet`
- Go unit tests
- Go race tests
- shell syntax checks for runtime and packaging scripts
- Samba VFS multiarch packaging static checks
- runtime harness manifest validation

CI splits the same coverage into static checks, Go tests, and kernel compile
jobs so failures point at a narrow surface.

## Runtime Harness Suites

The runtime harness runner is:

```sh
make runtime-harnesses
```

By default it runs the core privileged suite. Optional suites are selected with
`SMOOTHFS_RUNTIME_SUITE`.

| Suite | Command | Coverage | Required before release |
| --- | --- | --- | --- |
| `core` | `make runtime-harnesses` | smoothfs-only mount, movement, open-fd cutover reissue/fail-closed behavior, write-staging, range-staging, O_DIRECT, netlink receive-close | Yes |
| `protocol` | `SMOOTHFS_RUNTIME_SUITE=protocol make runtime-harnesses` | NFS, SMB, Samba VFS, smbtorture, iSCSI | Yes for protocol release candidates |
| `ops` | `SMOOTHFS_RUNTIME_SUITE=ops make runtime-harnesses` | DKMS kernel upgrade and module signing | Yes for package release candidates |
| `all` | `SMOOTHFS_RUNTIME_SUITE=all make runtime-harnesses` | core + protocol + ops | Required for release signoff |

Dry-run manifest validation:

```sh
make runtime-harnesses-list
SMOOTHFS_RUNTIME_SUITE=all SMOOTHFS_RUNTIME_DRY_RUN=1 \
  bash src/smoothfs/test/run_runtime_harnesses.sh
```

Hosted CI (ubuntu-latest / ubuntu-24.04-arm) runs manifest validation,
go tests, kernel-module compile, and the `smoothfs-dkms.deb` build, on
both shipped CPU architectures. It does **not** run privileged harnesses
— those need root, loop devices, and external services that the
GitHub-hosted runners don't provide. Privileged harnesses run on
self-hosted runners labeled `self-hosted`, `linux`, and
`smoothfs-runtime-<arch>` (one of `smoothfs-runtime-amd64` or
`smoothfs-runtime-arm64`). Each arch needs its own registered runner.
That runner must provide passwordless `sudo`, loop devices, XFS tooling,
matching kernel headers when `module_mode = build-and-load`, the protocol
or DKMS packages required by the selected suite, and (for protocol/all
suites) a Samba source tree at `/tmp/samba-<installed-version>` so the
workflow can build `vfs_smoothfs.so` on demand.

The workflow has two trigger paths:

- **Auto-run on push to `main`** for pushes that touch `src/smoothfs/**`,
  `controlplane/**`, top-level `*.go`, `Makefile`, or the workflow itself.
  The push path runs the matrix `arch=[arm64, amd64]` with fixed
  `suite=core`, `module_mode=build-and-load`, `fail_fast=false`, so
  every change is gated on both shipped CPU architectures. This is the
  regression gate.
- **`workflow_dispatch`** for operators who want a different `suite`,
  `arch`, `module_mode`, `tests` subset, or `fail_fast`. Dispatch runs
  one arch at a time (the `arch` input).

To bring up a fresh self-hosted runner host, run
`src/smoothfs/test/runner-setup.sh` as root on the runner VM. The
script is idempotent and installs the apt prereqs (build essentials,
DKMS, `debhelper` + `dh-dkms` for `smoothfs-dkms.deb` builds,
Samba + samba-testsuite, NFS, iSCSI / targetcli-fb, mokutil, groff,
`time`, samba's build-deps with the trixie-backports `libngtcp2-dev`),
runs `apt-get source samba` so the VFS-build workflow step has its
source-tree prerequisite ready, clones and builds the `cthon04` test
suite at `/opt/cthon04`, fixes the `~/actions-runner/.path` to
include `/sbin` (without it the workflow's prereqs check fails on
`mkfs.xfs` / `insmod` / `modinfo`), and grants passwordless sudo to
the runner user. The kernel itself stays operator-managed: smoothfs
needs ≥ 6.18, only available on Debian 13 from `trixie-backports`.

Use the workflow inputs as follows:

| Input | Values | Purpose |
| --- | --- | --- |
| `arch` | `amd64`, `arm64` | Selects the self-hosted runner label `smoothfs-runtime-<arch>`. Must match a registered runner. |
| `suite` | `core`, `protocol`, `ops`, `all` | Selects the runtime suite when `tests` is empty. |
| `tests` | space-separated script names | Runs a targeted subset and overrides `suite`. |
| `module_mode` | `build-and-load`, `preinstalled` | Either builds `src/smoothfs/smoothfs.ko` against the runner kernel and loads it, or verifies a preinstalled module. |
| `fail_fast` | boolean | Stops after the first failing harness when enabled. |

Release signoff requires running each suite on **every shipped CPU
architecture** (currently `amd64` and `arm64`). Each architecture needs a
self-hosted runner registered with the matching `smoothfs-runtime-<arch>`
label.

## Lower Filesystem Matrix

Runtime coverage must include each supported lower filesystem from the support
matrix when the change touches lookup, movement, metadata replay, write staging,
or protocol export behavior. The runtime workflow exposes a `lower_fs` input
(xfs / ext4 / btrfs) that sets `SMOOTHFS_LOWER_FS` in the harness env; the
shared `lower_fs_lib.sh` helper picks the right `mkfs` per harness, and a
harness can opt itself out of an unsupported lower via `require_lower_fs`.

| Lower filesystem | Required coverage | Validated coverage (this matrix) |
| --- | --- | --- |
| XFS | Primary full run for core, SMB, iSCSI, and O_DIRECT coverage | Full: core 16/16, protocol 8/8, ops 2/2, both arches |
| ext4 | Core create/read/write/move validation | Full: core 16/16, protocol 8/8, ops 2/2, both arches |
| btrfs | Core movement plus reflink/remap validation | Core 15+1 SKIP both arches (`odirect` SKIPs because btrfs falls back to buffered I/O for misaligned O_DIRECT instead of returning EINVAL — that's an xfs/ext4-specific kernel semantic, not a smoothfs bug); protocol 8/8 both arches; ops 2/2 |
| zfs | Mount, lookup, movement, and protocol smoke when OpenZFS is in the appliance image | Not yet wired (zfs lowers need a `zpool` flow on a block device, not `mkfs` on a loopback file) |

Per-harness opt-outs in tree today:
- `odirect.sh` → `require_lower_fs xfs ext4` (btrfs O_DIRECT misalign semantics differ)

Unsupported lowers must fail capability probing with a clear error rather than
silently mounting.

## Protocol Matrix

| Protocol | Required harnesses | Release gate |
| --- | --- | --- |
| NFS v3/v4.2 | `cthon04.sh` plus connectable file-handle coverage | Required for NFS-facing releases |
| SMB 2/3 | `smb_roundtrip.sh`, `smbtorture.sh`, `smbtorture_xfs_baseline.sh`, `smb_vfs_module.sh` | Required for SMB-facing releases |
| iSCSI fileio | `iscsi_roundtrip.sh`, `iscsi_pin.sh`, `iscsi_restart.sh` | Required for iSCSI-facing releases |
| Active-LUN movement | Active-LUN worker tests plus controlled appliance soak | Required before enabling outside gated rollout |

## CI Artifact Mapping

| GitHub check | What it proves | What it does not prove |
| --- | --- | --- |
| Static checks | Go formatting/vet, shell syntax, Samba VFS packaging paths, runtime harness manifest | Real mounts, external services, kernel API compile |
| Go tests (amd64, arm64) | Control-plane unit behavior and race checks on each shipped CPU architecture | Real smoothfs kernel behavior |
| Kernel module compile (amd64, arm64) | The module builds against current Debian headers on each shipped CPU architecture | Runtime correctness, host-native developer kernel headers |
| `smoothfs-dkms.deb` build (amd64, arm64) | `debian/control` + `debian/rules` + `debian/smoothfs-dkms.install` are consistent with the kernel module's Kbuild source list and `dpkg-buildpackage` runs clean against the trixie samba/dkms toolchain | DKMS rebuild against an arbitrary host's kernel — that's covered by the ops suite via `kernel_upgrade.sh` on the self-hosted runner |
| Privileged runtime harnesses | Real smoothfs mounts and selected core/protocol/ops behavior on a labeled self-hosted runner. Push-to-`main` runs `core` on both arches as the regression gate; protocol/ops/all are operator-dispatched | GitHub-hosted PR safety, runners without root, or uninstalled protocol/DKMS dependencies |

## Failure Policy

- A kernel compile failure blocks merge.
- A runtime harness failure blocks release. It may block merge when the touched
  area is directly covered by that harness.
- A protocol harness failure blocks release for that protocol.
- An ops harness failure blocks package release.
- Missing host-native headers are a local environment limitation, not a pass.
  The release owner must provide an equivalent CI artifact.

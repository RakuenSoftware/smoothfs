# smoothfs kernel build and test matrix

This matrix records the kernel and runtime coverage expected before a smoothfs
change is promoted into a SmoothNAS release.

## Kernel Targets

| Target | Purpose | Command or CI job | Required before merge |
| --- | --- | --- | --- |
| Debian current headers (amd64) | Compile against the current Debian amd64 kernel header surface | CI `Kernel module compile (ubuntu-latest)`, local `make kernel-build-debian` on amd64 | Yes |
| Debian current headers (arm64) | Compile against the current Debian arm64 kernel header surface | CI `Kernel module compile (ubuntu-24.04-arm)`, local `make kernel-build-debian` on arm64 | Yes |
| Host native headers | Compile against the developer or appliance host kernel | `make kernel-build KDIR=/lib/modules/$(uname -r)/build` | Required only when headers are installed |
| SmoothNAS LTS kernel | Compile against the appliance kernel line, currently 6.18 LTS, on each shipped CPU architecture (amd64, arm64) | Appliance CI or release builder | Required before release |
| Kernel upgrade path | DKMS skip/build behavior across unsupported and supported kernels | `SMOOTHFS_RUNTIME_SUITE=ops make runtime-harnesses` | Required before release |
| Secure Boot signing | DKMS module signing and MOK enrollment helper behavior | `module_signing.sh` in the ops runtime suite | Required before release |

The repository CI uses Debian headers because they are reproducible in GitHub
Actions and catch kernel API drift before merge. The `kernel-build-debian` target
selects `linux-headers-$(dpkg --print-architecture)` so the same recipe builds on
amd64 and arm64 hosted runners. Appliance release CI must still compile against
the exact SmoothNAS kernel artifact shipped to users, on each shipped CPU
architecture.

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

Default hosted CI runs manifest validation but does not run privileged
harnesses. The manual GitHub Actions workflow `Privileged runtime harnesses`
runs the same suites on a self-hosted runner labeled `self-hosted`, `linux`,
and `smoothfs-runtime`. That runner must provide passwordless `sudo`, loop
devices, XFS tooling, matching kernel headers when `module_mode =
build-and-load`, and the protocol or DKMS packages required by the selected
suite.

Use the workflow inputs as follows:

| Input | Values | Purpose |
| --- | --- | --- |
| `suite` | `core`, `protocol`, `ops`, `all` | Selects the runtime suite when `tests` is empty. |
| `tests` | space-separated script names | Runs a targeted subset and overrides `suite`. |
| `module_mode` | `build-and-load`, `preinstalled` | Either builds `src/smoothfs/smoothfs.ko` against the runner kernel and loads it, or verifies a preinstalled module. |
| `fail_fast` | boolean | Stops after the first failing harness when enabled. |

## Lower Filesystem Matrix

Runtime coverage must include each supported lower filesystem from the support
matrix when the change touches lookup, movement, metadata replay, write staging,
or protocol export behavior.

| Lower filesystem | Required coverage |
| --- | --- |
| XFS | Primary full run for core, SMB, iSCSI, and O_DIRECT coverage |
| ext4 | Core create/read/write/move validation |
| btrfs | Core movement plus reflink/remap validation |
| zfs | Mount, lookup, movement, and protocol smoke when OpenZFS is in the appliance image |

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
| Kernel module compile (amd64, arm64) | The module builds against current Debian headers on each shipped CPU architecture | Runtime correctness, host-native Proxmox headers, SmoothNAS LTS kernel |
| Privileged runtime harnesses | Real smoothfs mounts and selected core/protocol/ops behavior on a labeled self-hosted runner | GitHub-hosted PR safety, runners without root, or uninstalled protocol/DKMS dependencies |

Any release candidate must attach or link the appliance CI artifacts that fill
the gaps above.

## Failure Policy

- A kernel compile failure blocks merge.
- A runtime harness failure blocks release. It may block merge when the touched
  area is directly covered by that harness.
- A protocol harness failure blocks release for that protocol.
- An ops harness failure blocks package release.
- Missing host-native headers are a local environment limitation, not a pass.
  The release owner must provide an equivalent CI artifact.

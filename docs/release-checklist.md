# smoothfs release checklist

Use this checklist for every SmoothNAS release that includes smoothfs kernel,
control-plane, Samba VFS, or documentation changes.

## 1. Version Alignment

- Record the smoothfs git commit included in the release.
- Record the SmoothNAS/tierd git commit that consumes it.
- Record package versions for:
  - `smoothfs-dkms`
  - `smoothfs-samba-vfs`
  - `tierd` or the SmoothNAS service bundle that embeds the control plane
- Confirm `docs/smoothfs-support-matrix.md` names the shipped kernel, Samba, and
  OpenZFS versions.
- Confirm `docs/uapi-compatibility.md` has been reviewed for any generic-netlink
  or fixed-record changes.
- Confirm `docs/control-plane-schema.md` matches the production migration set.

## 2. Source and Static Gates

Required repository checks:

```sh
make verify
git diff --check
make kernel-build-debian
```

If local container or host-header prerequisites are unavailable, attach the
corresponding CI artifact instead of marking the check skipped.

Required review checks:

- No command, attribute, state, pin, or file-handle id was renumbered.
- New UAPI fields are additive or versioned.
- New schema columns are nullable or have defaults unless migrations backfill
  existing rows.
- Operator-facing docs have been updated for new sysfs, netlink, CLI, UI, or
  package behavior.

## 3. Kernel and DKMS Package

- Build `smoothfs-dkms` for the release architecture set.
- Install on a clean appliance image.
- Verify DKMS builds on the supported 6.18+ kernel.
- Verify DKMS skips unsupported kernels according to `BUILD_EXCLUSIVE_KERNEL`.
- Verify `modprobe smoothfs` succeeds.
- Verify `/sys/module/smoothfs` and `/sys/fs/smoothfs` surfaces appear as
  expected after mount.
- Run module signing checks on Secure Boot-enabled images.
- Run the MOK enrollment helper on a fresh image when the release changes DKMS
  signing behavior.
- Confirm rollback removes or supersedes the DKMS tree without leaving a stale
  module selected for the running kernel.

## 4. Samba VFS Package

- Build `smoothfs-samba-vfs` against the exact Samba package version in the
  appliance image.
- Confirm package dependencies pin the exact Samba version.
- Confirm the module installs under the Debian multiarch VFS path.
- Start Samba and verify the VFS module loads without unresolved symbols.
- Run SMB protocol harnesses before release signoff.
- Rebuild this package for every Samba security update, even when smoothfs code
  is unchanged.

## 5. Control Plane and Schema

- Apply production migrations on a clean database.
- Apply production migrations on an upgraded database from the previous release.
- Run the drift-check queries from `docs/control-plane-schema.md`.
- Confirm all required `control_plane_config` keys exist.
- Confirm `smoothfs_objects.object_id` values are 32-character hex strings.
- Confirm movement recovery behavior for pre-cutover and post-cutover rows.
- Confirm active-LUN rows preserve `pin_lun` semantics through recovery.

## 6. Runtime Harnesses

Required before release signoff:

```sh
make runtime-harnesses
SMOOTHFS_RUNTIME_SUITE=protocol make runtime-harnesses
SMOOTHFS_RUNTIME_SUITE=ops make runtime-harnesses
```

Use `SMOOTHFS_RUNTIME_SUITE=all make runtime-harnesses` on a host where all
protocol services and package dependencies are available.

For CI-backed signoff, run the manual GitHub Actions workflow `Privileged
runtime harnesses` on a self-hosted runner labeled `smoothfs-runtime`. Use
`module_mode=build-and-load` for source-tree validation against the runner
kernel, or `module_mode=preinstalled` when validating packaged DKMS artifacts.

Attach logs for:

- core smoothfs movement, open-fd cutover, and write-staging harnesses
- NFS cthon04
- SMB roundtrip and smbtorture subset
- Samba VFS module load/roundtrip
- iSCSI roundtrip, pin, and restart
- DKMS kernel upgrade
- module signing

## 7. Appliance Integration

- Verify SmoothNAS creates placement domains, tier targets, namespaces, and
  object rows that match `docs/control-plane-schema.md`.
- Verify mount-ready events register pools without manual planner setup.
- Verify quiesce and reconcile controls work from UI/API/CLI.
- Verify movement logs are visible to operators.
- Verify disk-spindown write-staging masks are set only after the appliance has
  externally observed a non-fast tier active.
- Verify active-LUN movement remains gated behind the intended rollout flag or
  operator workflow.
- Verify alerting surfaces failed movement rows and range-staging recovery
  pending counters.

## 8. Upgrade and Rollback

- Upgrade from the previous supported release with existing smoothfs pools.
- Reboot after upgrade and confirm pools remount.
- Confirm range-staging replay runs and reports expected recovery status.
- Confirm in-flight movement recovery handles pre-cutover and post-cutover rows.
- Roll back packages and confirm old services either run safely or refuse with a
  clear version/support error.
- Confirm rollback does not strand active LUNs unpinned.

## 9. Release Signoff

The release owner signs off only after:

- all required CI jobs pass
- all required runtime harness suites pass
- appliance migration drift checks pass
- support matrix versions match shipped artifacts
- release notes call out UAPI, schema, package, and operator workflow changes
- rollback notes are written for DKMS, Samba VFS, and control-plane migrations

Do not promote a release with unexplained runtime harness failures. A skipped
harness needs a named owner, a reason, and a replacement artifact that covers the
same risk.

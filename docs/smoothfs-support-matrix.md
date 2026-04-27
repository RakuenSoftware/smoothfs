# smoothfs support matrix

[Dutch version](smoothfs-support-matrix.nd.md)

The smoothfs stack has three independently-versioned moving parts —
the kernel module, the Samba VFS module, and the tierd control plane —
plus the four lower filesystems we test against. This page pins the
combinations the smoothfs project tests for the SmoothNAS integration;
anything outside this set may work but has no support commitment.

Last reviewed against Phase 7.10 plus disk-spindown write-staging truncate rehome (Phase 0–7 complete; Phase 8 gated on production soak).

## Appliance OS

| Field | Value |
|---|---|
| Base OS | Debian 13 (trixie) |
| Libc | glibc 2.41 |
| Init | systemd ≥ 255 |
| Package format | `.deb` via apt |

Older Debian releases (12 / bookworm) are not supported because the kernel floor (§Kernel) excludes them.

## Kernel

| Field | Value |
|---|---|
| Minimum | **6.18.0 LTS** |
| Tested | 6.18.22-smoothnas-lts, 6.19.10+deb13-amd64 (Debian bpo) |
| DKMS floor | enforced by `BUILD_EXCLUSIVE_KERNEL="^(6\.(1[8-9]|[2-9][0-9])|[7-9]\.).*"` in `dkms.conf` |
| Why 6.18 | smoothfs uses `lookup_one`, `set_default_d_op`, the dentry-returning `vfs_mkdir`, the new `renamedata` shape, `vfs_mmap`, and the parent-inode + qstr `d_revalidate` signature — all of which landed by 6.18 |

Kernel version below 6.18 → DKMS silently skips building smoothfs for that kernel (Phase 7.3 test `kernel_upgrade.sh` verifies this). The appliance still boots; smoothfs pools won't mount.

## OpenZFS

| Field | Value |
|---|---|
| Version | **2.4.1** |
| Source | upstream OpenZFS built via SmoothKernel recipes (Phase 2.3), **not** Debian's `zfs-dkms` |
| Why | Debian's `zfs-dkms 2.3.2` caps at kernel 6.14 — incompatible with our 6.18 floor. OpenZFS 2.4.1 is the first upstream release that builds against 6.18+. |

Mixing Debian's zfs-dkms with smoothfs-dkms on the same host is **unsupported** — the DKMS autoinstall for zfs will fail silently on every kernel upgrade.

## Samba

| Field | Value |
|---|---|
| Version | **2:4.22.8+dfsg-0+deb13u1** (Debian 13 stock) |
| Vendor suffix | `Debian-4.22.8+dfsg-0+deb13u1` |
| VFS ABI pin | `smoothfs-samba-vfs` deb has `Depends: samba (= <exact version>)` — apt refuses install on a mismatched pair |
| Rebuild trigger | any Samba security update |

The `smoothfs-samba-vfs` package rebuilds itself at dpkg-buildpackage time against the installed Samba source tree (via `apt-get source samba=<version>`). An appliance CI pipeline should rebuild this deb whenever the base Samba package changes.

## Pool lower filesystems (per-tier backing)

A smoothfs pool is a stack over N tier targets; each target is a lower filesystem.

| Lower | Status | Phase validated | Notes |
|---|---|---|---|
| `xfs` | **Supported** | Phase 1 (bare minimum), Phase 3 functional validation | Primary production choice — reflink, robust under write-amplification, used by Phase 5 SMB + Phase 6 iSCSI harnesses. Requires ≥ 300 MB per tier for mkfs. |
| `ext4` | **Supported** | Phase 3 | Fully functional; slower reflink than XFS. |
| `btrfs` | **Supported** | Phase 3 (+ explicit reflink / subvolume coverage) | Reflink via `FICLONERANGE` tested; snapshot-on-the-lower works. |
| `zfs` | **Supported** | Phase 1 / 2 baseline | Whole-dataset tier target; pool UUID must match. |
| `bcachefs` | Not supported | — | Phase 3 capability gate would accept it if proven; nothing has driven that validation yet. |

Other filesystems (fat / ntfs-3g / overlayfs / fuse / etc.) are not capability-gate-accepted and smoothfs will refuse to mount over them (returns `EOPNOTSUPP` with a `dmesg` line).

## Protocols

| Protocol | Status | Phase |
|---|---|---|
| **NFS v3 / v4.2** | Supported | 4.0–4.5 (cthon04 clean, connectable filehandles) |
| **SMB 2/3** | Supported | 5.0–5.8.4 (smbtorture 16/16 MUST_PASS, Samba VFS module with lease pin + FileId + fanotify lease-break) |
| **iSCSI (file-backed LUN)** | Supported | 6.0–6.5 (O_DIRECT conformance, LIO fileio round-trip, `PIN_LUN` contract, target restart) |
| Active-LUN movement | **Unsupported in v1** | Phase 8 (gated on Phase 6 soak) |

## Disk Spindown Write Staging

| Feature | Status | Notes |
|---|---|---|
| Sysfs status/control | Supported | `/sys/fs/smoothfs/<uuid>/write_staging_supported`, `write_staging_enabled`, `write_staging_full_pct`, `staged_bytes`, `staged_rehome_bytes`, `range_staged_bytes`, `range_staged_writes`, `staged_rehomes_total`, `staged_rehomes_pending`, `write_staging_drainable_rehomes`, `write_staging_drain_pressure`, `write_staging_drainable_tier_mask`, `oldest_staged_write_at`, `last_drain_at`, `last_drain_reason`, `write_staging_drain_active_tier_mask` |
| New writes to new files | Supported | New files land on the fastest tier until that tier reaches `write_staging_full_pct`, then spill to the next tier. |
| Truncate-for-write rehome | Supported | With write staging enabled, an `O_TRUNC` write to a regular file placed on a colder tier is rehomed to the fastest tier before the colder lower file is opened, unless the fastest tier is at the full threshold. smoothfs records the original tier as the staged file's future drain target and increments `staged_rehomes_total`. |
| Truncate-rehome drain cleanup | Supported | When SmoothNAS writes `write_staging_drain_active_tier_mask` with the original tier bit set, smoothfs removes stale original lower files for matching staged truncate rehomes and clears their staged state. This path relies on SmoothNAS only setting non-fast bits after external activity has already woken the tier. |
| Metadata tier activity gate | Supported | SmoothNAS can write `metadata_active_tier_mask`; smoothfs skips inactive tiers for fallback lookup and union readdir, serves cached `getattr` for already-resolved inactive-tier dentries, and reports skipped probes via `metadata_tier_skips`. |
| Range-level staging | Partial | Buffered non-truncating writes to unpinned cold-tier regular files can stage changed ranges on the fastest tier while preserving old-byte reads through range read-merge. Once ranges are staged, direct I/O and mmap are refused so callers cannot bypass the merge layer. Persistence replay remains pending. |
| Range-level drain back to HDD | Partial | When SmoothNAS writes `write_staging_drain_active_tier_mask` with the source tier bit set, smoothfs copies in-memory staged ranges back to that source lower file, fsyncs, clears staged-range state, and removes the fastest-tier sidecar. Remount/crash recovery remains pending, and SmoothNAS must only set non-fast bits after external activity has already woken the tier. |

LUN backing files are auto-pinned with `PIN_LUN` and tierd refuses to move them. Operators who need to move a LUN must quiesce the target, clear the pin manually, move, re-pin. The automated active-LUN path is Phase 8.

## Secure Boot

| Field | Value |
|---|---|
| MOK provisioning | Per-appliance; DKMS auto-generates `/var/lib/dkms/mok.{key,pub}` on first install |
| Enrollment helper | `/usr/share/smoothfs-dkms/enroll-signing-cert.sh` wraps `mokutil --import` |
| Regression gate | `/usr/share/smoothfs-dkms/module_signing.sh` asserts PKCS#7 signature + DKMS signer |
| Production single-key path | Not in v1 — each appliance enrolls its own key; a future "single offline-managed signing key across the fleet" story would let one cert trust modules built on any build host |

## Packaging

| Package | Version | Source |
|---|---|---|
| `smoothfs-dkms` | 0.1.0-1 | `src/smoothfs/debian/` — Phase 7.0 |
| `smoothfs-samba-vfs` | 0.1.0-1 | `src/smoothfs/samba-vfs/debian/` — Phase 7.1; rebuilds against installed Samba |
| `tierd` | 0.1.0-1 | `tierd/debian/` — Phase 7.4; ships `/usr/sbin/tierd` + `/usr/bin/tierd-cli` + systemd unit |

The three debs are independently versioned but expected to move
together in a SmoothNAS release.

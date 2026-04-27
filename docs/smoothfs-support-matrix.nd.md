# smoothfs supportmatrix

Deze pagina beschrijft welke componentversies het smoothfs project testte voor de
SmoothNAS integratie. Alles daarbuiten kan werken, maar valt niet onder support.

Laatste review: tegen Phase 7.10 plus disk-spindown write-staging truncate rehome
en actieve LUN fase-8 movement recovery (Phase 0–8 volledig; phase 8 blijft
rollout-gated buiten gecontroleerde productie-soak).

[English version](smoothfs-support-matrix.md)

## Appliance OS

| Veld | Waarde |
|---|---|
| Base OS | Debian 13 (trixie) |
| Libc | glibc 2.41 |
| Init | systemd ≥ 255 |
| Pakketformaat | `.deb` via apt |

Oudere Debian releases (12 / bookworm) zijn niet supported omdat de kernel floor
(§Kernel) dat uitsluit.

## Kernel

| Veld | Waarde |
|---|---|
| Minimum | **6.18.0 LTS** |
| Getest | 6.18.22-smoothnas-lts, 6.19.10+deb13-amd64 (Debian bpo) |
| DKMS floor | enforced by `BUILD_EXCLUSIVE_KERNEL="^(6\.(1[8-9]|[2-9][0-9])|[7-9]\.).*"` in `dkms.conf` |
| Waarom 6.18 | smoothfs gebruikt `lookup_one`, `set_default_d_op`, `vfs_mkdir` (met dentry return), de nieuwe `renamedata`, `vfs_mmap`, en `parent-inode + qstr d_revalidate` met nieuwe signatures — allemaal stabiel vanaf 6.18 |

Kernelversies onder 6.18 => DKMS slaat smoothfs build stilletjes over voor die kernel
(Phase 7.3 test `kernel_upgrade.sh` bevestigt dit). Appliance start nog wel op, maar
smoothfs pools monden niet.

## OpenZFS

| Veld | Waarde |
|---|---|
| Versie | **2.4.1** |
| Bron | upstream OpenZFS via SmoothKernel recepten (Phase 2.3), **niet** Debian `zfs-dkms` |
| Waarom | Debian `zfs-dkms 2.3.2` eindigt bij kernel 6.14 — niet compatible met 6.18 floor. OpenZFS 2.4.1 is de eerste release die tegen 6.18+ bouwt. |

Het mengen van Debian `zfs-dkms` met smoothfs-dkms op één host is **unsupported** —
de DKMS autoinstall voor zfs faalt stilletjes bij elke kernel upgrade.

## Samba

| Veld | Waarde |
|---|---|
| Versie | **2:4.22.8+dfsg-0+deb13u1** (Debian 13 standaard) |
| Vendor suffix | `Debian-4.22.8+dfsg-0+deb13u1` |
| VFS ABI pin | `smoothfs-samba-vfs` pakket heeft `Depends: samba (= <exact version>)` — apt weigert install bij niet-overeenkomende versies |
| Trigger voor rebuild | iedere Samba security update |

Het `smoothfs-samba-vfs` pakket rebuild automatisch bij dpkg-buildpackage tegen de
geïnstalleerde Samba bronboom (via `apt-get source samba=<version>`). Een
appliance CI pipeline moet deze deb opnieuw bouwen bij iedere basis Samba update.

## Pool lagere filesystems (per-tier backing)

Een smoothfs pool is een stack op N tier targets; elke target is een ondersteunde
lower filesystem.

| Lower | Status | Gevalideerde phase | Opmerkingen |
|---|---|---|---|
| `xfs` | **Supported** | Phase 1 (basis), Phase 3 functionele validatie | Eerste productie-keuze — reflink, robuust onder write-amplificatie, gebruikt door SMB Phase 5 + iSCSI Phase 6 harness. Vereist minimaal 300 MB per tier voor mkfs. |
| `ext4` | **Supported** | Phase 3 | Volledig functioneel; langzamer reflink dan XFS. |
| `btrfs` | **Supported** | Phase 3 (+ expliciete reflink/subvolume dekking) | Reflink via `FICLONERANGE` getest; snapshot-on-the-lower werkt. |
| `zfs` | **Supported** | Phase 1 / 2 baseline | Volledige dataset tier target; pool UUID moet overeenkomen. |
| `bcachefs` | Niet ondersteund | — | Capability gate zou Phase 3 accepteren als bewezen; nog geen validatie afgemaakt. |

Overige filesystems (fat / ntfs-3g / overlayfs / fuse / enz.) worden niet geaccepteerd
door de capability gate; smoothfs weigert mounten daarop met `EOPNOTSUPP`.

## Protocols

| Protocol | Status | Phase |
|---|---|---|
| **NFS v3 / v4.2** | Supported | 4.0–4.5 (cthon04 clean, connectable filehandles) |
| **SMB 2/3** | Supported | 5.0–5.8.4 (smbtorture 16/16 MUST_PASS, Samba VFS met lease pin + FileId + fanotify lease-break) |
| **iSCSI (file-backed LUN)** | Supported | 6.0–6.5 (O_DIRECT conformance, LIO fileio round-trip, `PIN_LUN` contract, target restart) |
| Active-LUN movement | **Ondersteund (opt-in)** | Fase 8: gecontroleerde productie-soak + expliciete operator workflow |

## Disk Spindown Write Staging

| Functie | Status | Opmerkingen |
|---|---|---|
| Sysfs status/control | Supported | `/sys/fs/smoothfs/<uuid>/write_staging_supported`, `write_staging_enabled`, `write_staging_full_pct`, `staged_bytes`, `staged_rehome_bytes`, `range_staged_bytes`, `range_staged_writes`, `staged_rehomes_total`, `staged_rehomes_pending`, `write_staging_drainable_rehomes`, `write_staging_drain_pressure`, `write_staging_drainable_tier_mask`, `oldest_staged_write_at`, `last_drain_at`, `last_drain_reason`, `write_staging_drain_active_tier_mask` |
| Nieuwe writes naar nieuwe files | Supported | Nieuwe bestanden komen op de snelste tier tot de drempel `write_staging_full_pct` bereikt is, daarna naar volgende tier. |
| Truncate-for-write rehome | Supported | Als write staging aan staat, wordt een `O_TRUNC` write op een regulier bestand in koudere tier eerst herplaatst naar de snelste tier vóór opening van die cold lower file, tenzij de snelste tier de volheidsdrempel bereikt heeft. smoothfs registreert oorspronkelijke tier als staged file drain target en verhoogt `staged_rehomes_total`. |
| Truncate-rehome drain cleanup | Supported | Als SmoothNAS `write_staging_drain_active_tier_mask` met de originele tier bit zet, verwijdert smoothfs oude originele lower files voor matching truncate rehome en wist staged state. Deze pad werkt alleen als SmoothNAS alleen niet-fast bits zet wanneer de tier extern al actief is. |
| Metadata tier activity gate | Supported | SmoothNAS kan `metadata_active_tier_mask` schrijven; kernel slaat metadata-only loops op inactieve tiers over. Bit 0 is altijd geforceerd aan op snelste tier. Voor een 2-tier pool bewaart `0x1` readdir/browse op snelste tier en slaat tier 1 over zolang die niet extern gezien is. Als een cold-tier dentry al is resolved, geeft `stat` gecachte smoothfs inode attributen terug in plaats van opnieuw vanaf die inactieve tier te lezen. |
| Range-level staging | Partial | Buffered niet-truncating writes naar niet-gepinde cold-tier reguliere files kunnen gewijzigde ranges op de snelste tier stage; oude bytes worden later correct via range read-merge gelezen. Zodra ranges staged zijn, wordt direct I/O en mmap geweigerd zodat bypass niet meer mogelijk is. Persistente replay is nog pending. |
| Range-level drain terug naar HDD | Partial | Als SmoothNAS `write_staging_drain_active_tier_mask` met bronbit zet, kopieert smoothfs in-memory staged ranges terug naar die bron-lower file, fsynct en ruimt staged-range state en snelle sidecar op. Remount/crash recovery voor range-level data blijft pending; SmoothNAS moet bits alleen zetten na externe activiteit. |

LUN backing files worden automatisch gepint met `PIN_LUN` en de normale planner
beweging slaat ze over.
Fase 8 introduceert een gecontroleerde active-LUN flow:

- stop/drain de iSCSI target zodat de backing file quiesced is,
- verwijder `PIN_LUN` en bereid een movement plan voor terwijl de object-rij nog
  `pin_lun` bevat,
- verplaats naar de target tier (`MovePlan` + `MoveCutover`),
- pin de destination backing file opnieuw met `PIN_LUN`,
- hervat de target pas nadat de destination pin validatie slaagt.

Als een movement onderbroken raakt, herstelt startup recovery de pin-state
consistentie door `PIN_LUN` opnieuw toe te passen op het uiteindelijke tier pad
voor in-flight rijen.

## Secure Boot

| Veld | Waarde |
|---|---|
| MOK provisioning | Per appliance; DKMS maakt automatisch `/var/lib/dkms/mok.{key,pub}` bij eerste install |
| Enrollment helper | `/usr/share/smoothfs-dkms/enroll-signing-cert.sh` met `mokutil --import` |
| Regression gate | `/usr/share/smoothfs-dkms/module_signing.sh` controleert PKCS#7 signature + DKMS signer |
| Productie single-key pad | Niet in v1 — elke appliance draait eigen key. Een toekomstig "single offline key" verhaal kan modules van elke build host vertrouwen. |

## Packaging

| Pakket | Versie | Bron |
|---|---|---|
| `smoothfs-dkms` | 0.1.0-1 | `src/smoothfs/debian/` — Phase 7.0 |
| `smoothfs-samba-vfs` | 0.1.0-1 | `src/smoothfs/samba-vfs/debian/` — Phase 7.1; rebuilt tegen geïnstalleerde Samba |
| `tierd` | 0.1.0-1 | `tierd/debian/` — Phase 7.4; bevat `/usr/sbin/tierd` + `/usr/bin/tierd-cli` + systemd unit |

De drie debs zijn onafhankelijk versioned maar worden samen verwacht te bewegen in een
SmoothNAS release.

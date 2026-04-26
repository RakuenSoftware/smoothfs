# smoothfs operator runbook

Day-to-day procedures for running smoothfs in the SmoothNAS appliance
integration. Covers install, pool lifecycle, share creation, routine
maintenance, kernel upgrades, and troubleshooting. Pair with
`smoothfs-support-matrix.md` for the tested version matrix.

## 1. First-time install

On a freshly-provisioned Debian 13 appliance:

```bash
# 1. Install the three debs. Order matters — tierd Recommends (not
#    Depends) the other two, so apt won't pull them automatically.
apt install ./smoothfs-dkms_0.1.0-1_all.deb
apt install ./smoothfs-samba-vfs_0.1.0-1_amd64.deb
apt install ./tierd_0.1.0-1_amd64.deb

# 2. Confirm the kernel module built + loaded.
bash /usr/share/smoothfs-dkms/kernel_upgrade.sh
bash /usr/share/smoothfs-dkms/module_signing.sh

# 3. On secure-boot hosts, enrol the DKMS MOK cert. This requires
#    a reboot + shim prompt — schedule accordingly.
bash /usr/share/smoothfs-dkms/enroll-signing-cert.sh
# (reboot; confirm at the shim MOK-management UI with the password
#  you entered during --import)

# 4. Confirm tierd is serving.
systemctl status tierd
curl -s http://127.0.0.1:8420/api/health
```

At this point the appliance has zero smoothfs pools. Log into the web UI (default 8420) or use `tierd-cli` to create one.

## 2. Creating a smoothfs pool

Smoothfs pools are operator-declared — tierd does **not** auto-stand-up pools from disk. Each pool needs a name, a UUID (auto-generated if not supplied), and an ordered list of tier mountpoints (fastest first).

### Via the web UI

Storage → smoothfs Pools → **Create Pool**. Fill in:

- **Name** — lowercase alnum + `._-`, ≤ 63 chars.
- **UUID** — leave blank to auto-generate.
- **Tier paths** — newline- or colon-separated. Must be existing directories. Fastest first.

tierd writes a systemd mount unit at `/etc/systemd/system/mnt-smoothfs-<name>.mount`, enables + starts it. Phase 2.5 auto-discovery wires the planner.

### Via CLI

```bash
tierd-cli smoothfs create-pool \
    --name tank \
    --tiers /mnt/nvme-fast:/mnt/sas-slow
```

The CLI drives the library directly, not the REST. CLI-created pools show up in the REST list only after tierd's mount-event auto-discovery kicks in (typically < 1 s after the mount succeeds).

### What you should see

```bash
mount | grep smoothfs                        # smoothfs pool mounted
ls -la /etc/systemd/system/mnt-smoothfs-*.mount  # unit file written
systemctl status mnt-smoothfs-tank.mount     # active (mounted)
curl -s http://127.0.0.1:8420/api/smoothfs/pools  # persisted row
```

## 3. Creating shares on a pool

Once the pool is mounted at `/mnt/smoothfs/<name>`, it's a normal POSIX path. Create shares using the existing Sharing flows:

- **NFS / SMB** — Sharing → Add Share, point the path at `/mnt/smoothfs/<name>/<subdir>`.
- **iSCSI (file-backed LUN)** — Sharing → iSCSI → Add Target → **File-backed**. Set Backing File to the absolute path of an existing sized file under `/mnt/smoothfs/<name>/`. The file is auto-pinned with `PIN_LUN` the moment tierd calls LIO.

For CLI:

```bash
# Create a sized backing file first.
truncate -s 256G /mnt/smoothfs/tank/luns/web-app.img

# Then create the LIO target.
tierd-cli iscsi create-fileio \
    --iqn iqn.2026-04.com.smoothnas:web-app \
    --file /mnt/smoothfs/tank/luns/web-app.img
```

`getfattr -n trusted.smoothfs.lun /mnt/smoothfs/tank/luns/web-app.img` should return `0x01`.

## 4. Routine maintenance

### Quiesce (pause movement)

Stops in-flight cutovers + refuses new `MOVE_PLAN`s. Use before any manual intervention on a pool (manual `cp` between tiers, manual `setfattr`, etc.). Phase 2 makes quiesce safe on a live pool — readers + writers keep working, just no movement.

```bash
# UI: per-pool Quiesce button.
# CLI:
tierd-cli smoothfs quiesce --pool <uuid>
```

### Reconcile (resume movement)

Lifts the quiesce + re-arms heat drain.

```bash
# UI: per-pool Reconcile button (prompts for reason — recorded in movement log).
# CLI:
tierd-cli smoothfs reconcile --pool <uuid> --reason "manual inspection complete"
```

### Movement log

Storage → smoothfs Pools → Movement log (below the pool list). Renders newest 100 transitions from `smoothfs_movement_log` across all pools. Each row shows the state transition, object_id, and source/dest tier. Use this to confirm quiesce stopped planner activity and reconcile resumed it.

Direct SQLite query (useful for scripting):

```bash
sqlite3 /var/lib/tierd/tierd.db \
    'SELECT written_at, to_state, source_tier, dest_tier FROM smoothfs_movement_log ORDER BY id DESC LIMIT 50;'
```

### Write staging

SmoothNAS controls write staging from the smoothfs Pools page. The kernel-side
switch and counters are also visible under `/sys/fs/smoothfs/<pool-uuid>/`:

```bash
cat /sys/fs/smoothfs/<uuid>/write_staging_supported
cat /sys/fs/smoothfs/<uuid>/write_staging_enabled
cat /sys/fs/smoothfs/<uuid>/write_staging_full_pct
cat /sys/fs/smoothfs/<uuid>/staged_bytes
cat /sys/fs/smoothfs/<uuid>/staged_rehomes_total
cat /sys/fs/smoothfs/<uuid>/write_staging_drainable_rehomes
cat /sys/fs/smoothfs/<uuid>/write_staging_drain_pressure
cat /sys/fs/smoothfs/<uuid>/write_staging_drainable_tier_mask
cat /sys/fs/smoothfs/<uuid>/metadata_active_tier_mask
cat /sys/fs/smoothfs/<uuid>/write_staging_drain_active_tier_mask
cat /sys/fs/smoothfs/<uuid>/metadata_tier_skips
```

The first data-plane path handles replace-style writes: when staging is enabled,
an `O_TRUNC` write to a regular file currently placed on a colder tier is
rehomed onto the fastest tier before the lower file is opened, provided the
fastest tier is below `write_staging_full_pct` (default 98). New files already
follow the same admission rule: they land on the fastest tier until that tier
reaches the full threshold, then spill to the next tier. Range-level staging for
non-truncating writes and draining back to HDD are follow-up work.
`staged_rehomes_total` counts these truncate-write rehomes.
`write_staging_drainable_rehomes` counts staged truncate rehomes whose original
tier is currently permitted by `write_staging_drain_active_tier_mask`.
`write_staging_drain_pressure` flips to `1` only when staged work exists and
the fastest tier is at the configured full threshold.
`write_staging_drainable_tier_mask` reports non-fast tiers that currently have
both staged work and SmoothNAS drain permission. It is a status signal only;
reading it never starts a drain.

SmoothNAS can also write `metadata_active_tier_mask` to suppress metadata-only
walks of standby tiers. Bit `0` is the fastest tier and is always forced on.
For a two-tier pool, writing `0x1` keeps browse/readdir fallback on the fastest
tier and skips tier 1 until SmoothNAS observes it externally active. If a
cold-tier dentry is already resolved, `stat` returns smoothfs's cached inode
attributes instead of refreshing from an inactive lower tier.

The separate `write_staging_drain_active_tier_mask` is the data-drain gate.
SmoothNAS should write a bit only after it has observed that tier's backing
devices active due to external activity. The fastest-tier bit is always forced
on by the kernel. When a colder tier becomes drain-active, smoothfs also drains
truncate-rehome staging records for that tier by removing the stale original
lower file and clearing the per-inode staged state. Future range-level staged
data drain work uses this mask rather than the metadata browse mask so directory
visibility and data-drain permission can move independently.

### Destroying a pool

Stops + removes the systemd mount unit. Any share pointing at a file on this pool will return `EIO` until the pool is re-created.

```bash
# UI: per-pool Destroy button.
# CLI:
tierd-cli smoothfs destroy-pool --name tank
```

The tier lower directories are untouched — `destroy-pool` only removes the smoothfs overlay. Re-creating with the same name + UUID + tiers resurrects the pool with all its data.

## 5. Kernel upgrades

`apt upgrade` pulls a new `linux-headers-*` package. DKMS's autoinstall hooks build smoothfs for the new kernel — no manual step required — as long as the new kernel is `≥ 6.18` (see `BUILD_EXCLUSIVE_KERNEL` in the support matrix).

After the upgrade completes, run the kernel-upgrade harness:

```bash
bash /usr/share/smoothfs-dkms/kernel_upgrade.sh
```

The harness confirms every installed kernel has either a signed smoothfs module at `/lib/modules/<kver>/updates/dkms/smoothfs.ko.xz` or a clean "out of BUILD_EXCLUSIVE_KERNEL" skip — no half-built or failed state. If a kernel built but didn't sign, `module_signing.sh` will catch it.

### Rollback

Per-kernel DKMS trees mean a failed build on kernel B never disturbs kernel A's working `.ko`. If the newly-installed kernel fails to boot or smoothfs fails to load, pick the previous kernel in GRUB — its module is still there. Once booted, `apt remove` the bad linux-headers package to keep DKMS from retrying the rebuild every upgrade.

## 6. Samba upgrades

Because `smoothfs-samba-vfs` pins `Depends: samba (= <exact version>)`, apt will **refuse** to upgrade Samba without a matching VFS deb. Rebuild the VFS module against the new Samba version:

```bash
# On the build host (CI, not the appliance):
apt-get source samba=<new-version>
cd /path/to/smoothfs/samba-vfs
dpkg-buildpackage -us -uc -b

# On the appliance:
apt install ./smoothfs-samba-vfs_0.1.0-1_amd64.deb
systemctl reload smbd    # picks up the new .so for new connections
```

## 7. Troubleshooting

### "smoothfs: active mounts present; leaving running module in place"

The `smoothfs-dkms` prerm printed this during `apt upgrade`. Expected — the package left the running module alone because you still have mounts. The new `.ko.xz` is on disk for the next reboot. To activate it immediately, destroy every smoothfs pool + `modprobe -r smoothfs && modprobe smoothfs`.

### `mount -t smoothfs ...` returns `-EOPNOTSUPP`

Lower filesystem doesn't pass the capability gate. Check `dmesg`:

```
smoothfs: tier /mnt/foo has s_magic 0xXXXX; only xfs, ext4, btrfs, zfs are supported
```

Mount the tier on a supported filesystem and try again.

### Samba VFS module fails to load with "version SAMBA_X.Y.Z_PRIVATE_SAMBA not found"

Samba was upgraded without rebuilding the VFS deb. apt should have prevented this (see §6). If you hit it anyway, apt-pin samba to the version matching your installed `smoothfs-samba-vfs`, or rebuild the VFS.

### `/var/lib/dkms/mok.*` missing under secure boot

Occurs on fresh installs if DKMS's framework.conf wasn't asked to autogenerate. Manually generate:

```bash
dkms generate_mok
bash /usr/share/smoothfs-dkms/enroll-signing-cert.sh
# reboot + shim prompt
```

### Movement wedged — quiesce doesn't return

Check `smoothfs_movement_log` for the last rows. If there's a row stuck at `cutover_in_progress` without a `switched`/`failed` successor, the kernel is likely in the SRCU drain waiting for a writer. `ss -tnlp | grep :<target-port>` to find the holding process; kill it if appropriate. As a last resort, `modprobe -r smoothfs` after every pool is unmounted forces recovery on the next mount.

### `Inspect` returns `ENOENT` / UI shows "pool not found"

Mount unit isn't active. `systemctl start mnt-smoothfs-<name>.mount` and check `journalctl -u mnt-smoothfs-<name>.mount` for the mount error.

### Disaster recovery: tierd.db corruption

The SmoothNAS sqlite db at `/var/lib/tierd/tierd.db` is a MIRROR of state that primarily lives elsewhere (systemd units on disk, kernel mounts, LIO saveconfig.json). Losing it means losing the REST view but **not** the pools themselves.

To recover:

```bash
systemctl stop tierd
mv /var/lib/tierd/tierd.db{,.broken}
systemctl start tierd
# tierd runs its goose migrations on an empty db.
# Phase 2.5 auto-discovery repopulates smoothfs_objects from the
# currently-mounted pools. iSCSI rows need to be re-imported
# manually (or re-created via the REST API) since LIO's
# saveconfig.json is the source of truth for targets.
```

No data on the tier lowers is at risk during a tierd.db recovery — only tierd's view of it is.

## 8. When to call support

Escalate anything that matches:

- Data-integrity-sounding errors in `dmesg`: `smoothfs: cutover lost lower_path`, `smoothfs: placement log corrupted`, `smoothfs: oid mismatch on ...`
- A pool that refuses to mount on a kernel that previously worked.
- A smoothfs module that loads but `modprobe smoothfs` then panics the kernel on first mount.
- `smbtorture` MUST_PASS set regressing (used to be 16/16).
- Any movement transition that reaches `failed` and won't clear on reconcile.

Gather before escalating:

```bash
tar czf /tmp/smoothfs-diag.tar.gz \
    /var/log/syslog* \
    /var/lib/tierd/tierd.db \
    /var/lib/dkms/smoothfs \
    /etc/systemd/system/mnt-smoothfs-*.mount \
    /etc/dkms/framework.conf.d/ \
    <(dmesg --time-format=iso) \
    <(uname -a) \
    <(modinfo smoothfs) \
    <(dkms status)
```

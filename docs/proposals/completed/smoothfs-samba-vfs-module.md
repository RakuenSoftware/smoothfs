# Samba VFS module for smoothfs

**Status:** Complete. Archived after confirming the repository contains the
module source, build script, Debian package metadata, and regression harness.

Phase 5.8.0 stood up the Samba source/build env; Phase 5.8.1 landed the transparent-passthrough skeleton (`src/smoothfs/samba-vfs/vfs_smoothfs.c` + `src/smoothfs/samba-vfs/build.sh`) and the regression harness (`src/smoothfs/test/smb_vfs_module.sh`). Phase 5.8.2 landed the `linux_setlease_fn` override: when Samba installs a kernel oplock (`kernel oplocks = yes`), the hook toggles `trusted.smoothfs.lease` on the fsp's lower fd to drive `SMOOTHFS_PIN_LEASE`. Phase 5.8.3 landed the tevent-integrated fanotify watcher in `connect_fn`: the module opens a `FAN_CLASS_NOTIF`-class fanotify fd against the share mount, adds it to smbd's tevent loop, and — for any `FAN_MODIFY` event from a pid other than this smbd child — walks the connection's fsps and posts `MSG_SMB_KERNEL_BREAK` on any that hold an oplock or SMB lease on the affected file_id. Self-pid filtering is load-bearing: without it every client write would break its own oplock. Phase 5.8.4 landed `file_id_create_fn` reading `trusted.smoothfs.fileid` for a stable SMB FileId: `fstat_fn` caches `(inode_no -> gen)` per-connection from the 12-byte xattr, `file_id_create_fn` looks up `sbuf->st_ex_ino` against that cache and copies the cached `gen` into the `extid` half of the `struct file_id`. On today's smoothfs kernel `si->gen` stays at 0 for every file (gen bumps land with a later kernel phase's oid-reuse work), so `extid=0` and the wire-observable FileId matches stock; the hook is there for when the kernel starts bumping. The module probes `trusted.smoothfs.fileid` at connect time so shares whose lower is not smoothfs pass straight through (no fanotify setup, no xattr work, no FileId cache). The kernel half (fanotify on forced cutover, `SMOOTHFS_PIN_LEASE`, `trusted.smoothfs.fileid` / `.lease` xattrs) already shipped in Phase 5.0 + 5.3. With 5.8.3 landed, the reference `src/smoothfs/test/lease_break_agent.c` is no longer required when the VFS module is loaded — the module subsumes its fanotify-listen + xattr-clear role and adds the SMB-side oplock break the agent never did.

## Why deferred

Phase 5.3 was originally scoped to cover both the kernel-side lease-break signal (forced MOVE_PLAN + fsnotify on cutover) *and* the Samba VFS module that consumes it. The kernel half landed; the Samba VFS module did not, because the Debian `samba-dev` package (Samba 4.22 on our LTS target) does **not** ship the internal `source3/include/*` headers that every example VFS module (`skel_transparent.c`, `skel_opaque.c`, `shadow_copy_test.c`) starts with:

```c
#include "../source3/include/includes.h"
```

`samba-dev` ships only the public API surface (NDR, credentials, talloc, tevent, ldb). Building a VFS module against Samba 4.x requires the full source tree and its waf build. The session in which Phase 5.3 landed did not have that environment provisioned; rather than stand it up and then land a blob of untested VFS code at the same time, Phase 5.3 closed the kernel contract and shipped `src/smoothfs/test/lease_break_agent.c` as a **reference implementation** of exactly what the VFS module will do. The module's job is to plug that same logic into Samba's lifecycle callbacks.

## What the module needs to do

Per the Phase 0 contract's §SMB table, three responsibilities:

| Invariant | Kernel provides | Module does |
|---|---|---|
| Stable file identity (SMB FileId) | `trusted.smoothfs.fileid` (12 B: `inode_no ∥ gen`) — Phase 5.0 | Read the xattr in the FILEID-producing callbacks (`SMB_VFS_FILE_ID_CREATE` or the current 4.22 equivalent), hand it to Samba as the `DEV/INODE/EXTID` triple the SMB2 spec requires |
| Lease/oplock under movement | `SMOOTHFS_PIN_LEASE` via `trusted.smoothfs.lease` — Phase 5.0 | On SMB lease grant (SMB_VFS_SET_LEASE or equivalent for SMB3 leases), `setxattr trusted.smoothfs.lease=1`. On lease break / share close, `removexattr`. No lower-FS round-trip; the kernel handles the pin entirely in memory. |
| Lease-break on forced movement | `fsnotify(FS_MODIFY)` on forced cutover — Phase 5.3 | Run a fanotify watcher (or use Samba's existing `notify_fam`/`notify_inotify` infrastructure) on the share's path. When an event arrives for a file the module currently has a lease on, break the SMB lease to the client, drop the xattr (kernel already cleared the pin — this is hygiene), then let the SMB client's subsequent request reacquire. |
| Case-insensitive name matching | nothing (smoothfs is case-preserving) | Implement via `SMB_VFS_GET_REAL_FILENAME_AT` the same way stock `vfs_catia` / `vfs_case_sensitive` modules do. Not smoothfs-specific; could even be satisfied by chaining `vfs_catia` ahead of the smoothfs module in `smb.conf`. |
| SMB FileNotifyChange | fsnotify events fire naturally from the VFS layer (create/unlink/rename/write) | Wire Samba's `notify_subsys = inotify` (default) against the smoothfs mount. No module code required — this is shared state between the module and stock Samba. |

So three of the five live in the module, and of those, the lease pin + lease-break are genuinely smoothfs-specific. FileId is a short xattr read. That's the real work.

## Scope for the future session

### 1. Build env

One-time setup on the test VM:

```
apt-get build-dep samba
apt-get source samba
cd samba-4.22.8+dfsg
./configure.developer --without-systemd --without-ad-dc
make bin/default/source3/modules/libvfs_module_*.so
```

The `build-dep` pulls in ~200 MB of `-dev` packages; `make` against a warm tree takes ~6 min on an 8-core VM. The only output we care about is `bin/default/source3/modules/libvfs_module_smoothfs.so`, but the build needs the surrounding scaffolding to exist.

Ship as a Debian package `smoothfs-samba-vfs` that drops the `.so` into `/usr/lib/x86_64-linux-gnu/samba/vfs/smoothfs.so` and updates the sample `smb.conf` snippet in `tierd/internal/smb/smb.go`.

### 2. Module skeleton

Start from `skel_transparent.c`. Override:

- `connect_fn` — register the module, set up the per-share fanotify watcher. Tevent-integrate it so the normal smbd event loop pumps it.
- `disconnect_fn` — tear down the watcher.
- `set_lease_fn` (SMB2/3 leases) and the SMB1 `oplock_request_fn` — toggle `trusted.smoothfs.lease`.
- `file_id_create_fn` — compose the SMB FileId from `trusted.smoothfs.fileid`. Fall back to stat-based ID if the xattr is missing (non-smoothfs lower or FS without xattrs).
- Optional: `get_real_filename_at_fn` for case-insensitive matching if we don't chain `vfs_catia`.

Every other callback should passthrough to `SMB_VFS_NEXT_*`. The module must be a *transparent* add-on.

### 3. Fanotify integration

Samba's existing `notify_inotify.c` is the shape to follow — a tevent-fd-added file descriptor that delivers events to the master smbd. The smoothfs module needs its own fanotify fd (because the Phase 0 contract says fanotify, not inotify, and because fanotify gives us `FAN_MARK_MOUNT` scope for free). The handler is the same `removexattr + send_lease_break` the reference agent demonstrates.

### 4. Test

Extend `src/smoothfs/test/smb_roundtrip.sh` (Phase 5.1) with a `[smoothfs]` `vfs objects = smoothfs` line once the module is available. The Phase 5.3 Go test `TestE2ESMBForcedMoveBreaksLease` should keep passing with the real VFS module replacing `lease_break_agent` — this is the regression guard.

Add `smbtorture` as the Phase 5.4 gate per the original stacked-tiering plan; the VFS module + kernel contract together are what that suite exercises.

## Out of scope

- **Samba-AD-DC integration.** smoothfs doesn't change AD semantics; the standalone-server config Phase 5.1 uses is fine.
- **Windows-side testing.** Loopback CIFS from Linux + `smbclient` is what 5.1/5.2/5.3 use; a real Windows client is nice-to-have for 5.4.
- **Performance tuning.** The Phase 3 non-functional targets were set against FUSE and NFS, not SMB; SMB numbers become meaningful only once the VFS module is in the hot path.

## Why we split this out

Running Samba's full source build is a proper side quest: it changes what tools are needed on the test VM, how packaging works, and what the CI matrix looks like. Doing that *and* writing a correct VFS module *and* landing the kernel forced-move signal in one change would have over-committed a single session. Splitting at this joint gives a bisectable kernel-side commit (Phase 5.3) and leaves the module as a self-contained next deliverable against a contract that already has a reference implementation (`lease_break_agent.c`) and an e2e gate (`TestE2ESMBForcedMoveBreaksLease`) the module has to satisfy.

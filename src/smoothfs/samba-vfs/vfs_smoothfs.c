/*
 * Samba VFS module: smoothfs
 *
 * Transparent passthrough with smoothfs-aware overrides — this is the
 * Phase 5.8+ Samba integration scoped in
 * docs/proposals/pending/smoothfs-samba-vfs-module.md. Loading this
 * module must not change any observable behaviour of a share over a
 * non-smoothfs lower; every smbtorture MUST_PASS test that passes
 * today without the module must still pass with
 * `vfs objects = smoothfs`.
 *
 * Functionality lands incrementally in Phase 5.8.x sub-phases:
 *   5.8.1  — loads, registers, logs, no behavioural override. Proves
 *            the build pipeline and regression-safe load path.
 *   5.8.2  — linux_setlease_fn wired to trusted.smoothfs.lease (the
 *            lease-pin half of Phase 5.0's kernel contract). When
 *            Samba installs a kernel lease on an fsp (F_RDLCK/F_WRLCK),
 *            we toggle the smoothfs lease xattr so the kernel refuses
 *            non-forced movement of the file while the SMB lease is
 *            held. F_UNLCK clears the xattr.
 *   5.8.3  — fanotify watcher in connect_fn + lease-break forwarding,
 *            replacing the reference lease_break_agent from Phase 5.3.
 *            Per Phase 0 §SMB the module watches the share's mount for
 *            FAN_MODIFY, finds any files_struct currently holding a
 *            kernel lease on the affected file_id, and sends the same
 *            MSG_SMB_KERNEL_BREAK the SIGIO path would send so the
 *            client sees a clean oplock break before the new tier's
 *            bytes become visible. trusted.smoothfs.lease is cleared
 *            as hygiene — the kernel already set pin_state=PIN_NONE
 *            on cutover before firing fsnotify, this just syncs the
 *            xattr view.
 *   5.8.4  — file_id_create_fn reading trusted.smoothfs.fileid for a
 *            stable SMB FileId. The xattr is a 12-byte blob:
 *            inode_no (u64 LE) | gen (u32 LE). The inode_no matches
 *            the stock sbuf->st_ex_ino smoothfs already reports, so
 *            the only new signal is gen — which becomes the SMB FileId
 *            extid and distinguishes object_id reuse. Since
 *            file_id_create_fn receives only sbuf (no fd, no path),
 *            gen is cached at fstat_fn time in a per-connection
 *            linked list keyed on inode_no and read back from there.
 *            (fstat is the earliest reliable hook — openat's fsp is
 *            sometimes O_PATH and fgetxattr refuses that with EBADF.)
 *
 * Copyright (C) SmoothNAS / RakuenSoftware
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "includes.h"
#include "smbd/smbd.h"
#include "smbd/globals.h"
#include "smbd/proto.h"
#include "lib/util/tevent_unix.h"
#include "lib/util/tevent_ntstatus.h"

#include <endian.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_VFS

#define SMOOTHFS_LEASE_XATTR    "trusted.smoothfs.lease"
#define SMOOTHFS_FILEID_XATTR   "trusted.smoothfs.fileid"
#define SMOOTHFS_FILEID_XATTR_LEN 12

/*
 * Per-connection cache of (inode_no -> gen) for the SMB FileId extid.
 * Populated on every successful openat_fn against a smoothfs lower;
 * read back in file_id_create_fn which is handed only an SMB_STRUCT_STAT
 * with no fd or path, so can't do its own xattr read. A simple
 * linked list is fine — smbd connections rarely have more than a few
 * hundred live fsps, and the lookup is per SMB op, not per byte.
 *
 * Entries are talloc children of the connect_data and die with the
 * connection. Stale entries (file deleted since) stay cached but are
 * harmless: their inode_no won't match any live sbuf, so the lookup
 * walks past them. Memory use is O(distinct inodes opened per
 * connection) * ~40 bytes.
 */
struct smoothfs_fileid_entry {
	uint64_t ino;
	uint32_t gen;
	struct smoothfs_fileid_entry *next;
};

struct smoothfs_connect_data {
	bool is_smoothfs;
	bool lease_watcher;
	bool stable_fileid;

	/*
	 * fanotify watcher on the share's mount. FAN_MODIFY fires when
	 * smoothfs's kernel module does a forced MOVE_CUTOVER on a
	 * PIN_LEASE file (movement.c :: smoothfs_movement_cutover).
	 * That's the only event source we care about — the same
	 * fsnotify also fires on regular writes, and our handler is
	 * no-op-for-the-common-case (no fsp with a lease → nothing to
	 * break, no xattr to drop), so the extra wake-ups are cheap.
	 *
	 * Set up in smoothfs_setup_fanotify() during connect, torn down
	 * by smoothfs_connect_data_destructor() on disconnect. If setup
	 * fails (missing CAP_SYS_ADMIN, kernel too old for FAN_MARK_MOUNT,
	 * etc.), fan_fd stays -1 and the share still works — clients
	 * just don't get the clean lease break on forced cutover.
	 */
	int fan_fd;
	struct tevent_fd *fan_fde;
	vfs_handle_struct *handle; /* back-pointer for the event handler */

	/* Phase 5.8.4: (ino -> gen) cache. */
	struct smoothfs_fileid_entry *fileid_head;
};

/*
 * Probe the share root for trusted.smoothfs.fileid. Its presence is
 * the sentinel for a smoothfs lower — non-smoothfs filesystems don't
 * synthesise the xattr. The probe goes through libc's getxattr(2)
 * directly rather than SMB_VFS_NEXT_FGETXATTR because at connect_fn
 * time conn->cwd_fsp is synthetic (fd = -1) so the fd-based VFS
 * helpers can't run. The share path is already resolved and
 * accessible at this point, so the direct path-based call is safe
 * and has no dependency on the rest of the VFS chain being wired up.
 */
static bool smoothfs_probe_lower(vfs_handle_struct *handle)
{
	const struct loadparm_substitution *lp_sub =
		loadparm_s3_global_substitution();
	const char *path;
	uint8_t buf[12];
	ssize_t n;

	path = lp_path(talloc_tos(), lp_sub, SNUM(handle->conn));
	if (path == NULL || path[0] == '\0') {
		return false;
	}
	n = getxattr(path, SMOOTHFS_FILEID_XATTR, buf, sizeof(buf));
	if (n >= 0) {
		return true;
	}
	if (errno != ENODATA && errno != ENOATTR && errno != EOPNOTSUPP &&
	    errno != ENOENT && errno != EACCES) {
		DBG_NOTICE("smoothfs: fileid probe on %s failed (%s); "
			   "assuming non-smoothfs lower\n",
			   path, strerror(errno));
	}
	return false;
}

/*
 * Handle one fanotify event:
 *   1. Drop events caused by our own smbd pid — client writes relayed
 *      through smbd fire FAN_MODIFY too, and if we broke our own
 *      oplock on every SMB write we'd nuke the performance benefit of
 *      leases and loop-break the lease pin off immediately after
 *      linux_setlease installed it. The Phase 5.3 forced-cutover
 *      signal originates from smoothfs kernel code invoked via tierd
 *      netlink, whose pid is not us.
 *   2. fstat the event fd to get the smoothfs inode's file_id.
 *   3. Walk fsps for this sconn; for each fsp with a kernel oplock or
 *      SMB lease on that file_id, post MSG_SMB_KERNEL_BREAK — same
 *      message the SIGIO handler posts when the kernel breaks a
 *      lease for a foreign writer. Samba's oplock dispatcher picks
 *      it up and sends the SMB-level break PDU to the client.
 *   4. removexattr trusted.smoothfs.lease (hygiene — pin_state is
 *      already PIN_NONE, but keep the xattr view consistent).
 *
 * Caller must close ev->fd after this returns.
 */
static void smoothfs_fanotify_event(struct smoothfs_connect_data *d,
				    const struct fanotify_event_metadata *ev)
{
	struct smbd_server_connection *sconn = d->handle->conn->sconn;
	SMB_STRUCT_STAT sbuf;
	struct file_id fid;
	struct files_struct *fsp;
	char link_path[64];
	char real_path[PATH_MAX];
	ssize_t plen;

	if (ev->fd < 0) {
		return;
	}
	if (ev->pid == getpid()) {
		/*
		 * Self-filter. fanotify delivers FAN_MODIFY for every
		 * write, including the ones this smbd child relayed
		 * from its SMB client. Processing those would break
		 * our own just-granted oplock back to the client —
		 * catastrophic for cache hit rate and circular with
		 * linux_setlease. Forced-cutover fsnotify comes from a
		 * separate netlink-caller pid (tierd or tierd-cli) so
		 * this filter is safe.
		 */
		return;
	}
	if (sys_fstat(ev->fd, &sbuf, false) != 0) {
		DBG_DEBUG("smoothfs: fstat on fanotify fd=%d: %s\n",
			  ev->fd, strerror(errno));
		return;
	}
	if (!S_ISREG(sbuf.st_ex_mode)) {
		/*
		 * Directories / symlinks fire FS_MODIFY on internal
		 * changes we don't care about. Phase 5.3 contract is
		 * for regular files.
		 */
		return;
	}

	fid = vfs_file_id_from_sbuf(d->handle->conn, &sbuf);

	/*
	 * Unconditional notice even when no matching fsp is found — for
	 * observability and for tests to prove the watcher fired. In the
	 * real forced-cutover path the interesting work only happens when
	 * there's an fsp with a live oplock, but on a shared smoothfs
	 * mount any foreign write delivers an event here and the log line
	 * is cheap.
	 */
	DBG_NOTICE("smoothfs: fanotify event: pid=%d inode=%llu dev=%llu\n",
		   (int)ev->pid,
		   (unsigned long long)sbuf.st_ex_ino,
		   (unsigned long long)sbuf.st_ex_dev);

	fsp = file_find_di_first(sconn, fid, false);
	while (fsp != NULL) {
		struct files_struct *next = file_find_di_next(fsp, false);

		if (fsp->oplock_type != NO_OPLOCK || fsp->lease != NULL) {
			DBG_NOTICE("smoothfs: posting kernel-oplock break on %s\n",
				   fsp_str_dbg(fsp));
			break_kernel_oplock(sconn->msg_ctx, fsp);
		}
		fsp = next;
	}

	/*
	 * Path-based removexattr via /proc/self/fd/<fd>. trusted.*
	 * requires CAP_SYS_ADMIN, which smbd has.
	 */
	snprintf(link_path, sizeof(link_path), "/proc/self/fd/%d", ev->fd);
	plen = readlink(link_path, real_path, sizeof(real_path) - 1);
	if (plen <= 0) {
		return;
	}
	real_path[plen] = '\0';
	if (removexattr(real_path, SMOOTHFS_LEASE_XATTR) < 0 &&
	    errno != ENODATA && errno != ENOATTR) {
		DBG_INFO("smoothfs: removexattr(lease) on %s: %s\n",
			 real_path, strerror(errno));
	}
}

static void smoothfs_fanotify_handler(struct tevent_context *ev,
				      struct tevent_fd *fde,
				      uint16_t flags, void *private_data)
{
	struct smoothfs_connect_data *d = private_data;
	char buf[4096];
	ssize_t got;
	struct fanotify_event_metadata *fev;

	got = read(d->fan_fd, buf, sizeof(buf));
	if (got < 0) {
		if (errno == EAGAIN || errno == EINTR) {
			return;
		}
		DBG_WARNING("smoothfs: fanotify read failed: %s\n",
			    strerror(errno));
		return;
	}
	fev = (struct fanotify_event_metadata *)buf;
	while (FAN_EVENT_OK(fev, got)) {
		smoothfs_fanotify_event(d, fev);
		if (fev->fd >= 0) {
			close(fev->fd);
		}
		fev = FAN_EVENT_NEXT(fev, got);
	}
}

static int smoothfs_connect_data_destructor(struct smoothfs_connect_data *d)
{
	if (d->fan_fde != NULL) {
		TALLOC_FREE(d->fan_fde);
	}
	if (d->fan_fd >= 0) {
		close(d->fan_fd);
		d->fan_fd = -1;
	}
	return 0;
}

static void smoothfs_setup_fanotify(vfs_handle_struct *handle,
				    struct smoothfs_connect_data *d)
{
	const struct loadparm_substitution *lp_sub =
		loadparm_s3_global_substitution();
	const char *path;
	int flags;

	d->fan_fd = -1;
	d->fan_fde = NULL;

	if (!d->is_smoothfs) {
		return;
	}

	path = lp_path(talloc_tos(), lp_sub, SNUM(handle->conn));
	if (path == NULL || path[0] == '\0') {
		return;
	}

	d->fan_fd = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC,
				  O_RDONLY | O_LARGEFILE);
	if (d->fan_fd < 0) {
		DBG_NOTICE("smoothfs: fanotify_init failed (%s); "
			   "forced-cutover lease-break falls back to "
			   "an external lease_break_agent\n",
			   strerror(errno));
		return;
	}

	flags = fcntl(d->fan_fd, F_GETFL, 0);
	if (flags < 0 || fcntl(d->fan_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		DBG_WARNING("smoothfs: O_NONBLOCK on fanotify fd failed (%s); "
			    "watcher disabled to avoid blocking smbd\n",
			    strerror(errno));
		close(d->fan_fd);
		d->fan_fd = -1;
		return;
	}

	if (fanotify_mark(d->fan_fd,
			  FAN_MARK_ADD | FAN_MARK_MOUNT,
			  FAN_MODIFY,
			  AT_FDCWD, path) < 0) {
		DBG_NOTICE("smoothfs: fanotify_mark(%s) failed (%s); "
			   "watcher disabled\n",
			   path, strerror(errno));
		close(d->fan_fd);
		d->fan_fd = -1;
		return;
	}

	d->handle = handle;
	d->fan_fde = tevent_add_fd(handle->conn->sconn->ev_ctx,
				   d, d->fan_fd,
				   TEVENT_FD_READ,
				   smoothfs_fanotify_handler, d);
	if (d->fan_fde == NULL) {
		DBG_WARNING("smoothfs: tevent_add_fd for fanotify failed; "
			    "watcher disabled\n");
		close(d->fan_fd);
		d->fan_fd = -1;
		return;
	}

	DBG_NOTICE("smoothfs: fanotify lease-break watcher active on %s\n",
		   path);
}

static int smoothfs_connect(vfs_handle_struct *handle,
			    const char *service, const char *user)
{
	struct smoothfs_connect_data *d;
	int ret;

	ret = SMB_VFS_NEXT_CONNECT(handle, service, user);
	if (ret < 0) {
		return ret;
	}

	d = talloc_zero(handle->conn, struct smoothfs_connect_data);
	if (d == NULL) {
		SMB_VFS_NEXT_DISCONNECT(handle);
		errno = ENOMEM;
		return -1;
	}
	talloc_set_destructor(d, smoothfs_connect_data_destructor);
	d->is_smoothfs = smoothfs_probe_lower(handle);

	SMB_VFS_HANDLE_SET_DATA(handle, d, NULL,
				struct smoothfs_connect_data,
				return -1);

	d->lease_watcher = lp_parm_bool(SNUM(handle->conn), "smoothfs",
					"lease watcher", false);
	d->stable_fileid = lp_parm_bool(SNUM(handle->conn), "smoothfs",
					"stable fileid", false);

	if (d->lease_watcher) {
		smoothfs_setup_fanotify(handle, d);
	} else {
		d->fan_fd = -1;
		d->fan_fde = NULL;
	}

	DBG_NOTICE("smoothfs: VFS module loaded for service %s (user %s); "
		   "lower is %s; fanotify %s; stable_fileid %s\n",
		   service ? service : "(unknown)",
		   user ? user : "(unknown)",
		   d->is_smoothfs ? "smoothfs" : "non-smoothfs (passthrough)",
		   d->fan_fde != NULL ? "active" : "inactive",
		   d->stable_fileid ? "active" : "inactive");

	return 0;
}

/*
 * Mirror Samba's kernel-lease lifecycle onto the smoothfs lease pin.
 *
 * Samba calls linux_setlease on the lower fd every time it installs or
 * releases a kernel file lease (F_SETLEASE) — that is the mechanism
 * stock smbd uses to detect foreign writers and break its own SMB
 * oplock/lease. We reuse that exact lifecycle as the signal for
 * SMOOTHFS_PIN_LEASE:
 *
 *   F_RDLCK / F_WRLCK  -> setxattr trusted.smoothfs.lease = 1
 *   F_UNLCK            -> removexattr trusted.smoothfs.lease
 *
 * An xattr failure never promotes to a VFS-level error; we always
 * return whatever the next module returned. The point is that the
 * kernel lease semantics stay identical whether or not the module is
 * loaded — the pin is additive metadata, not a gate on SMB correctness.
 *
 * The kernel's trusted.smoothfs.lease handler is idempotent for the
 * PIN_NONE <-> PIN_LEASE transitions; EBUSY back from the kernel means
 * a competing pin (HARDLINK/LUN/heat) already owns the file, which is
 * fine — that stronger pin keeps movement blocked and our hook has no
 * more work to do.
 */
static int smoothfs_linux_setlease(vfs_handle_struct *handle,
				   files_struct *fsp, int leasetype)
{
	struct smoothfs_connect_data *d = NULL;
	int ret;
	int xret;
	int saved_errno;

	ret = SMB_VFS_NEXT_LINUX_SETLEASE(handle, fsp, leasetype);
	if (ret < 0) {
		return ret;
	}

	SMB_VFS_HANDLE_GET_DATA(handle, d, struct smoothfs_connect_data,
				return ret);
	if (!d->is_smoothfs) {
		return ret;
	}

	saved_errno = errno;
	if (leasetype == F_UNLCK) {
		xret = SMB_VFS_NEXT_FREMOVEXATTR(handle, fsp,
						 SMOOTHFS_LEASE_XATTR);
		if (xret < 0 && errno != ENODATA && errno != ENOATTR) {
			DBG_WARNING("smoothfs: clear lease pin on %s: %s\n",
				    fsp_str_dbg(fsp), strerror(errno));
		} else {
			DBG_DEBUG("smoothfs: cleared lease pin on %s\n",
				  fsp_str_dbg(fsp));
		}
	} else {
		const uint8_t one = 1;

		xret = SMB_VFS_NEXT_FSETXATTR(handle, fsp,
					      SMOOTHFS_LEASE_XATTR,
					      &one, sizeof(one), 0);
		if (xret < 0 && errno == EBUSY) {
			DBG_DEBUG("smoothfs: lease pin on %s skipped; "
				  "stronger pin already in place\n",
				  fsp_str_dbg(fsp));
		} else if (xret < 0) {
			DBG_WARNING("smoothfs: set lease pin on %s: %s\n",
				    fsp_str_dbg(fsp), strerror(errno));
		} else {
			DBG_DEBUG("smoothfs: set lease pin on %s (leasetype=%d)\n",
				  fsp_str_dbg(fsp), leasetype);
		}
	}
	errno = saved_errno;
	return ret;
}

/*
 * Parse a trusted.smoothfs.fileid xattr blob (12 bytes: u64 LE ino |
 * u32 LE gen) and cache the (ino, gen) pair on the connection. Safe
 * to call for every successful openat — duplicate entries update in
 * place, so the list grows only with distinct inode_no values seen
 * during this connection's lifetime.
 */
static void smoothfs_cache_fileid(struct smoothfs_connect_data *d,
				  const uint8_t *blob)
{
	struct smoothfs_fileid_entry *e;
	uint64_t ino_le;
	uint32_t gen_le;
	uint64_t ino;
	uint32_t gen;

	memcpy(&ino_le, blob, sizeof(ino_le));
	memcpy(&gen_le, blob + sizeof(ino_le), sizeof(gen_le));
	ino = le64toh(ino_le);
	gen = le32toh(gen_le);

	for (e = d->fileid_head; e != NULL; e = e->next) {
		if (e->ino == ino) {
			e->gen = gen;
			return;
		}
	}

	e = talloc_zero(d, struct smoothfs_fileid_entry);
	if (e == NULL) {
		return;
	}
	e->ino = ino;
	e->gen = gen;
	e->next = d->fileid_head;
	d->fileid_head = e;
}

/*
 * fstat override: call NEXT, then fold the fsp's
 * trusted.smoothfs.fileid into the per-connection (ino -> gen) cache
 * so file_id_create_fn can look up the extid later. fstat_fn runs
 * after open with a real io fd (openat_fn is too early — Samba
 * sometimes opens with O_PATH to probe the dentry, and fgetxattr
 * refuses those). Non-smoothfs lowers short-circuit. Failures
 * reading the xattr are non-fatal — extid falls back to 0 for that
 * inode, which is what stock Samba would have returned anyway.
 */
static int smoothfs_fstat(vfs_handle_struct *handle,
			  struct files_struct *fsp, SMB_STRUCT_STAT *sbuf)
{
	struct smoothfs_connect_data *d = NULL;
	uint8_t blob[SMOOTHFS_FILEID_XATTR_LEN];
	ssize_t n;
	int ret;

	ret = SMB_VFS_NEXT_FSTAT(handle, fsp, sbuf);
	if (ret < 0) {
		return ret;
	}

	SMB_VFS_HANDLE_GET_DATA(handle, d, struct smoothfs_connect_data,
				return ret);
	if (!d->is_smoothfs || !d->stable_fileid) {
		return ret;
	}

	n = SMB_VFS_NEXT_FGETXATTR(handle, fsp, SMOOTHFS_FILEID_XATTR,
				   blob, sizeof(blob));
	if (n == SMOOTHFS_FILEID_XATTR_LEN) {
		smoothfs_cache_fileid(d, blob);
	} else if (n >= 0) {
		DBG_INFO("smoothfs: fileid xattr short read (%zd bytes)\n", n);
	}
	/* ENODATA / ENOATTR / EBADF (O_PATH fd) are expected; silence. */
	return ret;
}

/*
 * file_id_create override: start from the stock key (dev+inode,
 * extid=0), then upgrade extid to the cached gen if we have one. Non-
 * smoothfs shares pass straight through. Uncached inodes (e.g. metadata
 * operations that hit file_id_create without an intervening openat)
 * get extid=0 which matches stock behaviour.
 */
static struct file_id smoothfs_file_id_create(vfs_handle_struct *handle,
					      const SMB_STRUCT_STAT *sbuf)
{
	struct file_id key;
	struct smoothfs_connect_data *d = NULL;
	struct smoothfs_fileid_entry *e;

	key = SMB_VFS_NEXT_FILE_ID_CREATE(handle, sbuf);

	SMB_VFS_HANDLE_GET_DATA(handle, d, struct smoothfs_connect_data,
				return key);
	if (!d->is_smoothfs || !d->stable_fileid) {
		return key;
	}

	for (e = d->fileid_head; e != NULL; e = e->next) {
		if (e->ino == (uint64_t)sbuf->st_ex_ino) {
			key.extid = e->gen;
			DBG_INFO("smoothfs: file_id ino=%llu gen=%u\n",
				 (unsigned long long)e->ino, e->gen);
			break;
		}
	}
	return key;
}

static struct vfs_fn_pointers smoothfs_fns = {
	.connect_fn = smoothfs_connect,
	.fstat_fn = smoothfs_fstat,
	.linux_setlease_fn = smoothfs_linux_setlease,
	.file_id_create_fn = smoothfs_file_id_create,
	/*
	 * Intentionally empty otherwise. Any op not overridden here
	 * falls through to the next module in the stack (vfs_default
	 * by default), preserving stock Samba behaviour end-to-end.
	 */
};

static_decl_vfs;
NTSTATUS vfs_smoothfs_init(TALLOC_CTX *ctx)
{
	return smb_register_vfs(SMB_VFS_INTERFACE_VERSION, "smoothfs",
				&smoothfs_fns);
}

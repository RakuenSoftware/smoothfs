// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - xattr handlers.
 *
 * Per Phase 0 §POSIX semantics — xattrs:
 *   user.*               passthrough
 *   trusted.smoothfs.*   reserved (oid/gen/fileid/lease) — root only
 *   trusted.*            passthrough (root only) for any other prefix
 *   security.*           passthrough; preserved across movement
 *   system.posix_acl_*   handled by acl.c via the ACL handler chain
 *
 * Reserved names (all within trusted.smoothfs.*):
 *   oid         16 bytes      UUIDv7, served from si->oid; set writes through to the lower
 *   gen          4 bytes LE   u32, served from si->gen; set writes through to the lower
 *   fileid      12 bytes LE   inode_no (u64) | gen (u32); read-only, computed from si — Phase 5.0, SMB FileId source
 *   lease        1 byte       0/1, toggles pin_state between PIN_NONE and PIN_LEASE;
 *                             not persisted to the lower — Phase 5.0 lease-pin hook
 *   lun          1 byte       0/1, toggles pin_state between PIN_NONE and PIN_LUN;
 *                             not persisted to the lower — Phase 6.2 LUN-pin hook
 */

#include <linux/fs.h>
#include <linux/xattr.h>
#include <linux/capability.h>
#include <linux/string.h>

#include "smoothfs.h"

static struct dentry *xattr_lower(struct dentry *dentry)
{
	return smoothfs_lower_dentry(dentry);
}

static int generic_xattr_get(const struct xattr_handler *handler,
			     struct dentry *dentry, struct inode *inode,
			     const char *name, void *buffer, size_t size)
{
	struct dentry *lower = xattr_lower(dentry);
	char fullname[XATTR_NAME_MAX + 1];

	if (snprintf(fullname, sizeof(fullname), "%s%s",
		     handler->prefix ? handler->prefix : "", name)
	    >= (int)sizeof(fullname))
		return -ENAMETOOLONG;

	return vfs_getxattr(&nop_mnt_idmap, lower, fullname, buffer, size);
}

static int generic_xattr_set(const struct xattr_handler *handler,
			     struct mnt_idmap *idmap,
			     struct dentry *dentry, struct inode *inode,
			     const char *name, const void *value, size_t size,
			     int flags)
{
	struct dentry *lower = xattr_lower(dentry);
	char fullname[XATTR_NAME_MAX + 1];

	if (snprintf(fullname, sizeof(fullname), "%s%s",
		     handler->prefix ? handler->prefix : "", name)
	    >= (int)sizeof(fullname))
		return -ENAMETOOLONG;

	if (!value)
		return vfs_removexattr(idmap, lower, fullname);
	return vfs_setxattr(idmap, lower, fullname, value, size, flags);
}

static const struct xattr_handler smoothfs_user_xattr_handler = {
	.prefix = XATTR_USER_PREFIX,
	.get    = generic_xattr_get,
	.set    = generic_xattr_set,
};

/* trusted.smoothfs.* is reserved by Phase 0 §POSIX; any non-reserved
 * trusted.* xattr passes through to the lower (root-only via VFS). */
static int trusted_xattr_get(const struct xattr_handler *handler,
			     struct dentry *dentry, struct inode *inode,
			     const char *name, void *buffer, size_t size)
{
	struct smoothfs_inode_info *si;
	const u8 zero_oid[SMOOTHFS_OID_LEN] = { 0 };

	/* Fast path: serve trusted.smoothfs.oid and trusted.smoothfs.gen
	 * from si directly — we minted them at iget and track them in
	 * memory, so a vfs_getxattr through to the lower is redundant.
	 * Also covers the window where an OID has been minted but the
	 * writeback queue hasn't flushed to the lower yet — without this
	 * short-circuit the lower would return -ENODATA for such files
	 * even though the OID is live in-kernel. */
	if (inode && inode->i_sb->s_magic == SMOOTHFS_MAGIC) {
		si = SMOOTHFS_I(inode);
		if (strcmp(name, "smoothfs.oid") == 0 &&
		    memcmp(si->oid, zero_oid, SMOOTHFS_OID_LEN) != 0) {
			if (!buffer)
				return SMOOTHFS_OID_LEN;
			if (size < SMOOTHFS_OID_LEN)
				return -ERANGE;
			memcpy(buffer, si->oid, SMOOTHFS_OID_LEN);
			return SMOOTHFS_OID_LEN;
		}
		if (strcmp(name, "smoothfs.gen") == 0) {
			__le32 v = cpu_to_le32(si->gen);
			if (!buffer)
				return sizeof(v);
			if (size < sizeof(v))
				return -ERANGE;
			memcpy(buffer, &v, sizeof(v));
			return sizeof(v);
		}
		/* smoothfs.fileid is the SMB FileId source: inode_no (u64 LE)
		 * concatenated with gen (u32 LE). Computed every call — no
		 * lower passthrough, no persistence. */
		if (strcmp(name, "smoothfs.fileid") == 0) {
			__le64 ino = cpu_to_le64(inode->i_ino);
			__le32 gen = cpu_to_le32(si->gen);
			const size_t total = sizeof(ino) + sizeof(gen);

			if (!buffer)
				return total;
			if (size < total)
				return -ERANGE;
			memcpy(buffer, &ino, sizeof(ino));
			memcpy((u8 *)buffer + sizeof(ino), &gen, sizeof(gen));
			return total;
		}
		/* smoothfs.lease reflects pin_state. Returns 1 iff currently
		 * PIN_LEASE, otherwise 0. Not persisted to the lower. */
		if (strcmp(name, "smoothfs.lease") == 0) {
			u8 v = (si->pin_state == SMOOTHFS_PIN_LEASE) ? 1 : 0;

			if (!buffer)
				return sizeof(v);
			if (size < sizeof(v))
				return -ERANGE;
			memcpy(buffer, &v, sizeof(v));
			return sizeof(v);
		}
		/* smoothfs.lun — Phase 6.2. Reflects pin_state == PIN_LUN.
		 * Same shape as smoothfs.lease, distinct pin slot so the two
		 * never overwrite each other. Not persisted to the lower. */
		if (strcmp(name, "smoothfs.lun") == 0) {
			u8 v = (si->pin_state == SMOOTHFS_PIN_LUN) ? 1 : 0;

			if (!buffer)
				return sizeof(v);
			if (size < sizeof(v))
				return -ERANGE;
			memcpy(buffer, &v, sizeof(v));
			return sizeof(v);
		}
	}
	return generic_xattr_get(handler, dentry, inode, name, buffer, size);
}

static int trusted_xattr_set(const struct xattr_handler *handler,
			     struct mnt_idmap *idmap,
			     struct dentry *dentry, struct inode *inode,
			     const char *name, const void *value, size_t size,
			     int flags)
{
	if (strncmp(name, "smoothfs.", 9) == 0) {
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;

		/* smoothfs.fileid is computed, not stored. */
		if (strcmp(name, "smoothfs.fileid") == 0)
			return -EPERM;

		/* smoothfs.lease: toggle pin_state without touching the lower.
		 * Accepts a single-byte 0/1 on set, or removal. Leaves pins
		 * other than PIN_LEASE / PIN_NONE alone so HARDLINK/LUN/heat
		 * pins set by unrelated subsystems don't get clobbered. */
		if (inode && inode->i_sb->s_magic == SMOOTHFS_MAGIC &&
		    strcmp(name, "smoothfs.lease") == 0) {
			struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
			u8 want;

			if (!value) {
				want = 0;
			} else {
				if (size != 1)
					return -EINVAL;
				want = *(const u8 *)value;
				if (want != 0 && want != 1)
					return -EINVAL;
			}
			if (want) {
				if (si->pin_state == SMOOTHFS_PIN_NONE)
					si->pin_state = SMOOTHFS_PIN_LEASE;
				else if (si->pin_state != SMOOTHFS_PIN_LEASE)
					return -EBUSY;
			} else {
				if (si->pin_state == SMOOTHFS_PIN_LEASE)
					si->pin_state = SMOOTHFS_PIN_NONE;
			}
			return 0;
		}
		/* smoothfs.lun — Phase 6.2. Flips pin_state between
		 * PIN_NONE and PIN_LUN, same shape as smoothfs.lease but a
		 * different pin slot. Intended caller is tierd (when it
		 * records a LUN backing file) or an operator via setfattr;
		 * LIO itself has no idea about the smoothfs pin contract so
		 * we never try to auto-detect it inside the kernel. Unlike
		 * PIN_LEASE, PIN_LUN is *not* overridable by force=true on
		 * MOVE_PLAN — §iSCSI in Phase 0 rules that LUN movement is
		 * administrative only (must quiesce the target first). */
		if (inode && inode->i_sb->s_magic == SMOOTHFS_MAGIC &&
		    strcmp(name, "smoothfs.lun") == 0) {
			struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
			u8 want;

			if (!value) {
				want = 0;
			} else {
				if (size != 1)
					return -EINVAL;
				want = *(const u8 *)value;
				if (want != 0 && want != 1)
					return -EINVAL;
			}
			if (want) {
				if (si->pin_state == SMOOTHFS_PIN_NONE)
					si->pin_state = SMOOTHFS_PIN_LUN;
				else if (si->pin_state != SMOOTHFS_PIN_LUN)
					return -EBUSY;
			} else {
				if (si->pin_state == SMOOTHFS_PIN_LUN)
					si->pin_state = SMOOTHFS_PIN_NONE;
			}
			return 0;
		}
	}
	return generic_xattr_set(handler, idmap, dentry, inode, name, value,
				 size, flags);
}

static const struct xattr_handler smoothfs_trusted_xattr_handler = {
	.prefix = XATTR_TRUSTED_PREFIX,
	.get    = trusted_xattr_get,
	.set    = trusted_xattr_set,
};

static const struct xattr_handler smoothfs_security_xattr_handler = {
	.prefix = XATTR_SECURITY_PREFIX,
	.get    = generic_xattr_get,
	.set    = generic_xattr_set,
};

const struct xattr_handler * const smoothfs_xattr_handlers[] = {
	&smoothfs_user_xattr_handler,
	&smoothfs_trusted_xattr_handler,
	&smoothfs_security_xattr_handler,
	NULL,
};

/*
 * smoothfs_listxattr: pass through to the lower dentry.
 *
 * generic_listxattr (the kernel default) only emits names for xattr
 * handlers that define a fixed .name — our handlers are .prefix-only
 * (user.* / trusted.* / security.*), so generic_listxattr returns an
 * empty list. That silently breaks any caller doing listxattr +
 * getxattr-by-name to enumerate a file's EAs, most visibly Samba's
 * SMB_FIND_FILE_EA_LIST response which reads the full EA list via
 * SMB_VFS_FLISTXATTR on every matching dirent (cthon04's raw.search
 * :: ea list subtest catches this). Direct getxattr-by-name still
 * works because the handlers' .get forwards to the lower.
 *
 * Delegate to the lower's ->listxattr via vfs_listxattr. The lower's
 * own kernel list already includes our reserved trusted.smoothfs.*
 * names; leaving them visible is consistent with getxattr behaviour
 * (root can already read them there).
 */
ssize_t smoothfs_listxattr(struct dentry *dentry, char *list, size_t size)
{
	struct dentry *lower = smoothfs_lower_dentry(dentry);

	if (!lower)
		return -EINVAL;
	return vfs_listxattr(lower, list, size);
}

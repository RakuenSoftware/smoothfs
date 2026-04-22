// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - inode operations.
 *
 * Phase 1 implements the full passthrough op surface that the parent
 * proposal §Phase 1 lists: lookup, create, mknod, link, symlink, mkdir,
 * rmdir, rename, unlink, getattr, setattr, plus xattr/ACL/lock via the
 * separate handler tables in xattr.c / acl.c / lock.c.
 *
 * Targets kernel >= 6.6 (mnt_idmap-based ops, int-returning mkdir).
 * The Phase 0 contract §Operational Delivery names the appliance
 * kernel matrix as still-outstanding; the version pin lives in
 * dkms.conf and the kernel-matrix decision document.
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/cred.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/xattr.h>

#include "smoothfs.h"

/* Kernel-version pin lives in compat.h. */

/* ----------------------------------------------------------------- */
/* Lookup                                                            */
/* ----------------------------------------------------------------- */

static struct dentry *smoothfs_lookup(struct inode *dir, struct dentry *dentry,
				      unsigned int flags)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(dir->i_sb);
	struct smoothfs_inode_info *parent = SMOOTHFS_I(dir);
	struct dentry *lower_parent = parent->lower_path.dentry;
	struct dentry *lower;
	struct path lower_path;
	struct inode *inode = NULL;

	inode_lock_shared(d_inode(lower_parent));
	lower = smoothfs_compat_lookup(&nop_mnt_idmap, &dentry->d_name, lower_parent);
	inode_unlock_shared(d_inode(lower_parent));

	if (IS_ERR(lower))
		return ERR_CAST(lower);

	if (d_really_is_positive(lower)) {
		lower_path.mnt = parent->lower_path.mnt;
		lower_path.dentry = lower;
		mntget(lower_path.mnt);

		inode = smoothfs_iget(dir->i_sb, &lower_path, false);
		path_put(&lower_path);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
		/* path_put consumed our lookup_one ref on lower; the inode
		 * now holds its own via si->lower_path. The helper will
		 * take d_fsdata's own ref below. */
		smoothfs_set_lower_dentry(dentry, lower);
		return d_splice_alias(inode, dentry);
	}

	/* Negative lookup. lower still holds our lookup_one reference. */
	{
		char *rel_path = NULL;
		struct smoothfs_inode_info *replayed;

		{
			char *buf, *path;

			buf = kmalloc(PATH_MAX, GFP_KERNEL);
			if (!buf) {
				dput(lower);
				return ERR_PTR(-ENOMEM);
			}
			path = dentry_path_raw(dentry, buf, PATH_MAX);
			if (!IS_ERR(path)) {
				if (*path == '/')
					path++;
				rel_path = kstrdup(path, GFP_KERNEL);
			}
			kfree(buf);
		}
		if (rel_path) {
			replayed = smoothfs_lookup_rel_path(sbi, rel_path);
			if (replayed) {
				struct path replay_path = replayed->lower_path;
				int was_replay_pinned;

				inode = igrab(&replayed->vfs_inode);
				if (!inode) {
					kfree(rel_path);
					dput(lower);
					return ERR_PTR(-ESTALE);
				}
				/* Hand off the placement-replay pin to this
				 * lookup's dentry alias. d_splice_alias below
				 * will drop the dentry's caller ref into the
				 * dentry; iput here releases the original
				 * placement_replay-held ref. Without this
				 * handoff the pin would survive past umount. */
				was_replay_pinned =
					atomic_xchg(&replayed->replay_pinned, 0);
				if (was_replay_pinned)
					iput(&replayed->vfs_inode);
				dput(lower);
				lower = dget(replay_path.dentry);
			}
			kfree(rel_path);
		}
	}
	smoothfs_set_lower_dentry(dentry, lower);
	dput(lower);
	return d_splice_alias(inode, dentry);
}

/* ----------------------------------------------------------------- */
/* getattr / setattr                                                 */
/* ----------------------------------------------------------------- */

static int smoothfs_getattr(struct mnt_idmap *idmap, const struct path *path,
			    struct kstat *stat, u32 request_mask,
			    unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct path lower_path = *smoothfs_lower_path(inode);
	int err;

	/* Direct passthrough to the lower. vfs_getattr_nosec skips the
	 * security_inode_getattr hook because the VFS already ran it on
	 * the smoothfs path before dispatching to us. Avoiding the double
	 * LSM invocation is a measurable slice of the Phase 3 STAT p99.
	 *
	 * Mirror-on-every-getattr (the Phase 1/2 behaviour) is also gone:
	 * its per-call seqlock writes (i_size, set_nlink, *time_to_ts)
	 * were the other major contributor; smoothfs_copy_attrs now runs
	 * only on create/rename/cutover/setattr. */
	err = vfs_getattr_nosec(&lower_path, stat, request_mask, flags);
	if (err)
		return err;

	/* Preserve smoothfs's synthesised inode identity (§0.1). */
	stat->ino = inode->i_ino;
	stat->dev = inode->i_sb->s_dev;
	return 0;
}

static int smoothfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			    struct iattr *attr)
{
	struct dentry *lower = smoothfs_lower_dentry(dentry);
	struct inode *inode = d_inode(dentry);
	int err;

	err = setattr_prepare(idmap, dentry, attr);
	if (err)
		return err;

	inode_lock(d_inode(lower));
	err = notify_change(idmap, lower, attr, NULL);
	inode_unlock(d_inode(lower));
	if (err)
		return err;

	smoothfs_copy_attrs(inode, d_inode(lower));
	return 0;
}

/* ----------------------------------------------------------------- */
/* Create / mknod / symlink / link / mkdir / rmdir / unlink / rename */
/* ----------------------------------------------------------------- */

static int smoothfs_create(struct mnt_idmap *idmap, struct inode *dir,
			   struct dentry *dentry, umode_t mode, bool excl)
{
	struct dentry *lower_parent = smoothfs_lower_dentry(dentry->d_parent);
	struct dentry *lower;
	struct path lower_path;
	struct inode *inode;
	int err;

	inode_lock(d_inode(lower_parent));
	lower = smoothfs_compat_lookup(&nop_mnt_idmap, &dentry->d_name, lower_parent);
	if (IS_ERR(lower)) {
		inode_unlock(d_inode(lower_parent));
		return PTR_ERR(lower);
	}
	err = smoothfs_compat_create(idmap, d_inode(lower_parent), lower, mode, excl);
	inode_unlock(d_inode(lower_parent));
	if (err) {
		dput(lower);
		return err;
	}

	lower_path.mnt = SMOOTHFS_I(dir)->lower_path.mnt;
	lower_path.dentry = lower;
	mntget(lower_path.mnt);

	inode = smoothfs_iget(dir->i_sb, &lower_path, false);
	path_put(&lower_path);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	smoothfs_set_lower_dentry(dentry, lower);
	d_instantiate(dentry, inode);
	smoothfs_copy_attrs(dir, d_inode(lower_parent));
	return 0;
}

static int smoothfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
			  struct dentry *dentry, umode_t mode, dev_t rdev)
{
	struct dentry *lower_parent = smoothfs_lower_dentry(dentry->d_parent);
	struct dentry *lower;
	struct path lower_path;
	struct inode *inode;
	int err;

	inode_lock(d_inode(lower_parent));
	lower = smoothfs_compat_lookup(&nop_mnt_idmap, &dentry->d_name, lower_parent);
	if (IS_ERR(lower)) {
		inode_unlock(d_inode(lower_parent));
		return PTR_ERR(lower);
	}
	err = smoothfs_compat_mknod(idmap, d_inode(lower_parent), lower, mode, rdev);
	inode_unlock(d_inode(lower_parent));
	if (err) {
		dput(lower);
		return err;
	}

	lower_path.mnt = SMOOTHFS_I(dir)->lower_path.mnt;
	lower_path.dentry = lower;
	mntget(lower_path.mnt);

	inode = smoothfs_iget(dir->i_sb, &lower_path, false);
	path_put(&lower_path);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	smoothfs_set_lower_dentry(dentry, lower);
	d_instantiate(dentry, inode);
	smoothfs_copy_attrs(dir, d_inode(lower_parent));
	return 0;
}

static int smoothfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			    struct dentry *dentry, const char *symname)
{
	struct dentry *lower_parent = smoothfs_lower_dentry(dentry->d_parent);
	struct dentry *lower;
	struct path lower_path;
	struct inode *inode;
	int err;

	inode_lock(d_inode(lower_parent));
	lower = smoothfs_compat_lookup(&nop_mnt_idmap, &dentry->d_name, lower_parent);
	if (IS_ERR(lower)) {
		inode_unlock(d_inode(lower_parent));
		return PTR_ERR(lower);
	}
	err = smoothfs_compat_symlink(idmap, d_inode(lower_parent), lower, symname);
	inode_unlock(d_inode(lower_parent));
	if (err) {
		dput(lower);
		return err;
	}

	lower_path.mnt = SMOOTHFS_I(dir)->lower_path.mnt;
	lower_path.dentry = lower;
	mntget(lower_path.mnt);

	inode = smoothfs_iget(dir->i_sb, &lower_path, false);
	path_put(&lower_path);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	smoothfs_set_lower_dentry(dentry, lower);
	d_instantiate(dentry, inode);
	smoothfs_copy_attrs(dir, d_inode(lower_parent));
	return 0;
}

/* link(2): always within one tier. Cross-tier link returns EXDEV per
 * POSIX semantics §0.4. Phase 1 keeps the source tier; the
 * scheduler-observed nlink>1 will pin the link-set per Phase 2. */
static int smoothfs_link(struct dentry *old_dentry, struct inode *dir,
			 struct dentry *dentry)
{
	struct dentry *lower_old = smoothfs_lower_dentry(old_dentry);
	struct dentry *lower_parent = smoothfs_lower_dentry(dentry->d_parent);
	struct smoothfs_inode_info *si = SMOOTHFS_I(d_inode(old_dentry));
	struct dentry *lower;
	int err;

	if (lower_old->d_sb != lower_parent->d_sb)
		return -EXDEV;

	inode_lock(d_inode(lower_parent));
	lower = smoothfs_compat_lookup(&nop_mnt_idmap, &dentry->d_name, lower_parent);
	if (IS_ERR(lower)) {
		inode_unlock(d_inode(lower_parent));
		return PTR_ERR(lower);
	}
	err = vfs_link(lower_old, &nop_mnt_idmap, d_inode(lower_parent),
		       lower, NULL);
	inode_unlock(d_inode(lower_parent));
	if (err) {
		dput(lower);
		return err;
	}

	atomic_inc(&si->nlink_observed);
	/* Phase 0 §0.4: hardlink-set is pinned to current tier as soon
	 * as nlink > 1. Cleared automatically when nlink returns to 1
	 * via smoothfs_unlink. */
	if (atomic_read(&si->nlink_observed) > 1 &&
	    si->pin_state == SMOOTHFS_PIN_NONE)
		si->pin_state = SMOOTHFS_PIN_HARDLINK;

	smoothfs_set_lower_dentry(dentry, lower);
	dput(lower);
	ihold(d_inode(old_dentry));
	d_instantiate(dentry, d_inode(old_dentry));
	smoothfs_copy_attrs(d_inode(old_dentry), d_inode(lower_old));
	smoothfs_copy_attrs(dir, d_inode(lower_parent));
	return 0;
}

static int smoothfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct dentry *lower_parent = smoothfs_lower_dentry(dentry->d_parent);
	struct dentry *lower = smoothfs_lower_dentry(dentry);
	struct dentry *removing;
	struct inode *lower_dir = NULL;
	struct smoothfs_inode_info *si = SMOOTHFS_I(d_inode(dentry));
	int err;

	removing = smoothfs_compat_start_removing(lower_parent, lower, &lower_dir);
	if (IS_ERR(removing))
		return PTR_ERR(removing);
	err = vfs_unlink(&nop_mnt_idmap, lower_dir, removing, NULL);
	smoothfs_compat_end_removing(removing, lower_dir);

	if (!err) {
		atomic_dec(&si->nlink_observed);
		/* Clear hardlink pin once link-set returns to 1. */
		if (atomic_read(&si->nlink_observed) <= 1 &&
		    si->pin_state == SMOOTHFS_PIN_HARDLINK)
			si->pin_state = SMOOTHFS_PIN_NONE;
		/*
		 * Drop d_fsdata so this dentry stops pinning the lower
		 * dentry, but leave si->lower_path intact — the inode may
		 * still be alive (another hardlink, or nfsd holds a file
		 * handle), and getattr/read/write all deref si->lower_path
		 * unconditionally. evict_inode does the matching path_put
		 * when the inode's refcount actually drops to zero. An
		 * earlier version cleared si->lower_path here to work
		 * around a 6.19 vfs_rmdir d_walk stall; that stall has a
		 * different cause (since fixed upstream) and this clear
		 * introduced a NULL-deref under nfsd GETATTR after NFS
		 * UNLINK.
		 *
		 * drop_nlink (not clear_nlink) is what vfs_unlink did to
		 * the lower inode: decrement by one. If this was the last
		 * link, nlink goes to 0 and evict_inode takes over. If
		 * other hardlinks remain, nlink stays > 0 so vfs_link's
		 * i_nlink==0 guard (cthon04 basic/test7) does not spuriously
		 * refuse a subsequent link to the still-live inode.
		 */
		drop_nlink(d_inode(dentry));
		smoothfs_set_lower_dentry(dentry, NULL);
		d_drop(dentry);
		smoothfs_copy_attrs(dir, d_inode(lower_parent));
	}
	return err;
}

static struct dentry *smoothfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				     struct dentry *dentry, umode_t mode)
{
	struct dentry *lower_parent = smoothfs_lower_dentry(dentry->d_parent);
	struct dentry *lower;
	struct path lower_path;
	struct inode *inode;

	inode_lock(d_inode(lower_parent));
	lower = smoothfs_compat_lookup(&nop_mnt_idmap, &dentry->d_name, lower_parent);
	if (IS_ERR(lower)) {
		inode_unlock(d_inode(lower_parent));
		return ERR_CAST(lower);
	}
	{
		struct dentry *new_lower = smoothfs_compat_mkdir(idmap,
							d_inode(lower_parent),
							lower, mode);
		inode_unlock(d_inode(lower_parent));
		if (IS_ERR(new_lower)) {
			dput(lower);
			return ERR_CAST(new_lower);
		}
		if (new_lower != lower) {
			dput(lower);
			lower = new_lower;
		}
	}

	lower_path.mnt = SMOOTHFS_I(dir)->lower_path.mnt;
	lower_path.dentry = lower;
	mntget(lower_path.mnt);

	inode = smoothfs_iget(dir->i_sb, &lower_path, false);
	path_put(&lower_path);
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	smoothfs_set_lower_dentry(dentry, lower);
	d_instantiate(dentry, inode);
	smoothfs_copy_attrs(dir, d_inode(lower_parent));
	return NULL;
}

static int smoothfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct dentry *lower_parent = smoothfs_lower_dentry(dentry->d_parent);
	struct dentry *lower = smoothfs_lower_dentry(dentry);
	struct dentry *removing;
	struct inode *lower_dir = NULL;
	int err;

	removing = smoothfs_compat_start_removing(lower_parent, lower, &lower_dir);
	if (IS_ERR(removing))
		return PTR_ERR(removing);
	err = smoothfs_compat_rmdir(&nop_mnt_idmap, lower_dir, removing);
	smoothfs_compat_end_removing(removing, lower_dir);

	if (!err) {
		/* Drop d_fsdata so this dentry releases its pin on the
		 * lower dentry; si->lower_path stays until evict_inode.
		 * See smoothfs_unlink for the full rationale. */
		clear_nlink(d_inode(dentry));
		smoothfs_set_lower_dentry(dentry, NULL);
		d_drop(dentry);
		smoothfs_copy_attrs(dir, d_inode(lower_parent));
	}
	return err;
}

static int smoothfs_rename(struct mnt_idmap *idmap,
			   struct inode *old_dir, struct dentry *old_dentry,
			   struct inode *new_dir, struct dentry *new_dentry,
			   unsigned int flags)
{
	struct dentry *lower_old_parent = smoothfs_lower_dentry(old_dentry->d_parent);
	struct dentry *lower_new_parent = smoothfs_lower_dentry(new_dentry->d_parent);
	struct dentry *lower_old = smoothfs_lower_dentry(old_dentry);
	struct dentry *lower_new = smoothfs_lower_dentry(new_dentry);
	struct dentry *trap;
	struct renamedata rd = {};
	int err;

	if (lower_old_parent->d_sb != lower_new_parent->d_sb)
		return -EXDEV;
	if (lower_new)
		dget(lower_new);

	trap = lock_rename_child(lower_old, lower_new_parent);
	if (IS_ERR(trap)) {
		if (lower_new)
			dput(lower_new);
		return PTR_ERR(trap);
	}
	if (!lower_new) {
		lower_new = smoothfs_compat_lookup(&nop_mnt_idmap,
						   &new_dentry->d_name,
						   lower_new_parent);
		if (IS_ERR(lower_new)) {
			unlock_rename(lower_old_parent, lower_new_parent);
			return PTR_ERR(lower_new);
		}
	}
	if (d_unhashed(lower_old) || lower_old_parent != lower_old->d_parent) {
		unlock_rename(lower_old->d_parent, lower_new_parent);
		dput(lower_new);
		return -EINVAL;
	}
	if (d_unhashed(lower_new) || lower_new_parent != lower_new->d_parent) {
		unlock_rename(lower_old->d_parent, lower_new_parent);
		dput(lower_new);
		return -EINVAL;
	}
	if (trap == lower_old) {
		unlock_rename(lower_old->d_parent, lower_new_parent);
		dput(lower_new);
		return -EINVAL;
	}
	if (trap == lower_new) {
		err = (flags & RENAME_EXCHANGE) ? -EINVAL : -ENOTEMPTY;
		unlock_rename(lower_old->d_parent, lower_new_parent);
		dput(lower_new);
		return err;
	}
	if (d_really_is_positive(lower_new) && (flags & RENAME_NOREPLACE)) {
		unlock_rename(lower_old_parent, lower_new_parent);
		dput(lower_new);
		return -EEXIST;
	}

	memset(&rd, 0, sizeof(rd));
	rd.mnt_idmap = idmap;
	rd.old_parent = dget(lower_old->d_parent);
	rd.old_dentry = dget(lower_old);
	rd.new_parent = lower_new_parent;
	rd.new_dentry = dget(lower_new);
	rd.flags = flags;

	err = vfs_rename(&rd);
	unlock_rename(rd.old_parent, rd.new_parent);
	dput(rd.old_dentry);
	dput(rd.new_dentry);
	dput(rd.old_parent);
	if (err) {
		dput(lower_new);
		return err;
	}

	/* After this ->rename returns, vfs_rename calls d_move(old_dentry,
	 * new_dentry) which keeps OLD_dentry as the surviving dentry at
	 * the new position (with new's name). Any subsequent path walk for
	 * the new name finds OLD_dentry in the dcache, and its d_fsdata
	 * must point at the (renamed) lower — otherwise smoothfs_unlink /
	 * smoothfs_getattr on that dentry deref NULL. new_dentry becomes a
	 * throw-away after d_move, so clearing ITS d_fsdata is the right
	 * thing.
	 *
	 * (This was latent until Phase 4-prep's compat lookup fix made
	 * dentries actually stay cached across syscalls. Prior to that the
	 * next path walk always re-ran smoothfs_lookup and recreated a
	 * fresh dentry with a valid d_fsdata, so the mis-assignment never
	 * produced an observable fault.) */
	smoothfs_set_lower_dentry(old_dentry, lower_old);
	smoothfs_set_lower_dentry(new_dentry, NULL);
	dput(lower_new);
	smoothfs_copy_attrs(old_dir, d_inode(lower_old_parent));
	smoothfs_copy_attrs(new_dir, d_inode(lower_new_parent));
	return 0;
}

/*
 * .permission is deliberately NOT installed. The VFS falls back to
 * generic_permission(inode, mask) on the smoothfs inode, using the
 * mode/uid/gid we mirror from the lower at create time and refresh
 * in smoothfs_setattr. For the Phase 3 compat set (xfs, ext4, btrfs,
 * zfs) that's semantically identical to routing through the lower's
 * inode_permission — and it saves a full access-check stack + LSM
 * hook on every path-walk step.
 */

/* ----------------------------------------------------------------- */
/* Symlink readlink (get_link)                                       */
/* ----------------------------------------------------------------- */

static const char *smoothfs_get_link(struct dentry *dentry, struct inode *inode,
				     struct delayed_call *done)
{
	struct path *lower_path;
	struct dentry *lower;

	if (!dentry)
		return ERR_PTR(-ECHILD);

	lower = smoothfs_lower_dentry(dentry);
	if (!lower)
		return ERR_PTR(-EINVAL);
	lower_path = smoothfs_lower_path(inode);

	return vfs_get_link(lower, done);
}

/* ----------------------------------------------------------------- */
/* Dentry ops — d_revalidate trusts the lower's revalidator           */
/* ----------------------------------------------------------------- */

static int smoothfs_d_revalidate(struct inode *dir, const struct qstr *name,
				 struct dentry *dentry, unsigned int flags)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(dentry->d_sb);
	const struct dentry_operations *lower_ops;
	struct dentry *lower;
	struct dentry *lower_parent;

	/* Fast path for the Phase 3 compat set (xfs, ext4, btrfs, zfs):
	 * none of those lowers installs d_revalidate, so the probe marks
	 * any_lower_revalidates = false and every path-walk step returns
	 * 1 without dereferencing d_parent. Safe in RCU-walk too —
	 * dentry->d_sb is stable for a dentry's lifetime. */
	if (!READ_ONCE(sbi->any_lower_revalidates))
		return 1;

	lower = smoothfs_lower_dentry(dentry);
	if (!lower)
		return 0;
	lower_ops = lower->d_op;
	if (!lower_ops || !lower_ops->d_revalidate)
		return 1;

	/* Lower actually has a revalidator — we need d_parent, which is
	 * not safe to chase under RCU-walk. Downgrade to ref-walk. */
	if (flags & LOOKUP_RCU)
		return -ECHILD;

	lower_parent = smoothfs_lower_dentry(dentry->d_parent);
	return smoothfs_compat_lower_d_revalidate(lower_ops,
		lower_parent ? d_inode(lower_parent) : NULL,
		name, lower, flags);
}

static void smoothfs_d_release(struct dentry *dentry)
{
	struct dentry *lower = smoothfs_lower_dentry(dentry);

	if (lower)
		dput(lower);
}

const struct dentry_operations smoothfs_dentry_ops = {
	.d_revalidate = smoothfs_d_revalidate,
	.d_release    = smoothfs_d_release,
};

/* ----------------------------------------------------------------- */
/* Operation tables                                                   */
/* ----------------------------------------------------------------- */

const struct inode_operations smoothfs_dir_inode_ops = {
	.lookup     = smoothfs_lookup,
	.create     = smoothfs_create,
	.mknod      = smoothfs_mknod,
	.symlink    = smoothfs_symlink,
	.link       = smoothfs_link,
	.unlink     = smoothfs_unlink,
	.mkdir      = smoothfs_mkdir,
	.rmdir      = smoothfs_rmdir,
	.rename     = smoothfs_rename,
	.getattr    = smoothfs_getattr,
	.setattr    = smoothfs_setattr,
	.listxattr  = smoothfs_listxattr,
#ifdef CONFIG_FS_POSIX_ACL
	.get_inode_acl = smoothfs_get_inode_acl,
	.set_acl       = smoothfs_set_acl,
#endif
};

const struct inode_operations smoothfs_file_inode_ops = {
	.getattr    = smoothfs_getattr,
	.setattr    = smoothfs_setattr,
	.listxattr  = smoothfs_listxattr,
#ifdef CONFIG_FS_POSIX_ACL
	.get_inode_acl = smoothfs_get_inode_acl,
	.set_acl       = smoothfs_set_acl,
#endif
};

const struct inode_operations smoothfs_symlink_inode_ops = {
	.get_link   = smoothfs_get_link,
	.getattr    = smoothfs_getattr,
	.setattr    = smoothfs_setattr,
	.listxattr  = smoothfs_listxattr,
};

const struct inode_operations smoothfs_special_inode_ops = {
	.getattr    = smoothfs_getattr,
	.setattr    = smoothfs_setattr,
	.listxattr  = smoothfs_listxattr,
};

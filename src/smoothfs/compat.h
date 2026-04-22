/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * smoothfs - kernel-version compatibility shims.
 *
 * Single source of truth for every API that drifts between kernel
 * versions we care about. The rest of the module includes this and
 * uses the smoothfs_compat_*() helpers; no other file should contain
 * `#if LINUX_VERSION_CODE`.
 *
 * --------------------------------------------------------------------
 *  How to bump the kernel floor (e.g. from 6.18 LTS to 7.x)
 * --------------------------------------------------------------------
 *
 *  1. Bump SMOOTHFS_KERNEL_FLOOR_MAJOR / _MINOR to the new floor.
 *     The compile-time check at the bottom of this header rejects
 *     older kernels with a clear #error.
 *
 *  2. For each existing shim below: if the new floor makes the
 *     pre-shim compatibility branch dead, delete it. (Don't leave
 *     dead branches around — they make the next bump harder.)
 *
 *  3. For each NEW kernel API we want to adopt that exists from the
 *     new floor onward: add a #if LINUX_VERSION_CODE >= KERNEL_VERSION
 *     branch HERE. Add the shim, expose it as smoothfs_compat_*(),
 *     and let the call site use the helper unconditionally.
 *
 *  4. Update src/smoothfs/dkms.conf BUILD_EXCLUSIVE_KERNEL to match
 *     the new floor regex.
 *
 *  5. Update docs/proposals/pending/smoothfs-stacked-tiering.md
 *     §Implementation Status with the floor bump.
 *
 *  6. Smoke test on the test server, then commit.
 *
 * The point: bumping floors is a header edit + dead-branch sweep,
 * not a tour of every .c file.
 */

#ifndef _SMOOTHFS_COMPAT_H
#define _SMOOTHFS_COMPAT_H

#include <linux/version.h>

#define SMOOTHFS_KERNEL_FLOOR_MAJOR  6
#define SMOOTHFS_KERNEL_FLOOR_MINOR  18

#define SMOOTHFS_KERNEL_FLOOR \
	KERNEL_VERSION(SMOOTHFS_KERNEL_FLOOR_MAJOR, \
		       SMOOTHFS_KERNEL_FLOOR_MINOR, 0)

#if LINUX_VERSION_CODE < SMOOTHFS_KERNEL_FLOOR
#error "smoothfs requires a newer kernel — see compat.h for the floor pin"
#endif

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/mm.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/filelock.h>

/* ---------- dentry-op installation ----------
 * 6.18: set_default_d_op(sb, ops)
 * pre-6.18: sb->s_d_op = ops
 *
 * Floor is 6.18, so the pre-6.18 branch is dead. Kept conditional in
 * spirit: when we bump past 6.18 and a new install pattern lands,
 * this is where it goes.
 */
static inline void smoothfs_compat_set_dentry_ops(struct super_block *sb,
						  const struct dentry_operations *ops)
{
	set_default_d_op(sb, ops);
}

/* ---------- read_iter / write_iter dispatch ----------
 * 6.12+: vfs_iter_read / vfs_iter_write are the exported helpers.
 * Old call_read_iter / call_write_iter were inline helpers in fs.h
 * and were removed before our floor. Floor is 6.18, so vfs_iter_*
 * is the only path.
 */
static inline ssize_t smoothfs_compat_read_iter(struct file *lower,
						loff_t *ppos,
						struct iov_iter *to)
{
	return vfs_iter_read(lower, to, ppos, 0);
}

static inline ssize_t smoothfs_compat_write_iter(struct file *lower,
						 loff_t *ppos,
						 struct iov_iter *from)
{
	return vfs_iter_write(lower, from, ppos, 0);
}

/* ---------- lookup_one ----------
 * 6.18: lookup_one(idmap, qstr, parent)
 * pre-6.18: lookup_one_len(name, parent, len)
 *
 * IMPORTANT: lookup_one internally calls lookup_noperm_common which
 * OVERWRITES qname->hash via full_name_hash(base, ...). If the caller
 * passes the qstr that lives inside a dentry (e.g. &dentry->d_name),
 * that mutation corrupts the dentry's hash for the UPPER parent and
 * the VFS's __d_lookup_rcu will never find the cached dentry on the
 * next path walk — every stat would then trigger a full lookup and
 * the dcache bucket accumulates aliases. Work around by copying into
 * a local qstr; lookup_one may scribble on that local copy without
 * touching the caller's dentry.
 */
static inline struct dentry *
smoothfs_compat_lookup(struct mnt_idmap *idmap, const struct qstr *name,
		       struct dentry *parent)
{
	struct qstr local = { .name = name->name, .len = name->len,
			      .hash = name->hash };

	return lookup_one(idmap, &local, parent);
}

static inline int
smoothfs_compat_create(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
	return vfs_create(idmap, dentry, mode, NULL);
#else
	return vfs_create(idmap, dir, dentry, mode, excl);
#endif
}

/* ---------- vfs_mkdir return type / args ----------
 * 6.19 (Debian 13 backports): returns struct dentry * and takes
 * a delegated_inode * out-param.
 * 6.18 baseline: returns struct dentry * with no delegated inode.
 *
 * Helper does the now-canonical "may-replace-dentry" dance: returns
 * the dentry the caller should use (either input or replacement),
 * or ERR_PTR on failure. Callers always dput their original on
 * replacement.
 */
static inline struct dentry *
smoothfs_compat_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
	struct delegated_inode delegated = { 0 };
	struct dentry *new_d = vfs_mkdir(idmap, dir, dentry, mode, &delegated);
#else
	struct dentry *new_d = vfs_mkdir(idmap, dir, dentry, mode);
#endif

	if (IS_ERR(new_d))
		return new_d;
	if (new_d && new_d != dentry)
		return new_d;
	return dentry;
}

static inline int
smoothfs_compat_mknod(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode, dev_t rdev)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
	return vfs_mknod(idmap, dir, dentry, mode, rdev, NULL);
#else
	return vfs_mknod(idmap, dir, dentry, mode, rdev);
#endif
}

static inline int
smoothfs_compat_symlink(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, const char *symname)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
	return vfs_symlink(idmap, dir, dentry, symname, NULL);
#else
	return vfs_symlink(idmap, dir, dentry, symname);
#endif
}

static inline int
smoothfs_compat_rmdir(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
	return vfs_rmdir(idmap, dir, dentry, NULL);
#else
	return vfs_rmdir(idmap, dir, dentry);
#endif
}

static inline struct dentry *
smoothfs_compat_start_removing(struct dentry *parent, struct dentry *child,
			       struct inode **locked_dir)
{
	struct inode *dir = d_inode(parent);

	*locked_dir = dir;
	inode_lock(dir);
	if (child->d_parent != parent) {
		inode_unlock(dir);
		*locked_dir = NULL;
		return ERR_PTR(-EINVAL);
	}
	dget(child);
	return child;
}

static inline void
smoothfs_compat_end_removing(struct dentry *child, struct inode *locked_dir)
{
	if (!IS_ERR(child))
		dput(child);
	if (locked_dir)
		inode_unlock(locked_dir);
}

/* ---------- d_revalidate signature ----------
 * 6.18: int (*d_revalidate)(struct inode *parent_dir,
 *                            const struct qstr *name,
 *                            struct dentry *dentry,
 *                            unsigned int flags)
 * pre-6.18: int (*d_revalidate)(struct dentry *, unsigned int)
 *
 * Callers stash the new-style signature; this helper translates
 * for any lower whose ops still use the old-style. With floor 6.18,
 * lowers also have the new shape, so this is just a forward.
 */
static inline int
smoothfs_compat_lower_d_revalidate(const struct dentry_operations *lower_ops,
				   struct inode *lower_dir,
				   const struct qstr *name,
				   struct dentry *lower,
				   unsigned int flags)
{
	if (!lower_ops || !lower_ops->d_revalidate)
		return 1;
	return lower_ops->d_revalidate(lower_dir, name, lower, flags);
}

/* ---------- renamedata ----------
 * 6.18: { mnt_idmap, old_parent, old_dentry, new_parent, new_dentry,
 *         delegated_inode, flags }
 * pre-6.18: { old_mnt_idmap, old_dir, old_dentry, new_mnt_idmap,
 *              new_dir, new_dentry, ... }
 *
 * Helper assembles a renamedata for the current kernel.
 */
static inline void
smoothfs_compat_init_renamedata(struct renamedata *rd,
				struct mnt_idmap *idmap,
				struct dentry *old_parent,
				struct dentry *old_dentry,
				struct dentry *new_parent,
				struct dentry *new_dentry,
				unsigned int flags)
{
	memset(rd, 0, sizeof(*rd));
	rd->mnt_idmap  = idmap;
	rd->old_parent = old_parent;
	rd->old_dentry = old_dentry;
	rd->new_parent = new_parent;
	rd->new_dentry = new_dentry;
	rd->flags      = flags;
}

/* ---------- inode state access ----------
 * 6.19 wraps inode->i_state in struct inode_state_flags and expects
 * filesystem code to use inode_state_read_once().
 * 6.18 and earlier still exposed a raw bitmask field.
 */
static inline bool smoothfs_compat_inode_is_new(struct inode *inode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
	return inode_state_read_once(inode) & I_NEW;
#else
	return inode->i_state & I_NEW;
#endif
}

#endif /* _SMOOTHFS_COMPAT_H */

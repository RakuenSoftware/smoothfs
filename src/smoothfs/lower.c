// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - lower-file open/release + per-fd cutover reissue.
 *
 * Each smoothfs file struct's private_data points to smoothfs_file_info.
 * The info carries the open lower_file, the cutover_gen at the time we
 * opened it, and the credentials/flags needed to reopen against a new
 * lower after cutover.
 *
 * Reissue is lazy: smoothfs_lower_file(file) checks whether the inode
 * has advanced cutover_gen since this fd was opened, and if so drops
 * the old lower and reopens against the current lower_path. This is
 * what lets MOVE_PLAN succeed even when fds are open against the file.
 */

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/path.h>
#include <linux/fcntl.h>

#include "smoothfs.h"

static int reissue_perm_mask(fmode_t flags)
{
	int mask = 0;

	switch (flags & O_ACCMODE) {
	case O_RDONLY:
		mask = MAY_READ;
		break;
	case O_WRONLY:
		mask = MAY_WRITE;
		break;
	case O_RDWR:
		mask = MAY_READ | MAY_WRITE;
		break;
	}
	if (flags & O_APPEND)
		mask |= MAY_APPEND;
	return mask;
}

static struct file *open_lower_now(struct inode *inode, fmode_t flags,
				   const struct cred *cred)
{
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct path lower_path;
	const struct cred *old_cred;
	struct file *f;
	int err;

	inode_lock_shared(inode);
	lower_path = si->lower_path;
	path_get(&lower_path);
	inode_unlock_shared(inode);

	/* dentry_open is a low-level helper that does NOT re-check
	 * permissions on the path it's handed; the open-syscall fastpath
	 * relies on path-walk having already done that. After a cutover to
	 * a destination tier whose lower file has more restrictive perms
	 * than the source, an existing fd must NOT be silently re-attached
	 * to the destination if the saved creds couldn't open it from
	 * scratch. inode_permission against the saved cred is the smallest
	 * change that re-applies the access check that VFS would otherwise
	 * have performed at open(2) time. */
	old_cred = override_creds(cred);
	err = inode_permission(&nop_mnt_idmap, d_inode(lower_path.dentry),
			       reissue_perm_mask(flags));
	revert_creds(old_cred);
	if (err) {
		path_put(&lower_path);
		return ERR_PTR(err);
	}

	f = dentry_open(&lower_path,
			flags & ~(O_CREAT | O_EXCL | O_NOCTTY), cred);
	path_put(&lower_path);
	return f;
}

int smoothfs_open_lower(struct file *file, struct inode *inode)
{
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct smoothfs_file_info *fi;
	struct file *lower;

	fi = kmalloc(sizeof(*fi), GFP_KERNEL);
	if (!fi)
		return -ENOMEM;

	mutex_init(&fi->reissue_lock);
	fi->open_flags = file->f_flags;
	fi->open_cred  = get_cred(current_cred());

	lower = open_lower_now(inode, fi->open_flags, fi->open_cred);
	if (IS_ERR(lower)) {
		put_cred(fi->open_cred);
		kfree(fi);
		return PTR_ERR(lower);
	}
	fi->lower_file = lower;
	fi->lower_gen  = READ_ONCE(si->cutover_gen);

	file->private_data = fi;
	atomic_inc(&si->open_count);
	return 0;
}

int smoothfs_release_lower(struct file *file)
{
	struct smoothfs_file_info *fi = file->private_data;

	if (!fi)
		return 0;
	if (fi->lower_file)
		fput(fi->lower_file);
	if (fi->open_cred)
		put_cred(fi->open_cred);
	mutex_destroy(&fi->reissue_lock);
	kfree(fi);
	file->private_data = NULL;
	return 0;
}

int smoothfs_reissue_lower(struct file *file)
{
	struct smoothfs_file_info *fi = file->private_data;
	struct smoothfs_inode_info *si = SMOOTHFS_I(file_inode(file));
	u32 cur_gen;
	struct file *new_lower;

	if (!fi)
		return -EBADF;

	cur_gen = READ_ONCE(si->cutover_gen);
	if (likely(fi->lower_gen == cur_gen))
		return 0;

	mutex_lock(&fi->reissue_lock);
	cur_gen = READ_ONCE(si->cutover_gen);
	if (fi->lower_gen == cur_gen) {
		mutex_unlock(&fi->reissue_lock);
		return 0;
	}

	new_lower = open_lower_now(&si->vfs_inode, fi->open_flags, fi->open_cred);
	if (IS_ERR(new_lower)) {
		int err = PTR_ERR(new_lower);

		mutex_unlock(&fi->reissue_lock);
		return err;
	}

	fput(fi->lower_file);
	fi->lower_file = new_lower;
	fi->lower_gen  = cur_gen;
	mutex_unlock(&fi->reissue_lock);
	return 0;
}

/*
 * Return the current lower file for `file`, lazily reopening against
 * the inode's new lower_path if a cutover has happened since this fd
 * was opened. The caller owns the returned reference's lifetime via
 * the smoothfs_file_info struct — do NOT fput.
 */
struct file *smoothfs_lower_file(struct file *file)
{
	struct smoothfs_file_info *fi = file->private_data;
	int err;

	if (!fi)
		return ERR_PTR(-EBADF);
	err = smoothfs_reissue_lower(file);
	if (err)
		return ERR_PTR(err);
	return fi->lower_file;
}

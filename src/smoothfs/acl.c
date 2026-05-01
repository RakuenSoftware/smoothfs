// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - POSIX ACL passthrough.
 *
 * Per Phase 0 §POSIX semantics — ACLs: passed through to the lower; the
 * lower must support POSIX ACLs (capability-probed at mount via
 * SMOOTHFS_CAP_POSIX_ACL).
 */

#include <linux/fs.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>

#include "smoothfs.h"

#ifdef CONFIG_FS_POSIX_ACL

struct posix_acl *smoothfs_get_inode_acl(struct inode *inode, int type, bool rcu)
{
	struct path *lower_path = smoothfs_lower_path(inode);
	struct inode *lower = d_inode(lower_path->dentry);

	if (!IS_POSIXACL(lower))
		return NULL;
	if (rcu)
		return get_cached_acl_rcu(lower, type);
	return get_inode_acl(lower, type);
}

int smoothfs_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		     struct posix_acl *acl, int type)
{
	struct dentry *lower = smoothfs_lower_dentry(dentry);
	int err;

	/* vfs_set_acl already takes inode_lock(d_inode(dentry)) on its
	 * argument internally; an explicit inode_lock(d_inode(lower))
	 * here meant we'd recursively try to take the same lower-inode
	 * rwsem in write mode and self-deadlock. nfsd hits this while
	 * cthon04 / general-suite chmod traffic flows through
	 * SETATTR -> set_posix_acl -> smoothfs_set_acl; the kernel
	 * hung-task watchdog reports
	 *   "task nfsd:NNN <writer> blocked on an rw-semaphore
	 *    likely owned by task nfsd:NNN <writer>"
	 * with smoothfs_set_acl in the stack, just like the
	 * smoothfs_setattr deadlock fixed earlier. Let vfs_set_acl
	 * own the lower locking. */
	err = vfs_set_acl(idmap, lower, posix_acl_xattr_name(type), acl);
	if (!err)
		smoothfs_copy_attrs(d_inode(dentry), d_inode(lower));
	return err;
}

#endif /* CONFIG_FS_POSIX_ACL */

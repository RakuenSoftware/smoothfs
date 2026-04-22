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

	inode_lock(d_inode(lower));
	err = vfs_set_acl(idmap, lower, posix_acl_xattr_name(type), acl);
	inode_unlock(d_inode(lower));
	if (!err)
		smoothfs_copy_attrs(d_inode(dentry), d_inode(lower));
	return err;
}

#endif /* CONFIG_FS_POSIX_ACL */

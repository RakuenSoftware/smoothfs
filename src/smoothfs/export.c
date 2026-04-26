// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - NFS export_operations.
 *
 * Phase 4.1: non-connectable encode_fh / fh_to_dentry.
 * Phase 4.5: connectable encoding + fh_to_parent + get_parent.
 *
 * Wire formats (Phase 0 contract §0.7 + §Phase 4 addenda):
 *   FILEID_SMOOTHFS_OID (0x53, 24 bytes):
 *     fsid (4) | object_id (16) | gen (4)
 *   FILEID_SMOOTHFS_OID_CONNECTABLE (0x54, 40 bytes):
 *     fsid (4) | object_id (16) | gen (4) | parent_object_id (16)
 *
 *   gen is hard-wired to 0 in Phase 4 and ignored on decode. File
 *   resolution (fh_to_dentry) uses only the object_id; the connectable
 *   variant just carries extra trailing bytes that fh_to_parent reads.
 *
 * Resolution path: decode → verify fsid matches sbi->fsid →
 * smoothfs_lookup_oid (rhashtable, RCU-safe) → igrab → d_obtain_alias.
 * Anonymous dentries returned by d_obtain_alias come back without
 * d_fsdata; populate from si->lower_path so the rest of the smoothfs
 * dentry surface (smoothfs_lower_dentry callers) doesn't NULL-deref.
 *
 * get_parent walks the lower dentry chain and re-igets the smoothfs
 * inode for the lower parent. This is the runtime-reverse counterpart
 * to fh_to_parent and is used by nfsd when encoding a connectable
 * handle for a dentry it already holds.
 */

#include <linux/exportfs.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/string.h>

#include "smoothfs.h"

#define SMOOTHFS_FH_BYTES               24
#define SMOOTHFS_FH_DWORDS              (SMOOTHFS_FH_BYTES / sizeof(u32))
#define SMOOTHFS_FH_CONNECTABLE_BYTES   40
#define SMOOTHFS_FH_CONNECTABLE_DWORDS  (SMOOTHFS_FH_CONNECTABLE_BYTES / sizeof(u32))

static const u8 zero_oid[SMOOTHFS_OID_LEN];

static int smoothfs_encode_fh(struct inode *inode, u32 *fh, int *max_len,
			       struct inode *parent)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(inode->i_sb);
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	u32 fsid = sbi->fsid;
	u32 gen = 0;

	if (parent) {
		struct smoothfs_inode_info *psi = SMOOTHFS_I(parent);

		if (*max_len < SMOOTHFS_FH_CONNECTABLE_DWORDS) {
			*max_len = SMOOTHFS_FH_CONNECTABLE_DWORDS;
			return FILEID_INVALID;
		}

		memcpy(&fh[0], &fsid, sizeof(fsid));
		memcpy(&fh[1], si->oid, SMOOTHFS_OID_LEN);
		memcpy(&fh[5], &gen, sizeof(gen));
		memcpy(&fh[6], psi->oid, SMOOTHFS_OID_LEN);

		*max_len = SMOOTHFS_FH_CONNECTABLE_DWORDS;
		return FILEID_SMOOTHFS_OID_CONNECTABLE;
	}

	if (*max_len < SMOOTHFS_FH_DWORDS) {
		*max_len = SMOOTHFS_FH_DWORDS;
		return FILEID_INVALID;
	}

	memcpy(&fh[0], &fsid, sizeof(fsid));
	memcpy(&fh[1], si->oid, SMOOTHFS_OID_LEN);
	memcpy(&fh[5], &gen, sizeof(gen));

	*max_len = SMOOTHFS_FH_DWORDS;
	return FILEID_SMOOTHFS_OID;
}

/* Resolve a 16-byte OID to a smoothfs dentry. Shared by fh_to_dentry
 * and fh_to_parent. Returns ERR_PTR(-ESTALE) for missing or mid-eviction
 * inodes. On the fresh-anonymous branch, d_fsdata is populated from
 * si->lower_path so dentry operations don't later NULL-deref. */
static struct dentry *smoothfs_resolve_oid(struct super_block *sb,
					    const u8 *oid)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(sb);
	struct smoothfs_inode_info *si;
	struct inode *inode;
	struct dentry *dentry;

	if (memcmp(oid, zero_oid, SMOOTHFS_OID_LEN) == 0) {
		if (!sb->s_root)
			return ERR_PTR(-ESTALE);
		return dget(sb->s_root);
	}

	rcu_read_lock();
	si = smoothfs_lookup_oid(sbi, oid);
	inode = si ? igrab(&si->vfs_inode) : NULL;
	rcu_read_unlock();

	if (!inode)
		return ERR_PTR(-ESTALE);

	dentry = d_obtain_alias(inode);
	if (IS_ERR(dentry))
		return dentry;

	if (!dentry->d_fsdata) {
		si = SMOOTHFS_I(d_inode(dentry));
		if (si->lower_path.dentry)
			smoothfs_set_lower_dentry(dentry, si->lower_path.dentry);
	}

	return dentry;
}

static struct dentry *smoothfs_fh_to_dentry(struct super_block *sb,
					     struct fid *fid,
					     int fh_len, int fh_type)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(sb);
	u32 fsid;
	u8 oid[SMOOTHFS_OID_LEN];
	int required;

	if (fh_type == FILEID_SMOOTHFS_OID)
		required = SMOOTHFS_FH_DWORDS;
	else if (fh_type == FILEID_SMOOTHFS_OID_CONNECTABLE)
		required = SMOOTHFS_FH_CONNECTABLE_DWORDS;
	else
		return ERR_PTR(-EINVAL);

	if (fh_len < required)
		return ERR_PTR(-ESTALE);

	memcpy(&fsid, &fid->raw[0], sizeof(fsid));
	memcpy(oid, &fid->raw[1], SMOOTHFS_OID_LEN);
	/* fid->raw[5] is gen — ignored in Phase 4 per contract addendum.
	 * fid->raw[6..9] is parent_oid in the connectable form — read only
	 * by fh_to_parent. */

	if (fsid != sbi->fsid)
		return ERR_PTR(-ESTALE);

	return smoothfs_resolve_oid(sb, oid);
}

static struct dentry *smoothfs_fh_to_parent(struct super_block *sb,
					     struct fid *fid,
					     int fh_len, int fh_type)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(sb);
	u32 fsid;
	u8 parent_oid[SMOOTHFS_OID_LEN];

	if (fh_type != FILEID_SMOOTHFS_OID_CONNECTABLE)
		return ERR_PTR(-EINVAL);
	if (fh_len < SMOOTHFS_FH_CONNECTABLE_DWORDS)
		return ERR_PTR(-ESTALE);

	memcpy(&fsid, &fid->raw[0], sizeof(fsid));
	memcpy(parent_oid, &fid->raw[6], SMOOTHFS_OID_LEN);

	if (fsid != sbi->fsid)
		return ERR_PTR(-ESTALE);

	return smoothfs_resolve_oid(sb, parent_oid);
}

static struct dentry *smoothfs_get_parent(struct dentry *child)
{
	struct super_block *sb = child->d_sb;
	struct dentry *lower = smoothfs_lower_dentry(child);
	struct dentry *lower_parent;
	struct path lower_path;
	struct inode *inode;
	struct dentry *parent;

	if (!lower)
		return ERR_PTR(-ESTALE);

	lower_parent = dget_parent(lower);
	if (!lower_parent || d_really_is_negative(lower_parent)) {
		dput(lower_parent);
		return ERR_PTR(-ESTALE);
	}

	/* Root's lower sits at the pool mount point; its lower_parent is
	 * outside our namespace. Short-circuit to sb->s_root so callers
	 * never chase above the smoothfs root. */
	if (lower_parent == SMOOTHFS_I(d_inode(sb->s_root))->lower_path.dentry) {
		dput(lower_parent);
		return dget(sb->s_root);
	}

	lower_path.mnt = SMOOTHFS_I(d_inode(child))->lower_path.mnt;
	lower_path.dentry = lower_parent;
	mntget(lower_path.mnt);

	inode = smoothfs_iget(sb, &lower_path, false, false);
	path_put(&lower_path);
	dput(lower_parent);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	parent = d_obtain_alias(inode);
	if (IS_ERR(parent))
		return parent;

	if (!parent->d_fsdata) {
		struct smoothfs_inode_info *psi = SMOOTHFS_I(d_inode(parent));

		if (psi->lower_path.dentry)
			smoothfs_set_lower_dentry(parent, psi->lower_path.dentry);
	}

	return parent;
}

const struct export_operations smoothfs_export_ops = {
	.encode_fh    = smoothfs_encode_fh,
	.fh_to_dentry = smoothfs_fh_to_dentry,
	.fh_to_parent = smoothfs_fh_to_parent,
	.get_parent   = smoothfs_get_parent,
};

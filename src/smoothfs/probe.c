// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - lower-fs capability probing.
 *
 * Phase 1 implements the structural side: at mount we record what the
 * lower advertises and refuse to mount if any required bit (per Phase 0
 * §0.6) is missing. Heuristic detection only — real round-trip probes
 * (writing and reading back probe xattrs, etc.) land in Phase 1.5.
 *
 * The heuristic is conservative: known lower fs types get the required
 * bitset plus a small set of optional capabilities we expect from that
 * class. Anything outside the explicit compatibility matrix still fails
 * closed.
 */

#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/string.h>

#include "smoothfs.h"

#ifndef ZFS_SUPER_MAGIC
#define ZFS_SUPER_MAGIC 0x2fc12fc1
#endif

static u32 smoothfs_caps_for_magic(unsigned long magic)
{
	switch (magic) {
	case XFS_SUPER_MAGIC:
		return SMOOTHFS_CAPS_REQUIRED |
		       SMOOTHFS_CAP_QUOTA_USER      |
		       SMOOTHFS_CAP_QUOTA_PROJECT   |
		       SMOOTHFS_CAP_REFLINK         |
		       SMOOTHFS_CAP_COPY_FILE_RANGE |
		       SMOOTHFS_CAP_FSCRYPT;
	case ZFS_SUPER_MAGIC:
		return SMOOTHFS_CAPS_REQUIRED |
		       SMOOTHFS_CAP_QUOTA_USER      |
		       SMOOTHFS_CAP_COPY_FILE_RANGE;
	case EXT4_SUPER_MAGIC:
		return SMOOTHFS_CAPS_REQUIRED |
		       SMOOTHFS_CAP_QUOTA_USER      |
		       SMOOTHFS_CAP_QUOTA_PROJECT   |
		       SMOOTHFS_CAP_COPY_FILE_RANGE |
		       SMOOTHFS_CAP_FSCRYPT;
	case BTRFS_SUPER_MAGIC:
		return SMOOTHFS_CAPS_REQUIRED |
		       SMOOTHFS_CAP_REFLINK         |
		       SMOOTHFS_CAP_COPY_FILE_RANGE |
		       SMOOTHFS_CAP_FSCRYPT;
	default:
		return 0;
	}
}

int smoothfs_probe_capabilities(struct smoothfs_tier *tier)
{
	struct super_block *sb = tier->lower_path.dentry->d_sb;
	const char *name = sb->s_type ? sb->s_type->name : "?";
	u32 caps;

	caps = smoothfs_caps_for_magic(sb->s_magic);
	if (!caps) {
		pr_warn("smoothfs: lower fs '%s' (magic 0x%lx) is not in "
			"the compatibility matrix (xfs, zfs, ext4, btrfs)\n",
			name, sb->s_magic);
	}

	tier->caps = caps;
	return 0;
}

bool smoothfs_lower_has_revalidate(const struct smoothfs_tier *tier)
{
	struct dentry *root = tier->lower_path.dentry;

	/* DCACHE_OP_REVALIDATE is the VFS-level flag set when the sb's
	 * default d_ops (or a dentry's own ops) include d_revalidate.
	 * xfs, ext4, btrfs, zfs all leave this clear — no per-fs dentry
	 * revalidation. Future compat lowers (fuse, nfs-as-lower, etc.)
	 * would set it and force smoothfs_d_revalidate into the slow
	 * path. */
	return !!(root->d_flags & DCACHE_OP_REVALIDATE);
}

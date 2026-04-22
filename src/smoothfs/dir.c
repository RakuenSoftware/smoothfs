// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - directory file_operations.
 *
 * Phase 1: iterate_shared forwards to the lower; Phase 1 does not yet
 * unify entries across multiple lowers (§Directories in the parent
 * proposal — child objects materialise per-tier lazily and Phase 2
 * reconciles).
 */

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/dcache.h>

#include "smoothfs.h"

static int smoothfs_opendir(struct inode *inode, struct file *file)
{
	return smoothfs_open_lower(file, inode);
}

static int smoothfs_releasedir(struct inode *inode, struct file *file)
{
	return smoothfs_release_lower(file);
}

static int smoothfs_iterate_shared(struct file *file, struct dir_context *ctx)
{
	struct file *lower = smoothfs_lower_file(file);
	int err;

	if (!lower)
		return -EBADF;

	err = iterate_dir(lower, ctx);
	file->f_pos = lower->f_pos;
	return err;
}

static loff_t smoothfs_dir_llseek(struct file *file, loff_t offset, int whence)
{
	struct file *lower = smoothfs_lower_file(file);
	loff_t ret;

	if (lower->f_op && lower->f_op->llseek)
		ret = lower->f_op->llseek(lower, offset, whence);
	else
		ret = generic_file_llseek(lower, offset, whence);
	if (ret >= 0)
		file->f_pos = lower->f_pos;
	return ret;
}

const struct file_operations smoothfs_dir_ops = {
	.owner          = THIS_MODULE,
	.open           = smoothfs_opendir,
	.release        = smoothfs_releasedir,
	.iterate_shared = smoothfs_iterate_shared,
	.llseek         = smoothfs_dir_llseek,
	.read           = generic_read_dir,
};

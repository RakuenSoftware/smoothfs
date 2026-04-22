// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - regular-file file_operations and address_space_operations.
 *
 * Phase 1: passthrough read/write/fsync/mmap/llseek/splice/fallocate to
 * the lower file. Phase 2 will inject heat sampling at read/write
 * boundaries; the per-inode counters are already in place.
 */

#include <linux/fs.h>
#include <linux/uio.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/atomic.h>
#include <linux/uaccess.h>
#include <linux/timekeeping.h>
#include <linux/falloc.h>
#include <linux/splice.h>
#include <linux/file.h>
#include <linux/fdtable.h>

#include "smoothfs.h"

static int smoothfs_open(struct inode *inode, struct file *file)
{
	struct smoothfs_file_info *fi;
	int ret;

	ret = smoothfs_open_lower(file, inode);
	if (ret)
		return ret;

	/*
	 * Phase 6.0 — O_DIRECT conformance. do_dentry_open()'s O_DIRECT
	 * gate (fs/open.c) refuses the open unless the upper file has
	 * FMODE_CAN_ODIRECT set. smoothfs owns no pages and forwards
	 * read_iter/write_iter through vfs_iter_{read,write} on the
	 * lower, which handles IOCB_DIRECT natively on every lower we
	 * support (xfs, ext4, btrfs, zfs). Mirror the lower's capability
	 * onto our file so LIO's fileio backend (and any other O_DIRECT
	 * consumer) can open LUN backing files on a smoothfs mount.
	 */
	fi = file->private_data;
	if (fi && fi->lower_file &&
	    (fi->lower_file->f_mode & FMODE_CAN_ODIRECT))
		file->f_mode |= FMODE_CAN_ODIRECT;

	return 0;
}

static int smoothfs_release(struct inode *inode, struct file *file)
{
	atomic_dec(&SMOOTHFS_I(inode)->open_count);
	return smoothfs_release_lower(file);
}

static ssize_t smoothfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *lower = smoothfs_lower_file(iocb->ki_filp);
	ssize_t ret;

	ret = smoothfs_compat_read_iter(lower, &iocb->ki_pos, to);
	if (ret > 0) {
		struct smoothfs_inode_info *si =
			SMOOTHFS_I(file_inode(iocb->ki_filp));
		atomic64_add(ret, &si->read_bytes);
		si->last_access_ns = ktime_get_real_ns();
	}
	return ret;
}

static ssize_t smoothfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(inode->i_sb);
	struct file *lower;
	ssize_t ret;
	int srcu_idx;

	/*
	 * SRCU-based cutover barrier. Writers enter a read-side critical
	 * section; cutover calls synchronize_srcu() after flipping the
	 * movement state to drain in-flight writes. The re-check inside
	 * the read section closes the race where cutover sets state
	 * between our first check and srcu_read_lock.
	 *
	 * Replaces the atomic_inc/dec + wait_event_interruptible on
	 * inflight_writes that every write paid in steady state. SRCU
	 * read-lock/unlock is per-CPU and sleep-safe (unlike plain RCU),
	 * so vfs_iter_write can block on page IO inside it.
	 */
again:
	srcu_idx = srcu_read_lock(&sbi->cutover_srcu);
	if (unlikely(READ_ONCE(si->movement_state) ==
		     SMOOTHFS_MS_CUTOVER_IN_PROGRESS)) {
		int err;

		srcu_read_unlock(&sbi->cutover_srcu, srcu_idx);
		err = wait_event_interruptible(si->cutover_wq,
			READ_ONCE(si->movement_state) !=
			SMOOTHFS_MS_CUTOVER_IN_PROGRESS);
		if (err)
			return err;
		goto again;
	}

	lower = smoothfs_lower_file(iocb->ki_filp);
	ret = smoothfs_compat_write_iter(lower, &iocb->ki_pos, from);
	srcu_read_unlock(&sbi->cutover_srcu, srcu_idx);

	if (ret > 0) {
		struct inode *upper = file_inode(iocb->ki_filp);
		atomic64_add(ret, &si->write_bytes);
		si->last_access_ns = ktime_get_real_ns();
		/* getattr already reads through to the lower via
		 * vfs_getattr_nosec, but nfsd's NFSv4.2 READ_PLUS path
		 * consults file_inode(file)->i_size DIRECTLY — without
		 * this i_size_write the smoothfs inode reports zero size
		 * even when the lower has the data, and READ_PLUS
		 * decoders treat the whole file as a hole and return
		 * zeros. Cheap one-cache-line write; the rest of
		 * smoothfs_copy_attrs (mode/uid/gid/times) stays out of
		 * the write hot path since those don't change on write. */
		i_size_write(upper, i_size_read(file_inode(lower)));
	}
	return ret;
}

static int smoothfs_fsync(struct file *file, loff_t start, loff_t end,
			  int datasync)
{
	struct file *lower = smoothfs_lower_file(file);

	return vfs_fsync_range(lower, start, end, datasync);
}

/*
 * mmap rebinds vma->vm_file to the lower and forwards through vfs_mmap.
 * Page faults then resolve via the lower's address_space (smoothfs's
 * a_ops are empty — it doesn't own pages). As a side-effect the VMA is
 * linked into the lower's i_mmap, so mapping_writably_mapped() on the
 * lower's mapping is the authoritative writable-shared-mmap gate that
 * movement.c consults.
 *
 * Phases 2.2 and 2.3 tried wrapping the lower's vm_ops to count writable
 * shared mmaps on our own inode, but the wrapper interacted badly with
 * lowers that use the newer mmap_prepare API and couldn't be made
 * fork-safe without heavy per-VMA state. Delegating to the lower's
 * mapping avoids both problems.
 */
static int smoothfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct smoothfs_inode_info *si = SMOOTHFS_I(file_inode(file));
	struct file *lower = smoothfs_lower_file(file);

	if (!can_mmap_file(lower))
		return -ENODEV;
	if (vma_is_shared_maywrite(vma) && READ_ONCE(si->mappings_quiesced))
		return -EBUSY;

	vma_set_file(vma, lower);
	return vfs_mmap(vma->vm_file, vma);
}

static loff_t smoothfs_llseek(struct file *file, loff_t offset, int whence)
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

static ssize_t smoothfs_splice_read(struct file *in, loff_t *ppos,
				    struct pipe_inode_info *pipe,
				    size_t len, unsigned int flags)
{
	struct file *lower = smoothfs_lower_file(in);

	if (!lower->f_op || !lower->f_op->splice_read)
		return -EINVAL;
	return lower->f_op->splice_read(lower, ppos, pipe, len, flags);
}

static ssize_t smoothfs_splice_write(struct pipe_inode_info *pipe,
				     struct file *out, loff_t *ppos,
				     size_t len, unsigned int flags)
{
	struct file *lower = smoothfs_lower_file(out);
	ssize_t ret;

	if (!lower->f_op || !lower->f_op->splice_write)
		return -EINVAL;
	ret = lower->f_op->splice_write(pipe, lower, ppos, len, flags);
	/* No smoothfs_copy_attrs: getattr reads through to the lower. */
	return ret;
}

static long smoothfs_fallocate(struct file *file, int mode, loff_t offset,
			       loff_t len)
{
	struct file *lower = smoothfs_lower_file(file);

	if (!lower->f_op || !lower->f_op->fallocate)
		return -EOPNOTSUPP;
	return vfs_fallocate(lower, mode, offset, len);
}

static struct file *smoothfs_remap_source_file(struct file *src)
{
	if (!src)
		return NULL;
	if (file_inode(src)->i_sb->s_magic == SMOOTHFS_MAGIC)
		return smoothfs_lower_file(src);
	return src;
}

static loff_t smoothfs_remap_file_range(struct file *file_in, loff_t pos_in,
					struct file *file_out, loff_t pos_out,
					loff_t len, unsigned int remap_flags)
{
	struct file *lower_in = smoothfs_remap_source_file(file_in);
	struct file *lower_out = smoothfs_remap_source_file(file_out);
	loff_t ret;

	if (!lower_in || !lower_out)
		return -EBADF;
	if (lower_out->f_op && lower_out->f_op->remap_file_range)
		ret = lower_out->f_op->remap_file_range(lower_in, pos_in,
							lower_out, pos_out,
							len, remap_flags);
	else if (remap_flags == 0)
		ret = vfs_clone_file_range(lower_in, pos_in, lower_out, pos_out,
					   len, 0);
	else
		return -EOPNOTSUPP;
	if (ret < 0)
		return ret;
	/* No smoothfs_copy_attrs: getattr reads through to the lower. */
	return ret;
}

static long smoothfs_clone_ioctl(struct file *file, unsigned long arg,
				 bool ranged)
{
	struct file *lower_out = smoothfs_lower_file(file);
	struct file *lower_in;
	struct file_clone_range range;
	struct file *src_file;
	loff_t ret, pos_in, pos_out, len;
	int lower_src_fd;
	int src_fd;

	if (ranged) {
		if (copy_from_user(&range, (void __user *)arg, sizeof(range)))
			return -EFAULT;
		src_fd = range.src_fd;
		pos_in = range.src_offset;
		pos_out = range.dest_offset;
		len = range.src_length;
	} else {
		src_fd = (int)arg;
		pos_in = 0;
		pos_out = 0;
		len = i_size_read(file_inode(file));
	}

	src_file = fget(src_fd);
	if (!src_file)
		return -EBADF;
	lower_in = smoothfs_remap_source_file(src_file);
	if (!lower_in) {
		fput(src_file);
		return -EBADF;
	}
	if (!ranged) {
		if (!lower_out->f_op || !lower_out->f_op->unlocked_ioctl) {
			fput(src_file);
			return -ENOTTY;
		}
		lower_src_fd = get_unused_fd_flags(O_CLOEXEC);
		if (lower_src_fd < 0) {
			fput(src_file);
			return lower_src_fd;
		}
		get_file(lower_in);
		fd_install(lower_src_fd, lower_in);
		ret = lower_out->f_op->unlocked_ioctl(lower_out, FICLONE,
						      lower_src_fd);
		close_fd(lower_src_fd);
		fput(src_file);
		if (ret < 0)
			return ret;
		/* No smoothfs_copy_attrs: getattr reads through to the lower. */
		return 0;
	}

	if (len == 0)
		len = i_size_read(file_inode(lower_in)) - pos_in;
	ret = vfs_clone_file_range(lower_in, pos_in, lower_out, pos_out, len, 0);
	fput(src_file);
	if (ret < 0)
		return ret;
	/* No smoothfs_copy_attrs: getattr reads through to the lower. */
	return 0;
}

static long smoothfs_unlocked_ioctl(struct file *file, unsigned int cmd,
				    unsigned long arg)
{
	struct file *lower = smoothfs_lower_file(file);

	switch (cmd) {
	case FICLONE:
		return smoothfs_clone_ioctl(file, arg, false);
	case FICLONERANGE:
		return smoothfs_clone_ioctl(file, arg, true);
	default:
		break;
	}
	if (!lower->f_op || !lower->f_op->unlocked_ioctl)
		return -ENOTTY;
	return lower->f_op->unlocked_ioctl(lower, cmd, arg);
}

#ifdef CONFIG_COMPAT
static long smoothfs_compat_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	struct file *lower = smoothfs_lower_file(file);

	if (!lower->f_op || !lower->f_op->compat_ioctl)
		return -ENOTTY;
	return lower->f_op->compat_ioctl(lower, cmd, arg);
}
#endif

const struct file_operations smoothfs_file_ops = {
	.owner          = THIS_MODULE,
	.open           = smoothfs_open,
	.release        = smoothfs_release,
	.read_iter      = smoothfs_read_iter,
	.write_iter     = smoothfs_write_iter,
	.fsync          = smoothfs_fsync,
	.mmap           = smoothfs_mmap,
	.llseek         = smoothfs_llseek,
	.splice_read    = smoothfs_splice_read,
	.splice_write   = smoothfs_splice_write,
	.fallocate      = smoothfs_fallocate,
	.remap_file_range = smoothfs_remap_file_range,
	.unlocked_ioctl = smoothfs_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = smoothfs_compat_ioctl,
#endif
	.flock          = NULL,  /* set in lock.c via override at mount */
};

/* address_space ops: page cache lives at the lower; smoothfs does not
 * own pages in Phase 1. read_iter/write_iter dispatch directly. */
const struct address_space_operations smoothfs_aops = {
};

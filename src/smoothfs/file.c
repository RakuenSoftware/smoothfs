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
#include <linux/slab.h>

#include "smoothfs.h"

#define SMOOTHFS_DEFAULT_FULL_PCT 98
static bool smoothfs_tier_near_enospc(struct smoothfs_sb_info *sbi, u8 tier)
{
	struct kstatfs st;
	u8 full_pct = READ_ONCE(sbi->write_staging_full_pct);
	int err;

	if (tier >= sbi->ntiers)
		return true;
	err = vfs_statfs(&sbi->tiers[tier].lower_path, &st);
	if (err || st.f_blocks == 0)
		return true;
	if (full_pct == 0 || full_pct > 100)
		full_pct = SMOOTHFS_DEFAULT_FULL_PCT;
	return (st.f_blocks - st.f_bavail) * 100 >= st.f_blocks * full_pct;
}

static bool smoothfs_should_range_stage_write(struct inode *inode,
					      struct file *lower,
					      struct kiocb *iocb)
{
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(inode->i_sb);
	u8 tier;

	if (!READ_ONCE(sbi->write_staging_enabled))
		return false;
	if (!S_ISREG(inode->i_mode))
		return false;
	if (iocb->ki_flags & IOCB_DIRECT)
		return false;
	if (si->pin_state != SMOOTHFS_PIN_NONE)
		return false;
	if (!lower)
		return false;
	tier = smoothfs_tier_of(sbi, lower->f_path.mnt);
	if (tier >= sbi->ntiers || tier == sbi->fastest_tier)
		return false;
	if (smoothfs_tier_near_enospc(sbi, sbi->fastest_tier))
		return false;
	return true;
}

static int smoothfs_range_stage_open_locked(struct inode *inode)
{
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(inode->i_sb);
	struct file *stage;
	char *name;

	if (si->range_staged_path.dentry)
		return 0;

	name = kasprintf(GFP_KERNEL, ".smoothfs/range-%*phN.stage",
			 SMOOTHFS_OID_LEN, si->oid);
	if (!name)
		return -ENOMEM;
	stage = file_open_root(&sbi->tiers[sbi->fastest_tier].lower_path,
			       name, O_RDWR | O_CREAT, 0600);
	kfree(name);
	if (IS_ERR(stage))
		return PTR_ERR(stage);

	si->range_staged_path = stage->f_path;
	path_get(&si->range_staged_path);
	fput(stage);
	return 0;
}

static void smoothfs_range_stage_record_locked(struct inode *inode,
					       loff_t start, loff_t end,
					       struct smoothfs_staged_range *new_range)
{
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct smoothfs_staged_range *range, *tmp;
	struct smoothfs_staged_range *insert_before = NULL;

	if (end <= start) {
		kfree(new_range);
		return;
	}

	list_for_each_entry_safe(range, tmp, &si->range_staged_ranges, link) {
		if (end < range->start) {
			insert_before = range;
			break;
		}
		if (start > range->end)
			continue;
		if (range->start < start)
			start = range->start;
		if (range->end > end)
			end = range->end;
		list_del(&range->link);
		kfree(range);
	}

	range = new_range;
	range->start = start;
	range->end = end;
	if (insert_before)
		list_add_tail(&range->link, &insert_before->link);
	else
		list_add_tail(&range->link, &si->range_staged_ranges);
}

static ssize_t smoothfs_range_stage_write(struct kiocb *iocb,
					  struct file *lower,
					  struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(inode->i_sb);
	struct file *stage;
	loff_t pos = iocb->ki_pos;
	struct smoothfs_staged_range *new_range;
	ssize_t ret;
	u8 source_tier;
	int err;

	source_tier = smoothfs_tier_of(sbi, lower->f_path.mnt);

	mutex_lock(&si->range_staging_lock);
	err = smoothfs_range_stage_open_locked(inode);
	if (err) {
		mutex_unlock(&si->range_staging_lock);
		return err;
	}
	stage = dentry_open(&si->range_staged_path, O_RDWR, current_cred());
	if (IS_ERR(stage)) {
		mutex_unlock(&si->range_staging_lock);
		return PTR_ERR(stage);
	}
	new_range = kmalloc(sizeof(*new_range), GFP_KERNEL);
	if (!new_range) {
		fput(stage);
		mutex_unlock(&si->range_staging_lock);
		return -ENOMEM;
	}

	ret = smoothfs_compat_write_iter(stage, &iocb->ki_pos, from);
	if (ret > 0) {
		loff_t end = pos + ret;

		smoothfs_range_stage_record_locked(inode, pos, end, new_range);
		new_range = NULL;
		WRITE_ONCE(si->range_staged, true);
		WRITE_ONCE(si->range_staged_source_tier, source_tier);
		if (end > i_size_read(inode))
			i_size_write(inode, end);
		smoothfs_write_staging_note_range_write(sbi, ret);
	}
	kfree(new_range);
	fput(stage);
	mutex_unlock(&si->range_staging_lock);
	return ret;
}

static bool smoothfs_range_overlaps(struct smoothfs_inode_info *si,
				    loff_t start, loff_t end)
{
	struct smoothfs_staged_range *range;

	list_for_each_entry(range, &si->range_staged_ranges, link)
		if (range->start < end && range->end > start)
			return true;
	return false;
}

static ssize_t smoothfs_range_stage_overlay_locked(struct inode *inode,
						   char *buf, loff_t pos,
						   size_t len)
{
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct smoothfs_staged_range *range;
	struct file *stage;
	ssize_t ret = 0;

	if (!smoothfs_range_overlaps(si, pos, pos + len))
		return 0;

	stage = dentry_open(&si->range_staged_path, O_RDONLY, current_cred());
	if (IS_ERR(stage))
		return PTR_ERR(stage);

	list_for_each_entry(range, &si->range_staged_ranges, link) {
		loff_t start, end, stage_pos;
		ssize_t n;

		if (range->start >= pos + len)
			break;
		if (range->end <= pos)
			continue;

		start = max(range->start, pos);
		end = min(range->end, pos + (loff_t)len);
		stage_pos = start;
		n = kernel_read(stage, buf + (size_t)(start - pos),
				(size_t)(end - start), &stage_pos);
		if (n < 0) {
			ret = n;
			break;
		}
	}
	fput(stage);
	return ret;
}

static ssize_t smoothfs_range_stage_read_iter(struct kiocb *iocb,
					      struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct file *lower = smoothfs_lower_file(iocb->ki_filp);
	ssize_t done = 0;
	loff_t size = i_size_read(inode);

	while (iov_iter_count(to) > 0 && iocb->ki_pos < size) {
		size_t want = min_t(size_t, iov_iter_count(to),
				    SMOOTHFS_RANGE_READ_CHUNK);
		size_t avail = min_t(loff_t, want, size - iocb->ki_pos);
		loff_t pos = iocb->ki_pos;
		char *buf;
		ssize_t n;

		buf = kzalloc(avail, GFP_KERNEL);
		if (!buf)
			return done ? done : -ENOMEM;

		n = kernel_read(lower, buf, avail, &pos);
		if (n < 0) {
			kfree(buf);
			return done ? done : n;
		}
		mutex_lock(&si->range_staging_lock);
		n = smoothfs_range_stage_overlay_locked(inode, buf,
							iocb->ki_pos, avail);
		mutex_unlock(&si->range_staging_lock);
		if (n < 0) {
			kfree(buf);
			return done ? done : n;
		}
		n = copy_to_iter(buf, avail, to);
		kfree(buf);
		if (n <= 0)
			break;
		iocb->ki_pos += n;
		done += n;
		if ((size_t)n < avail)
			break;
	}
	return done;
}

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
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);

	if (atomic_dec_and_test(&si->open_count))
		smoothfs_clear_write_reservation(SMOOTHFS_SB(inode->i_sb), si);
	return smoothfs_release_lower(file);
}

static ssize_t smoothfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *lower = smoothfs_lower_file(iocb->ki_filp);
	struct smoothfs_inode_info *si =
		SMOOTHFS_I(file_inode(iocb->ki_filp));
	ssize_t ret;

	if (READ_ONCE(si->range_staged) && (iocb->ki_flags & IOCB_DIRECT))
		return -EBUSY;
	if (READ_ONCE(si->range_staged))
		ret = smoothfs_range_stage_read_iter(iocb, to);
	else
		ret = smoothfs_compat_read_iter(lower, &iocb->ki_pos, to);
	if (ret > 0) {
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
	u8 tier;
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
	tier = smoothfs_tier_of(sbi, lower->f_path.mnt);
	if (READ_ONCE(si->range_staged) && (iocb->ki_flags & IOCB_DIRECT))
		return -EBUSY;
	if (tier < sbi->ntiers)
		atomic_inc(&sbi->tiers[tier].active_writes);
	smoothfs_clear_write_reservation(sbi, si);
	if (smoothfs_should_range_stage_write(inode, lower, iocb))
		ret = smoothfs_range_stage_write(iocb, lower, from);
	else
		ret = smoothfs_compat_write_iter(lower, &iocb->ki_pos, from);
	if (tier < sbi->ntiers)
		atomic_dec(&sbi->tiers[tier].active_writes);
	srcu_read_unlock(&sbi->cutover_srcu, srcu_idx);

	if (ret > 0) {
		struct inode *upper = file_inode(iocb->ki_filp);
		atomic64_add(ret, &si->write_bytes);
		if (!READ_ONCE(si->range_staged) &&
		    READ_ONCE(si->write_staged) &&
		    tier == sbi->fastest_tier)
			smoothfs_write_staging_note_write(sbi, ret);
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
		if (!READ_ONCE(si->range_staged))
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
	if (READ_ONCE(si->range_staged))
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

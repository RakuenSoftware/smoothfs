// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - flock/fcntl lock passthrough.
 *
 * Phase 0 §POSIX locks: flock(2), OFD locks, advisory fcntl(F_SETLK)
 * are held in the smoothfs inode. Phase 1 holds them at the lower
 * because there is no movement to survive yet; Phase 2 lifts the lock
 * state out of the lower so cutover preserves it.
 *
 * Mandatory locks: refused at mount-time per §0.4 (caller passes
 * MS_MANDLOCK; we set fs_flags to never allow it). This file does not
 * implement that path — the kernel deprecation handles it.
 */

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/filelock.h>

#include "smoothfs.h"

/* file_operations exposes flock(struct file*, int, struct file_lock*).
 * The actual hook is added to smoothfs_file_ops after the table is
 * defined in file.c — kept separate so future Phase 2 lifting is local. */

int smoothfs_flock(struct file *file, int cmd, struct file_lock *fl)
{
	struct file *lower = smoothfs_lower_file(file);

	if (lower->f_op && lower->f_op->flock)
		return lower->f_op->flock(lower, cmd, fl);
	return locks_lock_file_wait(file, fl);
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - movement state machine (Phase 2 §0.3).
 *
 * tierd drives transitions via netlink commands; the kernel enforces
 * concurrency and atomicity. Phase 2 implements the simplest correct
 * cutover: refuse movement while any fd is open or any writable shared
 * mapping exists; on cutover, atomically swap lower_path on the inode
 * and bump cutover_gen. Phase 2.1 lifts the open-fd restriction with
 * the per-fd reissue protocol from Phase 0 §0.4.
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/path.h>
#include <linux/fsnotify.h>

#include "smoothfs.h"

/* Look up an inode by object_id via the per-pool rhashtable. RCU-safe
 * O(1) lookup; used by netlink movement commands and by Phase 4 NFS
 * fh_to_dentry. Caller does not take a ref; the inode is pinned by
 * the caller's sb hold. Returns NULL if the map is not yet ready (a
 * very narrow window at fill_super before sb_register). */
struct smoothfs_inode_info *smoothfs_lookup_oid(struct smoothfs_sb_info *sbi,
						const u8 oid[SMOOTHFS_OID_LEN])
{
	if (!READ_ONCE(sbi->oid_map_ready))
		return NULL;

	return rhashtable_lookup_fast(&sbi->oid_map, oid,
				      smoothfs_oid_rht_params);
}

static bool smoothfs_can_move(struct smoothfs_inode_info *si, bool force)
{
	struct inode *inode = &si->vfs_inode;
	struct inode *lower_inode = d_inode(si->lower_path.dentry);

	/* force=true from userspace only bypasses the PIN_LEASE case —
	 * the Samba VFS module's lease pin is the one kind of pin the
	 * admin can knowingly override (by accepting that the SMB client
	 * will have to break its lease). Every other pin (HARDLINK, LUN,
	 * the heat-derived HOT/COLD) represents a correctness constraint
	 * the caller cannot argue with. */
	if (si->pin_state != SMOOTHFS_PIN_NONE) {
		if (!(force && si->pin_state == SMOOTHFS_PIN_LEASE))
			return false;
	}
	if (atomic_read(&si->nlink_observed) > 1)
		return false;
	/* Writable shared mmaps block planning (Phase 0 §0.4). Since
	 * smoothfs_mmap rebinds vma->vm_file to the lower, each VMA is
	 * linked into the lower's i_mmap and the kernel's own
	 * i_mmap_writable counter is the authoritative gate. Admin
	 * override via SMOOTHFS_CMD_REVOKE_MAPPINGS zaps the PTEs; the
	 * holder must munmap before the counter drops. */
	if (mapping_writably_mapped(lower_inode->i_mapping))
		return false;
	/* Phase 2.2: writer fds are allowed. Per-fd reissue handles
	 * read fds across cutover; cutover itself drains in-flight
	 * writes via the cutover_wq write-barrier in file.c, then
	 * tierd's mtime-stable check rejects the cutover if the source
	 * changed since copy. */
	if (!S_ISREG(inode->i_mode))
		return false;
	return true;
}

int smoothfs_movement_plan(struct smoothfs_sb_info *sbi,
			   const u8 oid[SMOOTHFS_OID_LEN],
			   u8 dest_tier, u64 transaction_seq, bool force)
{
	struct smoothfs_inode_info *si;
	struct inode *inode;
	int err = 0;

	if (sbi->quiesced)
		return -EAGAIN;
	if (dest_tier >= sbi->ntiers)
		return -EINVAL;

	si = smoothfs_lookup_oid(sbi, oid);
	if (!si)
		return -ENOENT;

	inode = &si->vfs_inode;
	inode_lock(inode);
	if (si->movement_state != SMOOTHFS_MS_PLACED) {
		err = -EBUSY;
		goto out;
	}
	if (dest_tier == si->current_tier) {
		err = -EALREADY;
		goto out;
	}
	if (!smoothfs_can_move(si, force)) {
		err = -EBUSY;
		goto out;
	}

	si->mappings_quiesced = false;
	si->intended_tier   = dest_tier;
	si->movement_state  = SMOOTHFS_MS_PLAN_ACCEPTED;
	si->transaction_seq = transaction_seq;

	/* Recoverable writeback: if this record is lost before the next
	 * drain, tierd's planner re-issues the plan on its next cycle. */
	smoothfs_placement_record(sbi, oid, SMOOTHFS_MS_PLAN_ACCEPTED,
				  si->current_tier, dest_tier, /*sync=*/false);
	smoothfs_netlink_emit_move_state(sbi, oid,
					 SMOOTHFS_MS_PLAN_ACCEPTED,
					 transaction_seq);
out:
	inode_unlock(inode);
	return err;
}

/*
 * Cutover: tierd has copied data to dest tier and verified it.
 * The kernel atomically:
 *   1. Resolves the dest dentry on the destination lower
 *   2. Swaps lower_path on the inode (replacing src with dest)
 *   3. Bumps cutover_gen
 *   4. Records the transition
 *   5. Re-reads attrs from dest
 *
 * Phase 2 requires the file to have no open fds for a clean swap.
 * tierd's worker waits/retries when EBUSY is returned.
 */
int smoothfs_movement_cutover(struct smoothfs_sb_info *sbi,
			      const u8 oid[SMOOTHFS_OID_LEN],
			      u64 transaction_seq)
{
	struct smoothfs_inode_info *si;
	struct inode *inode;
	struct dentry *src_dentry, *dest_dentry, *old_lower_dentry;
	struct vfsmount *old_lower_mnt;
	struct path dest_path;
	int err = 0;

	if (sbi->quiesced)
		return -EAGAIN;

	si = smoothfs_lookup_oid(sbi, oid);
	if (!si)
		return -ENOENT;
	if (si->intended_tier >= sbi->ntiers)
		return -EINVAL;

	inode = &si->vfs_inode;
	inode_lock(inode);

	if (si->movement_state != SMOOTHFS_MS_PLAN_ACCEPTED) {
		/* tierd may move us through copy_in_progress / copy_complete /
		 * copy_verified states via separate MOVE_STATE updates; for
		 * Phase 2 the cutover may be invoked from any non-terminal
		 * pre-cutover state. */
		switch (si->movement_state) {
		case SMOOTHFS_MS_PLAN_ACCEPTED:
		case SMOOTHFS_MS_DESTINATION_RESERVED:
		case SMOOTHFS_MS_COPY_IN_PROGRESS:
		case SMOOTHFS_MS_COPY_COMPLETE:
		case SMOOTHFS_MS_COPY_VERIFIED:
			break;
		default:
			err = -EBUSY;
			goto out_unlock;
		}
	}
	if (si->transaction_seq != transaction_seq) {
		err = -ESTALE;
		goto out_unlock;
	}
	if (mapping_writably_mapped(d_inode(si->lower_path.dentry)->i_mapping)) {
		err = -EBUSY;
		goto out_unlock;
	}

	/* Set CUTOVER_IN_PROGRESS so write_iter stalls new writes on
	 * cutover_wq, then DROP inode_lock and drain in-flight writes.
	 * Holding inode_lock across a sleeping wait would serialize
	 * every other inode op (lookup, getattr, …) for the duration
	 * of the wait. Re-acquire below and re-validate. */
	si->movement_state = SMOOTHFS_MS_CUTOVER_IN_PROGRESS;
	/* Copy-on-write recovery can rediscover source and destination from
	 * lower tiers, so this only kicks asynchronous placement writeback;
	 * it does not block cutover on lower-fs durability. */
	smoothfs_placement_record(sbi, oid, SMOOTHFS_MS_CUTOVER_IN_PROGRESS,
				  si->current_tier, si->intended_tier,
				  /*sync=*/true);
	inode_unlock(inode);

	/*
	 * Drain in-flight writes via SRCU. New writers entering after the
	 * state flip above will see CUTOVER_IN_PROGRESS and park on
	 * cutover_wq; already-in-flight writers are holding the SRCU read
	 * side and synchronize_srcu blocks until they all exit.
	 *
	 * synchronize_srcu is unbounded (unlike the prior
	 * wait_event_interruptible_timeout 5s), but cutover is never on a
	 * benchmark path and writers can't sit in vfs_iter_write longer
	 * than the lower fs takes to complete one iov. If the lower is
	 * stuck hard enough for this to matter, the whole pool is stuck.
	 */
	synchronize_srcu(&sbi->cutover_srcu);

	inode_lock(inode);
	if (si->movement_state != SMOOTHFS_MS_CUTOVER_IN_PROGRESS) {
		/* Another path raced us out of the cutover state while we
		 * slept in synchronize_srcu. Roll back. */
		si->movement_state = SMOOTHFS_MS_COPY_VERIFIED;
		wake_up_all(&si->cutover_wq);
		/* Informational rollback; tierd's planner will notice the
		 * state on its next cycle regardless of durability here. */
		smoothfs_placement_record(sbi, oid, SMOOTHFS_MS_COPY_VERIFIED,
					  si->current_tier, si->intended_tier,
					  /*sync=*/false);
		err = -EBUSY;
		goto out_unlock;
	}

	/* Look up the dest dentry on the destination lower. tierd is
	 * responsible for having pre-copied the file there. We use the
	 * source dentry's name as the dest name (rename on cutover not
	 * supported in Phase 2). */
	src_dentry = si->lower_path.dentry;
	dest_path = sbi->tiers[si->intended_tier].lower_path;

	inode_lock(d_inode(dest_path.dentry));
	dest_dentry = smoothfs_compat_lookup(&nop_mnt_idmap,
					     &src_dentry->d_name,
					     dest_path.dentry);
	inode_unlock(d_inode(dest_path.dentry));
	if (IS_ERR(dest_dentry)) {
		err = PTR_ERR(dest_dentry);
		goto out_fail;
	}
	if (d_really_is_negative(dest_dentry)) {
		dput(dest_dentry);
		err = -ENOENT;
		goto out_fail;
	}

	/* Swap lower_path: keep refs balanced. The smoothfs dentry's
	 * d_fsdata also points at the OLD lower dentry — update it too,
	 * otherwise smoothfs_d_release dputs a freed pointer at umount. */
	old_lower_dentry = si->lower_path.dentry;
	old_lower_mnt    = si->lower_path.mnt;

	si->lower_path.dentry = dest_dentry;     /* lookup_one returned a ref */
	si->lower_path.mnt    = dest_path.mnt;
	mntget(si->lower_path.mnt);

	{
		struct dentry *smoothfs_dentry = d_find_alias(inode);

		if (smoothfs_dentry) {
			smoothfs_set_lower_dentry(smoothfs_dentry, dest_dentry);
			dput(smoothfs_dentry);
		}
	}

	si->current_tier   = si->intended_tier;
	si->cutover_gen++;
	si->movement_state = SMOOTHFS_MS_SWITCHED;
	wake_up_all(&si->cutover_wq);  /* writers stalled in write_iter */

	/* Phase 5.3: lease-break signal. If the cutover proceeded despite
	 * a held SMB lease (only possible via a force=true MOVE_PLAN), the
	 * Samba VFS module — or any fanotify/inotify listener standing in
	 * for it — needs to know so it can break its lease with the SMB
	 * client before the new tier's bytes become client-visible.
	 * FS_MODIFY is a good carrier: clients already expect to
	 * revalidate on it, and the pin_state check keeps normal
	 * (non-forced) moves from spamming listeners. The pin is cleared
	 * here so the forced state isn't visible past the cutover. */
	if (si->pin_state == SMOOTHFS_PIN_LEASE) {
		si->pin_state = SMOOTHFS_PIN_NONE;
		fsnotify_inode(inode, FS_MODIFY);
	}

	/* Refresh attrs from the new lower. */
	smoothfs_copy_attrs(inode, d_inode(dest_dentry));

	/* Kick writeback for observability. Replay normalizes from lower
	 * tier contents if this record is lost before the next drain. */
	smoothfs_placement_record(sbi, oid, SMOOTHFS_MS_SWITCHED,
				  si->current_tier, si->current_tier,
				  /*sync=*/true);
	smoothfs_netlink_emit_move_state(sbi, oid, SMOOTHFS_MS_SWITCHED,
					 transaction_seq);

	inode_unlock(inode);

	/* Drop the iget-path_get ref on the old dentry/mnt after the
	 * inode is unlocked. */
	dput(old_lower_dentry);
	mntput(old_lower_mnt);
	return 0;

out_fail:
	si->movement_state = SMOOTHFS_MS_FAILED;
	wake_up_all(&si->cutover_wq);
	/* Informational: diagnostic record of the failure; tierd retries
	 * based on live state, so durability here isn't load-bearing. */
	smoothfs_placement_record(sbi, oid, SMOOTHFS_MS_FAILED,
				  si->current_tier, si->intended_tier,
				  /*sync=*/false);
out_unlock:
	inode_unlock(inode);
	return err;
}

/*
 * Admin-override path: zap PTEs for any shared mappings of `oid` so
 * the holder takes a fault on next access. Holders must then munmap
 * for mapping_writably_mapped() to drop to false and MOVE_PLAN to
 * accept; revoke itself does not force the VMAs to unlink. Since
 * smoothfs_mmap rebinds vma->vm_file to the lower, VMAs are indexed
 * by the lower's i_mapping and that's the mapping we zap.
 *
 * Phase 0 §0.4 spec: "MAP_SHARED for write: forbidden during active
 * movement; admin override path is `tierd-cli smoothfs revoke
 * <oid>` which zaps PTEs. Operator workflow: revoke, then kill or
 * restart holders to force munmap."
 */
int smoothfs_revoke_mappings(struct smoothfs_sb_info *sbi,
			     const u8 oid[SMOOTHFS_OID_LEN])
{
	struct smoothfs_inode_info *si;
	struct inode *lower_inode;
	struct inode *inode;

	si = smoothfs_lookup_oid(sbi, oid);
	if (!si)
		return -ENOENT;
	inode = &si->vfs_inode;
	inode_lock(inode);
	si->mappings_quiesced = true;
	lower_inode = d_inode(si->lower_path.dentry);
	unmap_mapping_range(lower_inode->i_mapping, 0, 0, 1);
	inode_unlock(inode);
	return 0;
}

void smoothfs_clear_pool_mapping_quiesce(struct smoothfs_sb_info *sbi)
{
	struct smoothfs_inode_info *si;

	down_read(&sbi->inode_lock);
	list_for_each_entry(si, &sbi->inode_list, sb_link)
		WRITE_ONCE(si->mappings_quiesced, false);
	up_read(&sbi->inode_lock);
}

int smoothfs_movement_abort(struct smoothfs_sb_info *sbi,
			    const u8 oid[SMOOTHFS_OID_LEN],
			    u64 transaction_seq, const char *reason)
{
	struct smoothfs_inode_info *si;
	struct inode *inode;
	int err = 0;

	(void)reason;

	si = smoothfs_lookup_oid(sbi, oid);
	if (!si)
		return -ENOENT;

	inode = &si->vfs_inode;
	inode_lock(inode);
	if (si->transaction_seq != transaction_seq) {
		err = -ESTALE;
		goto out;
	}
	switch (si->movement_state) {
	case SMOOTHFS_MS_PLAN_ACCEPTED:
	case SMOOTHFS_MS_DESTINATION_RESERVED:
	case SMOOTHFS_MS_COPY_IN_PROGRESS:
	case SMOOTHFS_MS_COPY_COMPLETE:
	case SMOOTHFS_MS_COPY_VERIFIED:
		si->movement_state  = SMOOTHFS_MS_FAILED;
		si->intended_tier   = si->current_tier;
		si->transaction_seq = 0;
		/* Informational abort record; tierd drives recovery from
		 * live state, durability not load-bearing. */
		smoothfs_placement_record(sbi, oid, SMOOTHFS_MS_FAILED,
					  si->current_tier, si->current_tier,
					  /*sync=*/false);
		smoothfs_netlink_emit_move_state(sbi, oid,
						 SMOOTHFS_MS_FAILED,
						 transaction_seq);
		break;
	default:
		err = -EBUSY;
	}
out:
	inode_unlock(inode);
	return err;
}

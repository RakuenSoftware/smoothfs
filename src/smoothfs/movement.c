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
#include <linux/limits.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/completion.h>
#include <linux/kref.h>

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

/*
 * Sleepable OID lookup: the fast oid_map hit, else lazily instantiate the
 * placement-logged inode from the recovery index. MUST NOT be called under
 * RCU/spinlock (the resolve sleeps in kern_path + iget). RCU callers (NFS
 * fh_to_dentry) do the fast lookup under RCU and call smoothfs_recovery_
 * resolve_oid directly outside the RCU section on a miss.
 */
struct smoothfs_inode_info *
smoothfs_lookup_oid_resolve(struct smoothfs_sb_info *sbi,
			    const u8 oid[SMOOTHFS_OID_LEN])
{
	struct smoothfs_inode_info *si = smoothfs_lookup_oid(sbi, oid);

	if (si)
		return si;
	return smoothfs_recovery_resolve_oid(sbi, oid);
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

static char *smoothfs_current_rel_path(struct inode *inode)
{
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct dentry *dentry;
	char *buf;
	char *path;
	char *rel = NULL;

	dentry = d_find_alias(inode);
	if (!dentry)
		return si->rel_path ? kstrdup(si->rel_path, GFP_KERNEL) : NULL;

	buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buf)
		goto out;

	path = dentry_path_raw(dentry, buf, PATH_MAX);
	if (!IS_ERR(path)) {
		if (*path == '/')
			path++;
		rel = kstrdup(path, GFP_KERNEL);
	}
	kfree(buf);

out:
	dput(dentry);
	if (!rel && si->rel_path)
		rel = kstrdup(si->rel_path, GFP_KERNEL);
	return rel;
}

/*
 * Re-point a warm inode after tierd moved its file to another tier out-of-band.
 *
 * tierd's placement mover copies a file to the destination tier and unlinks the
 * source directly on the backing fs, bypassing the in-kernel cutover (see
 * smoothfs_movement_cutover), then pokes forget_lower on the old (tier,
 * lower_ino). The OID-identified smoothfs inode is unchanged, but its warm
 * dentry / si->lower_path still point at the now-unlinked old-tier lower:
 * ->getattr and open_lower_now keep dereferencing it and return ENOENT, while
 * readdir re-lists the name and d_revalidate never re-checks (the lowers install
 * no ->d_revalidate). A cold lookup would recover via smoothfs_lookup_rel_
 * across_tiers, but nothing forces the warm dentry cold.
 *
 * Resolve the object on any other tier and, if found, relower the inode and its
 * warm dentry exactly as the cutover does. Returns true if it re-pointed the
 * inode (caller must NOT evict it); false if the object is genuinely gone, so
 * the caller falls back to the reclaim/evict path.
 *
 * Called from the forget_lower sysfs store holding no lock. Takes the smoothfs
 * inode_lock (never a lower inode_lock, so the path_put-triggered lower eviction
 * cannot recurse), matching smoothfs_movement_cutover.
 */
bool smoothfs_relower_after_forget(struct smoothfs_sb_info *sbi,
				   struct inode *inode, u8 old_tier,
				   unsigned long old_lower_ino)
{
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct dentry *alias;
	struct path new_path, old_path;
	char *rel;
	u8 found_tier;

	rel = smoothfs_current_rel_path(inode);
	if (!rel)
		return false;
	if (smoothfs_lookup_rel_across_tiers(sbi, old_tier, rel, &new_path,
					     &found_tier)) {
		kfree(rel);
		return false;   /* genuinely removed -> caller evicts */
	}
	kfree(rel);

	inode_lock(inode);

	/*
	 * The out-of-band unlink already made (old_tier, old_lower_ino)
	 * dangling; purge its stale fast-path cache entry unconditionally so a
	 * later iget cannot resolve a reused lower ino back to this inode.
	 */
	smoothfs_lower_ino_map_remove(sbi, old_tier, old_lower_ino);

	/*
	 * A racing cutover/relower may have re-pointed us off old_tier already;
	 * if so the inode is live and correct — leave it and don't evict.
	 */
	if (smoothfs_tier_of(sbi, si->lower_path.mnt) != old_tier) {
		inode_unlock(inode);
		path_put(&new_path);
		return true;
	}

	old_path = si->lower_path;
	si->lower_path = new_path;   /* transfers the resolver's refs */
	(void)smoothfs_lower_ino_map_insert(sbi, found_tier,
					    d_inode(new_path.dentry)->i_ino,
					    inode->i_ino);

	alias = d_find_alias(inode);
	if (alias) {
		smoothfs_set_lower_dentry(alias, new_path.dentry);
		dput(alias);
	}

	si->current_tier = found_tier;
	si->cutover_gen++;   /* force open fds to reissue against the new tier */
	smoothfs_copy_attrs(inode, d_inode(new_path.dentry));
	inode_unlock(inode);

	/*
	 * Drop the last ref on the unlinked old lower after unlock: its backing
	 * ->evict_inode runs here and frees the blocks — the reclaim forget_lower
	 * originally existed for.
	 */
	path_put(&old_path);
	return true;
}

static int smoothfs_resolve_cutover_dest(struct smoothfs_sb_info *sbi,
					 u8 dest_tier,
					 struct dentry *src_dentry,
					 const char *rel_path,
					 struct path *dest_path)
{
	struct path parent_path;
	struct dentry *dest_dentry;
	struct qstr qname;
	char *work = NULL;
	char *name;
	char *slash;
	int err;

	if (dest_tier >= sbi->ntiers)
		return -EINVAL;

	if (rel_path && *rel_path) {
		work = kstrdup(rel_path, GFP_KERNEL);
		if (!work)
			return -ENOMEM;
		slash = strrchr(work, '/');
		if (slash) {
			*slash = '\0';
			name = slash + 1;
			if (!*work) {
				parent_path = sbi->tiers[dest_tier].lower_path;
				path_get(&parent_path);
			} else {
				err = vfs_path_lookup(
					sbi->tiers[dest_tier].lower_path.dentry,
					sbi->tiers[dest_tier].lower_path.mnt,
					work,
					LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
					&parent_path);
				if (err)
					goto out_free;
			}
		} else {
			name = work;
			parent_path = sbi->tiers[dest_tier].lower_path;
			path_get(&parent_path);
		}
	} else {
		name = (char *)src_dentry->d_name.name;
		parent_path = sbi->tiers[dest_tier].lower_path;
		path_get(&parent_path);
	}

	if (!name || !*name) {
		err = -EINVAL;
		goto out_parent;
	}

	qname = (struct qstr)QSTR_INIT(name, strlen(name));
	inode_lock(d_inode(parent_path.dentry));
	dest_dentry = smoothfs_compat_lookup(&nop_mnt_idmap, &qname,
					     parent_path.dentry);
	inode_unlock(d_inode(parent_path.dentry));
	if (IS_ERR(dest_dentry)) {
		err = PTR_ERR(dest_dentry);
		goto out_parent;
	}
	if (d_really_is_negative(dest_dentry)) {
		dput(dest_dentry);
		err = -ENOENT;
		goto out_parent;
	}

	dest_path->dentry = dest_dentry;
	dest_path->mnt = parent_path.mnt;
	mntget(dest_path->mnt);
	err = 0;

out_parent:
	path_put(&parent_path);
out_free:
	kfree(work);
	return err;
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

	si = smoothfs_lookup_oid_resolve(sbi, oid);
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

#define SMOOTHFS_CUTOVER_DRAIN_TIMEOUT_MS 5000

struct smoothfs_cutover_drain {
	struct rcu_head  rcu;
	struct kref      ref;
	struct completion done;
};

static void smoothfs_cutover_drain_free(struct kref *ref)
{
	kfree(container_of(ref, struct smoothfs_cutover_drain, ref));
}

static void smoothfs_cutover_drain_cb(struct rcu_head *head)
{
	struct smoothfs_cutover_drain *d =
		container_of(head, struct smoothfs_cutover_drain, rcu);
	complete(&d->done);
	kref_put(&d->ref, smoothfs_cutover_drain_free);
}

/*
 * Wait up to SMOOTHFS_CUTOVER_DRAIN_TIMEOUT_MS for all in-flight
 * smoothfs_write_iter callers to exit the cutover_srcu read side.
 * Uses call_srcu (non-blocking) + wait_for_completion_timeout so that a
 * single writer blocked on a stalled lower-fs I/O cannot hold up cutover
 * indefinitely — returns -EBUSY on timeout, 0 on success.
 *
 * If allocation fails we fall back to unbounded synchronize_srcu; the
 * OOM path is already degraded enough that the extra wait is the least
 * of our problems.
 */
static int smoothfs_cutover_drain_writers(struct smoothfs_sb_info *sbi)
{
	struct smoothfs_cutover_drain *d;

	d = kmalloc(sizeof(*d), GFP_KERNEL);
	if (!d) {
		synchronize_srcu(&sbi->cutover_srcu);
		return 0;
	}

	kref_init(&d->ref);
	init_completion(&d->done);
	kref_get(&d->ref);  /* callback holds a ref; caller holds the other */
	call_srcu(&sbi->cutover_srcu, &d->rcu, smoothfs_cutover_drain_cb);

	if (!wait_for_completion_timeout(&d->done,
				msecs_to_jiffies(SMOOTHFS_CUTOVER_DRAIN_TIMEOUT_MS))) {
		/* Timed out. Release our ref — callback will free when it fires. */
		kref_put(&d->ref, smoothfs_cutover_drain_free);
		return -EBUSY;
	}

	kref_put(&d->ref, smoothfs_cutover_drain_free);
	return 0;
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
			      u64 transaction_seq,
			      u64 expected_write_seq,
			      bool check_write_seq)
{
	struct smoothfs_inode_info *si;
	struct inode *inode;
	struct dentry *src_dentry, *old_lower_dentry;
	struct vfsmount *old_lower_mnt;
	struct path dest_path = {};
	char *rel_path = NULL;
	int err = 0;

	if (sbi->quiesced)
		return -EAGAIN;

	si = smoothfs_lookup_oid_resolve(sbi, oid);
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
	 * cutover_wq; already-in-flight writers hold the SRCU read side.
	 * smoothfs_cutover_drain_writers() waits up to
	 * SMOOTHFS_CUTOVER_DRAIN_TIMEOUT_MS for them to exit rather than
	 * blocking indefinitely — a writer stuck on a stalled HDD/NFS I/O
	 * was holding every cutover hostage and starving backup throughput.
	 */
	err = smoothfs_cutover_drain_writers(sbi);
	if (err) {
		inode_lock(inode);
		if (si->movement_state == SMOOTHFS_MS_CUTOVER_IN_PROGRESS) {
			si->movement_state = SMOOTHFS_MS_COPY_VERIFIED;
			wake_up_all(&si->cutover_wq);
			smoothfs_placement_record(sbi, oid, SMOOTHFS_MS_COPY_VERIFIED,
						  si->current_tier, si->intended_tier,
						  /*sync=*/false);
		}
		inode_unlock(inode);
		return -EBUSY;
	}

	inode_lock(inode);
	if (si->movement_state != SMOOTHFS_MS_CUTOVER_IN_PROGRESS) {
		/* Another path raced us out of the cutover state while we
		 * waited in smoothfs_cutover_drain_writers. Roll back. */
		si->movement_state = SMOOTHFS_MS_COPY_VERIFIED;
		wake_up_all(&si->cutover_wq);
		smoothfs_placement_record(sbi, oid, SMOOTHFS_MS_COPY_VERIFIED,
					  si->current_tier, si->intended_tier,
					  /*sync=*/false);
		err = -EBUSY;
		goto out_unlock;
	}
	if (check_write_seq &&
	    atomic64_read(&si->write_seq) != expected_write_seq) {
		err = -ESTALE;
		goto out_fail;
	}

	/* Look up the dest dentry on the destination lower. tierd copied
	 * to the namespace-relative path returned by INSPECT, so resolve the
	 * same relative path here. Falling back to the basename at the tier
	 * root is only for old in-memory objects that do not yet carry
	 * rel_path. */
	src_dentry = si->lower_path.dentry;
	rel_path = smoothfs_current_rel_path(inode);
	err = smoothfs_resolve_cutover_dest(sbi, si->intended_tier,
					    src_dentry, rel_path, &dest_path);
	if (err)
		goto out_fail;

	/*
	 * Never cut over to an incomplete copy. tierd copies the file to the
	 * destination tier before the cutover switches the authoritative lower
	 * to it; if that copy did not finish -- e.g. a tier hit ENOSPC and left
	 * a truncated or 0-byte destination -- switching to it and then
	 * reclaiming the source in cleanup destroys the file's data. (Writers
	 * were already drained above, so the source size is stable here.)
	 * Refuse the cutover: the source stays authoritative and tierd
	 * re-copies. This is a fail-safe on top of the movement state machine,
	 * not a substitute for tierd's own copy verification.
	 */
	if (i_size_read(d_inode(dest_path.dentry)) <
	    i_size_read(d_inode(src_dentry))) {
		pr_warn_ratelimited(
			"smoothfs: cutover refused, destination incomplete (%lld < %lld bytes); keeping source on tier %u\n",
			(long long)i_size_read(d_inode(dest_path.dentry)),
			(long long)i_size_read(d_inode(src_dentry)),
			si->current_tier);
		path_put(&dest_path);
		err = -EAGAIN;
		goto out_fail;
	}

	/* Swap lower_path: keep refs balanced. The smoothfs dentry's
	 * d_fsdata also points at the OLD lower dentry — update it too,
	 * otherwise smoothfs_d_release dputs a freed pointer at umount. */
	old_lower_dentry = si->lower_path.dentry;
	old_lower_mnt    = si->lower_path.mnt;

	si->lower_path = dest_path;     /* owns lookup/mnt refs from resolver */
	if (rel_path) {
		smoothfs_path_map_del(sbi, si);
		kfree(si->rel_path);
		si->rel_path = rel_path;
		rel_path = NULL;
		smoothfs_path_map_add(sbi, si);
	}

	{
		struct dentry *smoothfs_dentry = d_find_alias(inode);

		if (smoothfs_dentry) {
			smoothfs_set_lower_dentry(smoothfs_dentry,
						  si->lower_path.dentry);
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
	smoothfs_copy_attrs(inode, d_inode(si->lower_path.dentry));

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
	kfree(rel_path);
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

	si = smoothfs_lookup_oid_resolve(sbi, oid);
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

	si = smoothfs_lookup_oid_resolve(sbi, oid);
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

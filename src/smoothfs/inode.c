// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - inode operations.
 *
 * Phase 1 implements the full passthrough op surface that the parent
 * proposal §Phase 1 lists: lookup, create, mknod, link, symlink, mkdir,
 * rmdir, rename, unlink, getattr, setattr, plus xattr/ACL/lock via the
 * separate handler tables in xattr.c / acl.c / lock.c.
 *
 * Targets kernel >= 6.6 (mnt_idmap-based ops, int-returning mkdir).
 * The Phase 0 contract §Operational Delivery names the appliance
 * kernel matrix as still-outstanding; the version pin lives in
 * dkms.conf and the kernel-matrix decision document.
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/cred.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/xattr.h>
#include <linux/limits.h>
#include <linux/statfs.h>

#include "smoothfs.h"

/* Kernel-version pin lives in compat.h. */

#define SMOOTHFS_DEFAULT_FULL_PCT 98

static char *smoothfs_rel_path_from_dentry(struct dentry *dentry)
{
	char *buf, *path, *rel = NULL;

	buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buf)
		return NULL;
	path = dentry_path_raw(dentry, buf, PATH_MAX);
	if (!IS_ERR(path)) {
		if (*path == '/')
			path++;
		rel = kstrdup(path, GFP_KERNEL);
	}
	kfree(buf);
	return rel;
}

static int smoothfs_resolve_rel_path_on_tier(struct smoothfs_sb_info *sbi,
					     u8 tier, const char *rel_path,
					     struct path *out)
{
	char *buf, *rendered, *full = NULL;
	int err;

	buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	rendered = d_path(&sbi->tiers[tier].lower_path, buf, PATH_MAX);
	if (IS_ERR(rendered)) {
		err = PTR_ERR(rendered);
		kfree(buf);
		return err;
	}
	if (rel_path && *rel_path)
		full = kasprintf(GFP_KERNEL, "%s/%s", rendered, rel_path);
	else
		full = kstrdup(rendered, GFP_KERNEL);
	kfree(buf);
	if (!full)
		return -ENOMEM;
	err = kern_path(full, LOOKUP_FOLLOW, out);
	kfree(full);
	return err;
}

static bool smoothfs_tier_near_enospc(struct smoothfs_sb_info *sbi, u8 tier)
{
	struct kstatfs st;
	u8 full_pct = READ_ONCE(sbi->write_staging_full_pct);
	int err;

	err = vfs_statfs(&sbi->tiers[tier].lower_path, &st);
	if (err || st.f_blocks == 0)
		return false;

	if (full_pct == 0 || full_pct > 100)
		full_pct = SMOOTHFS_DEFAULT_FULL_PCT;
	return (st.f_blocks - st.f_bavail) * 100 >= st.f_blocks * full_pct;
}

static u8 smoothfs_select_create_tier(struct smoothfs_sb_info *sbi)
{
	u8 tier;
	u8 best_tier;
	int best_load;

	if (sbi->ntiers <= 1)
		return sbi->fastest_tier;
	if (READ_ONCE(sbi->write_staging_enabled) &&
	    !smoothfs_tier_near_enospc(sbi, sbi->fastest_tier))
		return sbi->fastest_tier;

	best_tier = sbi->fastest_tier;
	best_load = atomic_read(&sbi->tiers[best_tier].active_writes) +
		    atomic_read(&sbi->tiers[best_tier].pending_writes);
	if (best_load == 0)
		return best_tier;

	for (tier = 0; tier < sbi->ntiers; tier++) {
		int load;

		if (tier == best_tier)
			continue;
		load = atomic_read(&sbi->tiers[tier].active_writes) +
		       atomic_read(&sbi->tiers[tier].pending_writes);
		if (load == 0)
			return tier;
		if (load < best_load) {
			best_load = load;
			best_tier = tier;
		}
	}

	return best_tier;
}

static int smoothfs_ensure_oid_persisted(struct smoothfs_inode_info *si)
{
	u8 oid[SMOOTHFS_OID_LEN];
	int err;

	err = smoothfs_read_oid_xattr(si->lower_path.dentry, oid);
	if (!err)
		return 0;
	if (err != -ENODATA)
		return err;

	err = smoothfs_write_oid_xattr(si->lower_path.dentry, si->oid);
	if (err == -EEXIST)
		return 0;
	return err;
}

static int smoothfs_set_inode_placement(struct smoothfs_inode_info *si,
					const char *rel_path, u8 tier)
{
	char *dup = NULL;

	if (rel_path) {
		dup = kstrdup(rel_path, GFP_KERNEL);
		if (!dup)
			return -ENOMEM;
	}

	kfree(si->rel_path);
	si->rel_path = dup;
	si->current_tier = tier;
	si->intended_tier = tier;
	si->movement_state = SMOOTHFS_MS_PLACED;
	si->transaction_seq = 0;
	return 0;
}

static int smoothfs_track_placed(struct smoothfs_sb_info *sbi,
				 struct inode *inode,
				 const char *rel_path, u8 tier,
				 bool pin_lookup_ref, bool record_log)
{
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	int err;

	err = smoothfs_set_inode_placement(si, rel_path, tier);
	if (err)
		return err;

	if (record_log) {
		err = smoothfs_ensure_oid_persisted(si);
		if (err)
			return err;
		err = smoothfs_placement_record(sbi, si->oid, SMOOTHFS_MS_PLACED,
						tier, tier, /*sync=*/false);
		if (err)
			return err;
	}

	if (pin_lookup_ref)
		atomic_set(&si->replay_pinned, 1);
	return 0;
}

static bool smoothfs_should_stage_truncate(struct smoothfs_sb_info *sbi,
					   struct smoothfs_inode_info *si,
					   struct inode *inode,
					   const struct iattr *attr)
{
	if (!READ_ONCE(sbi->write_staging_enabled))
		return false;
	if (!S_ISREG(inode->i_mode))
		return false;
	if (!(attr->ia_valid & ATTR_SIZE) || attr->ia_size != 0)
		return false;
	if (si->current_tier == sbi->fastest_tier)
		return false;
	if (si->pin_state != SMOOTHFS_PIN_NONE)
		return false;
	if (smoothfs_tier_near_enospc(sbi, sbi->fastest_tier))
		return false;
	return true;
}

static int smoothfs_materialize_parent_on_tier(struct mnt_idmap *idmap,
					       struct super_block *sb,
					       struct smoothfs_sb_info *sbi,
					       u8 tier, const char *rel_path,
					       struct path *out);

static int smoothfs_stage_truncate_to_fast(struct mnt_idmap *idmap,
					   struct dentry *dentry,
					   struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(inode->i_sb);
	struct path parent_path;
	struct path new_path;
	struct path old_path = {};
	struct dentry *lower;
	struct qstr qname = dentry->d_name;
	char *rel_path = NULL;
	char *parent_rel_path = NULL;
	char *rel_dup = NULL;
	u8 old_tier;
	int err;

	if (!smoothfs_should_stage_truncate(sbi, si, inode, attr))
		return -EOPNOTSUPP;

	rel_path = smoothfs_rel_path_from_dentry(dentry);
	parent_rel_path = smoothfs_rel_path_from_dentry(dentry->d_parent);
	if (!rel_path || !parent_rel_path) {
		err = -ENOMEM;
		goto out;
	}
	rel_dup = kstrdup(rel_path, GFP_KERNEL);
	if (!rel_dup) {
		err = -ENOMEM;
		goto out;
	}

	err = smoothfs_materialize_parent_on_tier(idmap, inode->i_sb, sbi,
						  sbi->fastest_tier,
						  parent_rel_path,
						  &parent_path);
	if (err)
		goto out;

	inode_lock(d_inode(parent_path.dentry));
	lower = smoothfs_compat_lookup(&nop_mnt_idmap, &qname,
				       parent_path.dentry);
	if (IS_ERR(lower)) {
		err = PTR_ERR(lower);
		inode_unlock(d_inode(parent_path.dentry));
		path_put(&parent_path);
		goto out;
	}
	if (d_really_is_negative(lower)) {
		err = smoothfs_compat_create(idmap, d_inode(parent_path.dentry),
					     lower, inode->i_mode & 07777,
					     false);
		if (err) {
			dput(lower);
			inode_unlock(d_inode(parent_path.dentry));
			path_put(&parent_path);
			goto out;
		}
	}
	inode_unlock(d_inode(parent_path.dentry));

	new_path.mnt = parent_path.mnt;
	new_path.dentry = lower;
	mntget(new_path.mnt);

	err = notify_change(idmap, lower, attr, NULL);
	if (err)
		goto out_new_path;

	err = smoothfs_write_oid_xattr(lower, si->oid);
	if (err == -EEXIST)
		err = 0;
	if (err)
		goto out_new_path;
	err = smoothfs_write_gen_xattr(lower, si->gen);
	if (err)
		goto out_new_path;

	/* The smoothfs inode's i_rwsem is already write-held by VFS:
	 * do_truncate -> inode_lock(d_inode(dentry)) -> notify_change ->
	 * smoothfs_setattr -> here. Re-taking inode_lock(inode) on the
	 * same task would be a writer-on-writer recursive acquire on
	 * the same rwsem and self-deadlock the truncate (kernel hung-task
	 * watchdog reports "<writer> blocked on rw-semaphore likely owned
	 * by task <writer>"). The smoothfs_inode_info field updates below
	 * are already serialized: VFS i_rwsem mutually excludes other
	 * setattr/movement paths that take inode_lock(inode), and the
	 * cutover_srcu read-side lock taken by smoothfs_begin_data_change
	 * keeps the placement-cutover writer drained. */
	old_path = si->lower_path;
	old_tier = smoothfs_tier_of(sbi, old_path.mnt);
	si->lower_path = new_path;
	kfree(si->rel_path);
	si->rel_path = rel_dup;
	rel_dup = NULL;
	si->current_tier = sbi->fastest_tier;
	si->intended_tier = sbi->fastest_tier;
	si->movement_state = SMOOTHFS_MS_PLACED;
	si->transaction_seq = 0;
	si->write_staged = true;
	si->write_staged_drain_tier = old_tier;
	si->cutover_gen++;

	if (old_tier < SMOOTHFS_MAX_TIERS && old_path.dentry)
		smoothfs_lower_ino_map_remove(sbi, old_tier,
			d_inode(old_path.dentry)->i_ino);
	(void)smoothfs_lower_ino_map_insert(sbi, sbi->fastest_tier,
		d_inode(new_path.dentry)->i_ino, inode->i_ino);
	path_put(&old_path);
	smoothfs_set_lower_dentry(dentry, lower);
	smoothfs_copy_attrs(inode, d_inode(lower));
	smoothfs_write_staging_note_rehome(sbi);
	err = smoothfs_placement_record(sbi, si->oid, SMOOTHFS_MS_PLACED,
					sbi->fastest_tier, sbi->fastest_tier,
					/*sync=*/false);
	if (err) {
		pr_warn_ratelimited("smoothfs: staged truncate placement record failed: %d\n",
				    err);
		err = 0;
	}
	path_put(&parent_path);
	goto out;

out_new_path:
	path_put(&new_path);
	path_put(&parent_path);
out:
	kfree(rel_dup);
	kfree(parent_rel_path);
	kfree(rel_path);
	return err;
}

static int smoothfs_lookup_rel_across_tiers(struct smoothfs_sb_info *sbi,
					    u8 exclude_tier,
					    const char *rel_path,
					    struct path *out,
					    u8 *found_tier)
{
	u8 tier;

	for (tier = 0; tier < sbi->ntiers; tier++) {
		if (tier == exclude_tier)
			continue;
		if (!rel_path || !*rel_path)
			continue;
		if (!smoothfs_metadata_tier_active(sbi, tier)) {
			smoothfs_note_metadata_tier_skip(sbi);
			continue;
		}
		if (!smoothfs_resolve_rel_path_on_tier(sbi, tier, rel_path, out)) {
			if (found_tier)
				*found_tier = tier;
			return 0;
		}
	}

	return -ENOENT;
}

static int smoothfs_materialize_parent_on_tier(struct mnt_idmap *idmap,
					       struct super_block *sb,
					       struct smoothfs_sb_info *sbi,
					       u8 tier, const char *rel_path,
					       struct path *out)
{
	struct path cur;
	char *work = NULL, *rest, *component;
	char *built = NULL;
	int err = 0;

	cur = sbi->tiers[tier].lower_path;
	path_get(&cur);

	if (!rel_path || !*rel_path) {
		*out = cur;
		return 0;
	}

	work = kstrdup(rel_path, GFP_KERNEL);
	if (!work) {
		err = -ENOMEM;
		goto out_err;
	}
	rest = work;

	while ((component = strsep(&rest, "/")) != NULL) {
		struct qstr qname;
		struct dentry *child;
		struct path child_path;
		bool created = false;

		if (!*component)
			continue;

		if (!built) {
			built = kstrdup(component, GFP_KERNEL);
		} else {
			char *next = kasprintf(GFP_KERNEL, "%s/%s", built, component);
			kfree(built);
			built = next;
		}
		if (!built) {
			err = -ENOMEM;
			goto out_err;
		}

		qname = (struct qstr)QSTR_INIT(component, strlen(component));
		inode_lock(d_inode(cur.dentry));
		child = smoothfs_compat_lookup(&nop_mnt_idmap, &qname, cur.dentry);
		if (IS_ERR(child)) {
			err = PTR_ERR(child);
			inode_unlock(d_inode(cur.dentry));
			goto out_err;
		}
		if (d_really_is_negative(child)) {
			struct dentry *new_child;

			new_child = smoothfs_compat_mkdir(idmap, d_inode(cur.dentry),
							  child, 0755);
			if (IS_ERR(new_child)) {
				err = PTR_ERR(new_child);
				dput(child);
				inode_unlock(d_inode(cur.dentry));
				goto out_err;
			}
			if (new_child != child) {
				dput(child);
				child = new_child;
			}
			created = true;
		}
		inode_unlock(d_inode(cur.dentry));

		child_path.mnt = cur.mnt;
		child_path.dentry = child;
		mntget(child_path.mnt);
		if (!S_ISDIR(d_inode(child)->i_mode)) {
			path_put(&child_path);
			err = -ENOTDIR;
			goto out_err;
		}

		if (created) {
			struct inode *inode;

			inode = smoothfs_iget(sb, &child_path, false, true);
			if (IS_ERR(inode)) {
				path_put(&child_path);
				err = PTR_ERR(inode);
				goto out_err;
			}
			err = smoothfs_track_placed(sbi, inode, built, tier,
						    /*pin_lookup_ref=*/true,
						    /*record_log=*/true);
			if (err) {
				iput(inode);
				path_put(&child_path);
				goto out_err;
			}
		}

		path_put(&cur);
		cur = child_path;
	}

	*out = cur;
	kfree(built);
	kfree(work);
	return 0;

out_err:
	path_put(&cur);
	kfree(built);
	kfree(work);
	return err;
}

/* ----------------------------------------------------------------- */
/* Lookup                                                            */
/* ----------------------------------------------------------------- */

static struct dentry *smoothfs_lookup(struct inode *dir, struct dentry *dentry,
				      unsigned int flags)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(dir->i_sb);
	struct smoothfs_inode_info *parent = SMOOTHFS_I(dir);
	u8 parent_tier = smoothfs_tier_of(sbi, parent->lower_path.mnt);
	struct dentry *lower_parent = parent->lower_path.dentry;
	struct dentry *lower;
	struct path lower_path;
	struct inode *inode = NULL;

	inode_lock_shared(d_inode(lower_parent));
	lower = smoothfs_compat_lookup(&nop_mnt_idmap, &dentry->d_name, lower_parent);
	inode_unlock_shared(d_inode(lower_parent));

	if (IS_ERR(lower))
		return ERR_CAST(lower);

	if (parent_tier >= sbi->ntiers)
		parent_tier = sbi->fastest_tier;

	if (d_really_is_positive(lower)) {
		lower_path.mnt = parent->lower_path.mnt;
		lower_path.dentry = lower;
		mntget(lower_path.mnt);

		inode = smoothfs_iget(dir->i_sb, &lower_path, false, false);
		path_put(&lower_path);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
		/* path_put consumed our lookup_one ref on lower; the inode
		 * now holds its own via si->lower_path. The helper will
		 * take d_fsdata's own ref below. */
		smoothfs_set_lower_dentry(dentry, lower);
		return d_splice_alias(inode, dentry);
	}

	/* Negative lookup. lower still holds our lookup_one reference. */
	{
		char *rel_path = NULL;
		struct smoothfs_inode_info *replayed;

		{
			char *buf, *path;

			buf = kmalloc(PATH_MAX, GFP_KERNEL);
			if (!buf) {
				dput(lower);
				return ERR_PTR(-ENOMEM);
			}
			path = dentry_path_raw(dentry, buf, PATH_MAX);
			if (!IS_ERR(path)) {
				if (*path == '/')
					path++;
				rel_path = kstrdup(path, GFP_KERNEL);
			}
			kfree(buf);
		}
		if (rel_path) {
			replayed = smoothfs_lookup_rel_path(sbi, rel_path);
			if (replayed) {
				struct path replay_path = replayed->lower_path;
				int was_replay_pinned;

				inode = igrab(&replayed->vfs_inode);
				if (!inode) {
					kfree(rel_path);
					dput(lower);
					return ERR_PTR(-ESTALE);
				}
				/* Hand off the placement-replay pin to this
				 * lookup's dentry alias. d_splice_alias below
				 * will drop the dentry's caller ref into the
				 * dentry; iput here releases the original
				 * placement_replay-held ref. Without this
				 * handoff the pin would survive past umount. */
				was_replay_pinned =
					atomic_xchg(&replayed->replay_pinned, 0);
				if (was_replay_pinned)
					iput(&replayed->vfs_inode);
				dput(lower);
				lower = dget(replay_path.dentry);
			}
			if (!inode) {
				u8 found_tier;
				int err;

				err = smoothfs_lookup_rel_across_tiers(sbi, parent_tier,
								       rel_path,
								       &lower_path,
								       &found_tier);
				if (!err) {
					inode = smoothfs_iget(dir->i_sb, &lower_path, false, false);
					path_put(&lower_path);
					if (IS_ERR(inode)) {
						kfree(rel_path);
						dput(lower);
						return ERR_CAST(inode);
					}
					err = smoothfs_track_placed(sbi, inode, rel_path,
								    found_tier,
								    /*pin_lookup_ref=*/false,
								    /*record_log=*/true);
					if (err) {
						iput(inode);
						kfree(rel_path);
						dput(lower);
						return ERR_PTR(err);
					}
					dput(lower);
					lower = dget(smoothfs_lower_path(inode)->dentry);
				}
			}
		}
		kfree(rel_path);
	}
	smoothfs_set_lower_dentry(dentry, lower);
	dput(lower);
	return d_splice_alias(inode, dentry);
}

/* ----------------------------------------------------------------- */
/* getattr / setattr                                                 */
/* ----------------------------------------------------------------- */

static int smoothfs_getattr(struct mnt_idmap *idmap, const struct path *path,
			    struct kstat *stat, u32 request_mask,
			    unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(inode->i_sb);
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct path lower_path = *smoothfs_lower_path(inode);
	int err;

	if (!smoothfs_metadata_tier_active(sbi, READ_ONCE(si->current_tier))) {
		generic_fillattr(idmap, request_mask, inode, stat);
		stat->ino = inode->i_ino;
		stat->dev = inode->i_sb->s_dev;
		smoothfs_note_metadata_tier_skip(sbi);
		return 0;
	}

	/* Direct passthrough to the lower. vfs_getattr_nosec skips the
	 * security_inode_getattr hook because the VFS already ran it on
	 * the smoothfs path before dispatching to us. Avoiding the double
	 * LSM invocation is a measurable slice of the Phase 3 STAT p99.
	 *
	 * Mirror-on-every-getattr (the Phase 1/2 behaviour) is also gone:
	 * its per-call seqlock writes (i_size, set_nlink, *time_to_ts)
	 * were the other major contributor; smoothfs_copy_attrs now runs
	 * only on create/rename/cutover/setattr. */
	err = vfs_getattr_nosec(&lower_path, stat, request_mask, flags);
	if (err)
		return err;

	/* Preserve smoothfs's synthesised inode identity (§0.1). */
	stat->ino = inode->i_ino;
	stat->dev = inode->i_sb->s_dev;
	return 0;
}

static int smoothfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			    struct iattr *attr)
{
	struct dentry *lower = smoothfs_lower_dentry(dentry);
	struct inode *inode = d_inode(dentry);
	int err;
	int srcu_idx;

	err = setattr_prepare(idmap, dentry, attr);
	if (err)
		return err;

	srcu_idx = smoothfs_begin_data_change(inode);
	if (srcu_idx < 0)
		return srcu_idx;

	err = smoothfs_stage_truncate_to_fast(idmap, dentry, attr);
	if (err == 0) {
		if (attr->ia_valid & ATTR_SIZE)
			smoothfs_note_data_change(inode);
		smoothfs_end_data_change(inode, srcu_idx);
		return 0;
	}
	if (err != -EOPNOTSUPP) {
		smoothfs_end_data_change(inode, srcu_idx);
		return err;
	}

	inode_lock(d_inode(lower));
	err = notify_change(idmap, lower, attr, NULL);
	inode_unlock(d_inode(lower));
	if (err) {
		smoothfs_end_data_change(inode, srcu_idx);
		return err;
	}

	smoothfs_copy_attrs(inode, d_inode(lower));
	if (attr->ia_valid & ATTR_SIZE)
		smoothfs_note_data_change(inode);
	smoothfs_end_data_change(inode, srcu_idx);
	return 0;
}

/* ----------------------------------------------------------------- */
/* Create / mknod / symlink / link / mkdir / rmdir / unlink / rename */
/* ----------------------------------------------------------------- */

static int smoothfs_create(struct mnt_idmap *idmap, struct inode *dir,
			   struct dentry *dentry, umode_t mode, bool excl)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(dir->i_sb);
	struct path parent_path;
	struct dentry *lower;
	struct path lower_path;
	struct inode *inode;
	char *rel_path = NULL;
	char *parent_rel_path = NULL;
	u8 parent_tier;
	u8 tier;
	u8 start_tier;
	u8 attempt;
	bool fallback_placement = false;
	int err = -ENOSPC;

	parent_tier = smoothfs_tier_of(sbi, SMOOTHFS_I(dir)->lower_path.mnt);
	if (parent_tier >= sbi->ntiers)
		parent_tier = sbi->fastest_tier;
	start_tier = smoothfs_select_create_tier(sbi);

	rel_path = smoothfs_rel_path_from_dentry(dentry);
	parent_rel_path = smoothfs_rel_path_from_dentry(dentry->d_parent);
	if (!rel_path || !parent_rel_path) {
		err = -ENOMEM;
		goto out;
	}

	for (attempt = 0; attempt < sbi->ntiers; attempt++) {
		bool materialize_parent;

		tier = (start_tier + attempt) % sbi->ntiers;
		materialize_parent = tier != parent_tier;

		if (tier != sbi->ntiers - 1 && smoothfs_tier_near_enospc(sbi, tier)) {
			fallback_placement = true;
			continue;
		}

		if (materialize_parent) {
			err = smoothfs_materialize_parent_on_tier(idmap, dir->i_sb,
								  sbi, tier,
								  parent_rel_path,
								  &parent_path);
			if (err == -ENOSPC) {
				fallback_placement = true;
				continue;
			}
			if (err)
				goto out;
		} else {
			parent_path = SMOOTHFS_I(dir)->lower_path;
			path_get(&parent_path);
		}

		inode_lock(d_inode(parent_path.dentry));
		lower = smoothfs_compat_lookup(&nop_mnt_idmap, &dentry->d_name,
					       parent_path.dentry);
		if (IS_ERR(lower)) {
			err = PTR_ERR(lower);
			inode_unlock(d_inode(parent_path.dentry));
			path_put(&parent_path);
			goto out;
		}
		err = smoothfs_compat_create(idmap, d_inode(parent_path.dentry),
					     lower, mode, excl);
		inode_unlock(d_inode(parent_path.dentry));
		if (err) {
			dput(lower);
			path_put(&parent_path);
			if (err == -ENOSPC) {
				fallback_placement = true;
				continue;
			}
			goto out;
		}

		lower_path.mnt = parent_path.mnt;
		lower_path.dentry = lower;
		mntget(lower_path.mnt);

		inode = smoothfs_iget(dir->i_sb, &lower_path, false, true);
		path_put(&lower_path);
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
			path_put(&parent_path);
			goto out;
		}
		err = smoothfs_track_placed(sbi, inode, rel_path, tier,
					    /*pin_lookup_ref=*/false,
					    /*record_log=*/false);
		if (err) {
			iput(inode);
			path_put(&parent_path);
			goto out;
		}
		atomic_set(&SMOOTHFS_I(inode)->write_reservation, 1);
		atomic_inc(&sbi->tiers[tier].pending_writes);
		if (fallback_placement && tier != start_tier)
			smoothfs_spill_note_success(sbi, inode, parent_tier, tier);

		smoothfs_set_lower_dentry(dentry, lower);
		d_instantiate(dentry, inode);
		smoothfs_copy_attrs(dir, d_inode(parent_path.dentry));
		path_put(&parent_path);
		err = 0;
		goto out;
	}

out:
	if (err == -ENOSPC)
		smoothfs_spill_note_failed_all_tiers(sbi);
	kfree(parent_rel_path);
	kfree(rel_path);
	return err;
}

static int smoothfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
			  struct dentry *dentry, umode_t mode, dev_t rdev)
{
	struct dentry *lower_parent = smoothfs_lower_dentry(dentry->d_parent);
	struct dentry *lower;
	struct path lower_path;
	struct inode *inode;
	int err;

	inode_lock(d_inode(lower_parent));
	lower = smoothfs_compat_lookup(&nop_mnt_idmap, &dentry->d_name, lower_parent);
	if (IS_ERR(lower)) {
		inode_unlock(d_inode(lower_parent));
		return PTR_ERR(lower);
	}
	err = smoothfs_compat_mknod(idmap, d_inode(lower_parent), lower, mode, rdev);
	inode_unlock(d_inode(lower_parent));
	if (err) {
		dput(lower);
		return err;
	}

	lower_path.mnt = SMOOTHFS_I(dir)->lower_path.mnt;
	lower_path.dentry = lower;
	mntget(lower_path.mnt);

	inode = smoothfs_iget(dir->i_sb, &lower_path, false, true);
	path_put(&lower_path);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	smoothfs_set_lower_dentry(dentry, lower);
	d_instantiate(dentry, inode);
	smoothfs_copy_attrs(dir, d_inode(lower_parent));
	return 0;
}

static int smoothfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			    struct dentry *dentry, const char *symname)
{
	struct dentry *lower_parent = smoothfs_lower_dentry(dentry->d_parent);
	struct dentry *lower;
	struct path lower_path;
	struct inode *inode;
	int err;

	inode_lock(d_inode(lower_parent));
	lower = smoothfs_compat_lookup(&nop_mnt_idmap, &dentry->d_name, lower_parent);
	if (IS_ERR(lower)) {
		inode_unlock(d_inode(lower_parent));
		return PTR_ERR(lower);
	}
	err = smoothfs_compat_symlink(idmap, d_inode(lower_parent), lower, symname);
	inode_unlock(d_inode(lower_parent));
	if (err) {
		dput(lower);
		return err;
	}

	lower_path.mnt = SMOOTHFS_I(dir)->lower_path.mnt;
	lower_path.dentry = lower;
	mntget(lower_path.mnt);

	inode = smoothfs_iget(dir->i_sb, &lower_path, false, true);
	path_put(&lower_path);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	smoothfs_set_lower_dentry(dentry, lower);
	d_instantiate(dentry, inode);
	smoothfs_copy_attrs(dir, d_inode(lower_parent));
	return 0;
}

/* link(2): always within one tier. Cross-tier link returns EXDEV per
 * POSIX semantics §0.4. Phase 1 keeps the source tier; the
 * scheduler-observed nlink>1 will pin the link-set per Phase 2. */
static int smoothfs_link(struct dentry *old_dentry, struct inode *dir,
			 struct dentry *dentry)
{
	struct dentry *lower_old = smoothfs_lower_dentry(old_dentry);
	struct dentry *lower_parent = smoothfs_lower_dentry(dentry->d_parent);
	struct smoothfs_inode_info *si = SMOOTHFS_I(d_inode(old_dentry));
	struct dentry *lower;
	int err;

	if (lower_old->d_sb != lower_parent->d_sb)
		return -EXDEV;

	inode_lock(d_inode(lower_parent));
	lower = smoothfs_compat_lookup(&nop_mnt_idmap, &dentry->d_name, lower_parent);
	if (IS_ERR(lower)) {
		inode_unlock(d_inode(lower_parent));
		return PTR_ERR(lower);
	}
	err = vfs_link(lower_old, &nop_mnt_idmap, d_inode(lower_parent),
		       lower, NULL);
	inode_unlock(d_inode(lower_parent));
	if (err) {
		dput(lower);
		return err;
	}

	atomic_inc(&si->nlink_observed);
	/* Phase 0 §0.4: hardlink-set is pinned to current tier as soon
	 * as nlink > 1. Cleared automatically when nlink returns to 1
	 * via smoothfs_unlink. */
	if (atomic_read(&si->nlink_observed) > 1 &&
	    si->pin_state == SMOOTHFS_PIN_NONE)
		si->pin_state = SMOOTHFS_PIN_HARDLINK;

	smoothfs_set_lower_dentry(dentry, lower);
	dput(lower);
	ihold(d_inode(old_dentry));
	d_instantiate(dentry, d_inode(old_dentry));
	smoothfs_copy_attrs(d_inode(old_dentry), d_inode(lower_old));
	smoothfs_copy_attrs(dir, d_inode(lower_parent));
	return 0;
}

static int smoothfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct dentry *lower = smoothfs_lower_dentry(dentry);
	struct dentry *removing;
	struct inode *lower_dir = NULL;
	struct smoothfs_inode_info *si = SMOOTHFS_I(d_inode(dentry));
	struct inode *lower_inode;
	int err;

	/* When a file is spilled onto a non-canonical tier, lower lives on
	 * that tier's lower fs while smoothfs_lower_dentry(dentry->d_parent)
	 * still points at the canonical-tier parent. They are dentries from
	 * different lower filesystems, so the dentry-parent identity check
	 * inside smoothfs_compat_start_removing would reject the pair with
	 * EINVAL. lower->d_parent is the file's actual parent on its own
	 * lower fs; that is what vfs_unlink needs, and that is also the
	 * parent whose mtime/ctime the unlink updates — so the post-unlink
	 * smoothfs_copy_attrs reads from the same dentry. */
	removing = smoothfs_compat_start_removing(lower->d_parent, lower, &lower_dir);
	if (IS_ERR(removing))
		return PTR_ERR(removing);
	lower_inode = d_inode(removing);
	if (lower_inode && lower_inode->i_nlink == 0)
		err = 0;
	else
		err = vfs_unlink(&nop_mnt_idmap, lower_dir, removing, NULL);
	smoothfs_compat_end_removing(removing, lower_dir);

	if (!err) {
		atomic_dec(&si->nlink_observed);
		/* Clear hardlink pin once link-set returns to 1. */
		if (atomic_read(&si->nlink_observed) <= 1 &&
		    si->pin_state == SMOOTHFS_PIN_HARDLINK)
			si->pin_state = SMOOTHFS_PIN_NONE;
		/*
		 * Drop d_fsdata so this dentry stops pinning the lower
		 * dentry, but leave si->lower_path intact — the inode may
		 * still be alive (another hardlink, or nfsd holds a file
		 * handle), and getattr/read/write all deref si->lower_path
		 * unconditionally. evict_inode does the matching path_put
		 * when the inode's refcount actually drops to zero. An
		 * earlier version cleared si->lower_path here to work
		 * around a 6.19 vfs_rmdir d_walk stall; that stall has a
		 * different cause (since fixed upstream) and this clear
		 * introduced a NULL-deref under nfsd GETATTR after NFS
		 * UNLINK.
		 *
		 * drop_nlink (not clear_nlink) is what vfs_unlink did to
		 * the lower inode: decrement by one. If this was the last
		 * link, nlink goes to 0 and evict_inode takes over. If
		 * other hardlinks remain, nlink stays > 0 so vfs_link's
		 * i_nlink==0 guard (cthon04 basic/test7) does not spuriously
		 * refuse a subsequent link to the still-live inode.
		 */
		drop_nlink(d_inode(dentry));
		smoothfs_set_lower_dentry(dentry, NULL);
		d_drop(dentry);
		/* Copy from the parent we actually modified (lower->d_parent),
		 * not the canonical lower_parent which never saw the unlink. */
		smoothfs_copy_attrs(dir, d_inode(lower->d_parent));
	}
	return err;
}

static struct dentry *smoothfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				     struct dentry *dentry, umode_t mode)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(dir->i_sb);
	struct path parent_path;
	struct dentry *lower;
	struct path lower_path;
	struct inode *inode;
	char *rel_path = NULL;
	char *parent_rel_path = NULL;
	u8 parent_tier;
	u8 tier;
	int err = -ENOSPC;

	parent_tier = smoothfs_tier_of(sbi, SMOOTHFS_I(dir)->lower_path.mnt);
	if (parent_tier >= sbi->ntiers)
		parent_tier = sbi->fastest_tier;

	rel_path = smoothfs_rel_path_from_dentry(dentry);
	parent_rel_path = smoothfs_rel_path_from_dentry(dentry->d_parent);
	if (!rel_path || !parent_rel_path) {
		err = -ENOMEM;
		goto out_err;
	}

	for (tier = sbi->fastest_tier; tier < sbi->ntiers; tier++) {
		bool materialize_parent = tier != parent_tier;
		bool cold_placement = tier != sbi->fastest_tier;

		if (tier != sbi->ntiers - 1 && smoothfs_tier_near_enospc(sbi, tier))
			continue;

		if (materialize_parent) {
			err = smoothfs_materialize_parent_on_tier(idmap, dir->i_sb,
								  sbi, tier,
								  parent_rel_path,
								  &parent_path);
			if (err == -ENOSPC)
				continue;
			if (err)
				goto out_err;
		} else {
			parent_path = SMOOTHFS_I(dir)->lower_path;
			path_get(&parent_path);
		}

		inode_lock(d_inode(parent_path.dentry));
		lower = smoothfs_compat_lookup(&nop_mnt_idmap, &dentry->d_name,
					       parent_path.dentry);
		if (IS_ERR(lower)) {
			err = PTR_ERR(lower);
			inode_unlock(d_inode(parent_path.dentry));
			path_put(&parent_path);
			goto out_err;
		}
		{
			struct dentry *new_lower = smoothfs_compat_mkdir(idmap,
							d_inode(parent_path.dentry),
							lower, mode);
			inode_unlock(d_inode(parent_path.dentry));
			if (IS_ERR(new_lower)) {
				err = PTR_ERR(new_lower);
				dput(lower);
				path_put(&parent_path);
				if (err == -ENOSPC)
					continue;
				goto out_err;
			}
			if (new_lower != lower) {
				dput(lower);
				lower = new_lower;
			}
		}

		lower_path.mnt = parent_path.mnt;
		lower_path.dentry = lower;
		mntget(lower_path.mnt);

		inode = smoothfs_iget(dir->i_sb, &lower_path, false, true);
		path_put(&lower_path);
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
			path_put(&parent_path);
			goto out_err;
		}
		err = smoothfs_track_placed(sbi, inode, rel_path, tier,
					    /*pin_lookup_ref=*/false,
					    /*record_log=*/false);
		if (err) {
			iput(inode);
			path_put(&parent_path);
			goto out_err;
		}
		if (cold_placement)
			smoothfs_spill_note_success(sbi, inode, parent_tier, tier);

		smoothfs_set_lower_dentry(dentry, lower);
		d_instantiate(dentry, inode);
		smoothfs_copy_attrs(dir, d_inode(parent_path.dentry));
		path_put(&parent_path);
		kfree(parent_rel_path);
		kfree(rel_path);
		return NULL;
	}

out_err:
	if (err == -ENOSPC)
		smoothfs_spill_note_failed_all_tiers(sbi);
	kfree(parent_rel_path);
	kfree(rel_path);
	return ERR_PTR(err);
}

static int smoothfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct dentry *lower = smoothfs_lower_dentry(dentry);
	struct dentry *removing;
	struct inode *lower_dir = NULL;
	int err;

	/* See smoothfs_unlink for the reason we use lower->d_parent rather
	 * than smoothfs_lower_dentry(dentry->d_parent): the latter is the
	 * canonical-tier parent and may live on a different lower fs from
	 * the directory we are removing, which trips the dentry-parent
	 * identity check inside smoothfs_compat_start_removing. */
	removing = smoothfs_compat_start_removing(lower->d_parent, lower, &lower_dir);
	if (IS_ERR(removing))
		return PTR_ERR(removing);
	err = smoothfs_compat_rmdir(&nop_mnt_idmap, lower_dir, removing);
	smoothfs_compat_end_removing(removing, lower_dir);

	if (!err) {
		/* Drop d_fsdata so this dentry releases its pin on the
		 * lower dentry; si->lower_path stays until evict_inode.
		 * See smoothfs_unlink for the full rationale. */
		clear_nlink(d_inode(dentry));
		smoothfs_set_lower_dentry(dentry, NULL);
		d_drop(dentry);
		/* Copy from the parent we actually modified (lower->d_parent);
		 * see smoothfs_unlink for the canonical-vs-actual rationale. */
		smoothfs_copy_attrs(dir, d_inode(lower->d_parent));
	}
	return err;
}

static int smoothfs_rename(struct mnt_idmap *idmap,
			   struct inode *old_dir, struct dentry *old_dentry,
			   struct inode *new_dir, struct dentry *new_dentry,
			   unsigned int flags)
{
	struct dentry *lower_old_parent = smoothfs_lower_dentry(old_dentry->d_parent);
	struct dentry *lower_new_parent = smoothfs_lower_dentry(new_dentry->d_parent);
	struct dentry *lower_old = smoothfs_lower_dentry(old_dentry);
	struct dentry *lower_new = smoothfs_lower_dentry(new_dentry);
	struct dentry *trap;
	struct renamedata rd = {};
	int err;

	if (lower_old_parent->d_sb != lower_new_parent->d_sb)
		return -EXDEV;
	/* Cross-tier rename: source and destination resolve to lower
	 * dentries on different lower filesystems (e.g. source on fast,
	 * destination is a tier-fallthrough hit on slow). vfs_rename
	 * across superblocks is invalid; return -EXDEV so the caller
	 * (typically coreutils mv / Python shutil.move) falls back to
	 * copy+delete instead of crashing into the d_parent identity
	 * check below with EINVAL — which userspace then mis-interprets
	 * as "cannot move to a subdirectory of itself" and bails out. */
	if (lower_old && lower_new &&
	    lower_old->d_sb != lower_new->d_sb)
		return -EXDEV;
	if (lower_new)
		dget(lower_new);

	trap = lock_rename_child(lower_old, lower_new_parent);
	if (IS_ERR(trap)) {
		if (lower_new)
			dput(lower_new);
		return PTR_ERR(trap);
	}
	if (!lower_new) {
		lower_new = smoothfs_compat_lookup(&nop_mnt_idmap,
						   &new_dentry->d_name,
						   lower_new_parent);
		if (IS_ERR(lower_new)) {
			unlock_rename(lower_old_parent, lower_new_parent);
			return PTR_ERR(lower_new);
		}
	}
	if (d_unhashed(lower_old) || lower_old_parent != lower_old->d_parent) {
		unlock_rename(lower_old->d_parent, lower_new_parent);
		dput(lower_new);
		return -EINVAL;
	}
	if (d_unhashed(lower_new) || lower_new_parent != lower_new->d_parent) {
		unlock_rename(lower_old->d_parent, lower_new_parent);
		dput(lower_new);
		return -EINVAL;
	}
	if (trap == lower_old) {
		unlock_rename(lower_old->d_parent, lower_new_parent);
		dput(lower_new);
		return -EINVAL;
	}
	if (trap == lower_new) {
		err = (flags & RENAME_EXCHANGE) ? -EINVAL : -ENOTEMPTY;
		unlock_rename(lower_old->d_parent, lower_new_parent);
		dput(lower_new);
		return err;
	}
	if (d_really_is_positive(lower_new) && (flags & RENAME_NOREPLACE)) {
		unlock_rename(lower_old_parent, lower_new_parent);
		dput(lower_new);
		return -EEXIST;
	}

	memset(&rd, 0, sizeof(rd));
	rd.mnt_idmap = idmap;
	rd.old_parent = dget(lower_old->d_parent);
	rd.old_dentry = dget(lower_old);
	rd.new_parent = lower_new_parent;
	rd.new_dentry = dget(lower_new);
	rd.flags = flags;

	err = vfs_rename(&rd);
	unlock_rename(rd.old_parent, rd.new_parent);
	dput(rd.old_dentry);
	dput(rd.new_dentry);
	dput(rd.old_parent);
	if (err) {
		dput(lower_new);
		return err;
	}

	/* After this ->rename returns, vfs_rename calls d_move(old_dentry,
	 * new_dentry) which keeps OLD_dentry as the surviving dentry at
	 * the new position (with new's name). Any subsequent path walk for
	 * the new name finds OLD_dentry in the dcache, and its d_fsdata
	 * must point at the (renamed) lower — otherwise smoothfs_unlink /
	 * smoothfs_getattr on that dentry deref NULL. new_dentry becomes a
	 * throw-away after d_move, so clearing ITS d_fsdata is the right
	 * thing.
	 *
	 * (This was latent until Phase 4-prep's compat lookup fix made
	 * dentries actually stay cached across syscalls. Prior to that the
	 * next path walk always re-ran smoothfs_lookup and recreated a
	 * fresh dentry with a valid d_fsdata, so the mis-assignment never
	 * produced an observable fault.) */
	smoothfs_set_lower_dentry(old_dentry, lower_old);
	smoothfs_set_lower_dentry(new_dentry, NULL);
	dput(lower_new);
	smoothfs_copy_attrs(old_dir, d_inode(lower_old_parent));
	smoothfs_copy_attrs(new_dir, d_inode(lower_new_parent));

	/* Update si->rel_path on the moved inode to its new name.
	 * smoothfs_lookup falls through to smoothfs_lookup_rel_path on a
	 * canonical-tier negative lookup, which walks sb_link by si->rel_path
	 * string-equality. Without updating it here, a fresh stat() of the
	 * OLD path post-rename keeps resolving to the moved inode (the
	 * lower has the file at the new name; this list-walk still has it
	 * keyed under the old name). The visible symptom is dual-resolution
	 * — the directory listing correctly shows only the new name, but
	 * stat'ing the old path returns the renamed inode until drop_caches
	 * evicts the smoothfs inode and the rel_path goes away with it.
	 * smb_roundtrip and smbtorture base.rename / base.xcopy reproduce
	 * this deterministically. */
	{
		struct inode *moved_inode = d_inode(old_dentry);
		struct smoothfs_inode_info *si;
		char *new_rel = smoothfs_rel_path_from_dentry(new_dentry);

		if (moved_inode && new_rel) {
			si = SMOOTHFS_I(moved_inode);
			down_write(&SMOOTHFS_SB(old_dir->i_sb)->inode_lock);
			kfree(si->rel_path);
			si->rel_path = new_rel;
			up_write(&SMOOTHFS_SB(old_dir->i_sb)->inode_lock);
		} else {
			kfree(new_rel);
		}
	}
	return 0;
}

/*
 * .permission is deliberately NOT installed. The VFS falls back to
 * generic_permission(inode, mask) on the smoothfs inode, using the
 * mode/uid/gid we mirror from the lower at create time and refresh
 * in smoothfs_setattr. For the Phase 3 compat set (xfs, ext4, btrfs,
 * zfs) that's semantically identical to routing through the lower's
 * inode_permission — and it saves a full access-check stack + LSM
 * hook on every path-walk step.
 */

/* ----------------------------------------------------------------- */
/* Symlink readlink (get_link)                                       */
/* ----------------------------------------------------------------- */

static const char *smoothfs_get_link(struct dentry *dentry, struct inode *inode,
				     struct delayed_call *done)
{
	struct path *lower_path;
	struct dentry *lower;

	if (!dentry)
		return ERR_PTR(-ECHILD);

	lower = smoothfs_lower_dentry(dentry);
	if (!lower)
		return ERR_PTR(-EINVAL);
	lower_path = smoothfs_lower_path(inode);

	return vfs_get_link(lower, done);
}

/* ----------------------------------------------------------------- */
/* Dentry ops — d_revalidate trusts the lower's revalidator           */
/* ----------------------------------------------------------------- */

static int smoothfs_d_revalidate(struct inode *dir, const struct qstr *name,
				 struct dentry *dentry, unsigned int flags)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(dentry->d_sb);
	const struct dentry_operations *lower_ops;
	struct dentry *lower;
	struct dentry *lower_parent;

	/* Fast path for the Phase 3 compat set (xfs, ext4, btrfs, zfs):
	 * none of those lowers installs d_revalidate, so the probe marks
	 * any_lower_revalidates = false and every path-walk step returns
	 * 1 without dereferencing d_parent. Safe in RCU-walk too —
	 * dentry->d_sb is stable for a dentry's lifetime. */
	if (!READ_ONCE(sbi->any_lower_revalidates))
		return 1;

	lower = smoothfs_lower_dentry(dentry);
	if (!lower)
		return 0;
	lower_ops = lower->d_op;
	if (!lower_ops || !lower_ops->d_revalidate)
		return 1;

	/* Lower actually has a revalidator — we need d_parent, which is
	 * not safe to chase under RCU-walk. Downgrade to ref-walk. */
	if (flags & LOOKUP_RCU)
		return -ECHILD;

	lower_parent = smoothfs_lower_dentry(dentry->d_parent);
	return smoothfs_compat_lower_d_revalidate(lower_ops,
		lower_parent ? d_inode(lower_parent) : NULL,
		name, lower, flags);
}

static void smoothfs_d_release(struct dentry *dentry)
{
	struct dentry *lower = smoothfs_lower_dentry(dentry);

	if (lower)
		dput(lower);
}

const struct dentry_operations smoothfs_dentry_ops = {
	.d_revalidate = smoothfs_d_revalidate,
	.d_release    = smoothfs_d_release,
};

/* ----------------------------------------------------------------- */
/* Operation tables                                                   */
/* ----------------------------------------------------------------- */

const struct inode_operations smoothfs_dir_inode_ops = {
	.lookup     = smoothfs_lookup,
	.create     = smoothfs_create,
	.mknod      = smoothfs_mknod,
	.symlink    = smoothfs_symlink,
	.link       = smoothfs_link,
	.unlink     = smoothfs_unlink,
	.mkdir      = smoothfs_mkdir,
	.rmdir      = smoothfs_rmdir,
	.rename     = smoothfs_rename,
	.getattr    = smoothfs_getattr,
	.setattr    = smoothfs_setattr,
	.listxattr  = smoothfs_listxattr,
#ifdef CONFIG_FS_POSIX_ACL
	.get_inode_acl = smoothfs_get_inode_acl,
	.set_acl       = smoothfs_set_acl,
#endif
};

const struct inode_operations smoothfs_file_inode_ops = {
	.getattr    = smoothfs_getattr,
	.setattr    = smoothfs_setattr,
	.listxattr  = smoothfs_listxattr,
#ifdef CONFIG_FS_POSIX_ACL
	.get_inode_acl = smoothfs_get_inode_acl,
	.set_acl       = smoothfs_set_acl,
#endif
};

const struct inode_operations smoothfs_symlink_inode_ops = {
	.get_link   = smoothfs_get_link,
	.getattr    = smoothfs_getattr,
	.setattr    = smoothfs_setattr,
	.listxattr  = smoothfs_listxattr,
};

const struct inode_operations smoothfs_special_inode_ops = {
	.getattr    = smoothfs_getattr,
	.setattr    = smoothfs_setattr,
	.listxattr  = smoothfs_listxattr,
};

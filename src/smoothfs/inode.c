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

static u8 smoothfs_select_high_water_tier(struct smoothfs_sb_info *sbi)
{
	struct kstatfs *stats;
	bool *stat_ok;
	u8 tier;
	u8 best_tier = sbi->fastest_tier;
	u64 max_blocks = 0;
	u64 best_avail = 0;
	u64 water;

	stats = kcalloc(sbi->ntiers, sizeof(*stats), GFP_KERNEL);
	if (!stats)
		return sbi->fastest_tier;
	stat_ok = kcalloc(sbi->ntiers, sizeof(*stat_ok), GFP_KERNEL);
	if (!stat_ok) {
		kfree(stats);
		return sbi->fastest_tier;
	}

	for (tier = 0; tier < sbi->ntiers; tier++) {
		int err = vfs_statfs(&sbi->tiers[tier].lower_path,
				     &stats[tier]);

		if (err || stats[tier].f_blocks == 0)
			continue;
		stat_ok[tier] = true;
		if (stats[tier].f_blocks > max_blocks)
			max_blocks = stats[tier].f_blocks;
		if (stats[tier].f_bavail > best_avail) {
			best_avail = stats[tier].f_bavail;
			best_tier = tier;
		}
	}
	water = max_blocks / 2;
	while (water > 0) {
		for (tier = 0; tier < sbi->ntiers; tier++) {
			if (!stat_ok[tier])
				continue;
			if (tier != sbi->ntiers - 1 &&
			    smoothfs_tier_near_enospc(sbi, tier))
				continue;
			if (stats[tier].f_bavail > water) {
				kfree(stat_ok);
				kfree(stats);
				return tier;
			}
		}
		water /= 2;
	}

	kfree(stat_ok);
	kfree(stats);
	return best_tier;
}

static u8 smoothfs_select_create_tier(struct smoothfs_sb_info *sbi)
{
	u8 tier;

	if (sbi->ntiers <= 1)
		return sbi->fastest_tier;

	if (READ_ONCE(sbi->create_policy) == SMOOTHFS_CREATE_POLICY_HIGH_WATER)
		return smoothfs_select_high_water_tier(sbi);

	/*
	 * SMOOTHFS_CREATE_POLICY_FASTEST: place new files on the fastest
	 * tier that still has space, falling through to slower tiers only
	 * when a faster one is at its ENOSPC watermark. Tiers are indexed in
	 * rank order (0 == fastest), so a fastest->slowest scan returns the
	 * first tier with room. The slowest tier is always eligible as a
	 * last resort — there is nowhere slower to fall to.
	 *
	 * This deliberately ignores per-tier write load. An earlier variant
	 * routed each new file to the least-loaded tier, which on a busy
	 * pool meant a fresh file frequently landed on an idle *slow* tier
	 * while its directory and siblings lived on the fast tier. Apps that
	 * write atomically via tmp+rename within one directory (Steam
	 * appmanifests, SQLite, dpkg, git, editors) then renamed across
	 * tiers, and a cross-tier rename(2) returns EXDEV — so the write
	 * failed. "Fastest tier with space" keeps a freshly created tmp
	 * co-located with its rename target on the same tier.
	 *
	 * write_staging is now orthogonal to placement: it controls the
	 * staging/drain machinery, not which tier a create lands on (the
	 * fastest-with-space scan already subsumes the old write_staging
	 * fast path of "fastest tier unless near ENOSPC").
	 */
	for (tier = 0; tier < sbi->ntiers; tier++) {
		if (tier == sbi->ntiers - 1)
			return tier;
		if (!smoothfs_tier_near_enospc(sbi, tier))
			return tier;
	}

	return sbi->fastest_tier;
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
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(si->vfs_inode.i_sb);
	char *dup = NULL;

	if (rel_path) {
		dup = kstrdup(rel_path, GFP_KERNEL);
		if (!dup)
			return -ENOMEM;
	}

	smoothfs_path_map_del(sbi, si);
	kfree(si->rel_path);
	si->rel_path = dup;
	smoothfs_path_map_add(sbi, si);
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

	{
		/* Apply attrs to the freshly staged backing object under
		 * privileged creds — it may be root-owned. See smoothfs_mknod. */
		const struct cred *old = override_creds(sbi->creator_cred);

		err = notify_change(idmap, lower, attr, NULL);
		revert_creds(old);
	}
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
	smoothfs_path_map_del(sbi, si);
	kfree(si->rel_path);
	si->rel_path = rel_dup;
	rel_dup = NULL;
	smoothfs_path_map_add(sbi, si);
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

int smoothfs_lookup_rel_across_tiers(struct smoothfs_sb_info *sbi,
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
			const struct cred *old_cred;

			/* The spill-tier root is root-owned; the calling user
			 * cannot mkdir under it. Run the namespace op under the
			 * mounter's privileged creds so the spill dir-chain is
			 * always created (its ownership is irrelevant — DAC is
			 * not enforced at this layer). */
			old_cred = override_creds(sbi->creator_cred);
			new_child = smoothfs_compat_mkdir(idmap, d_inode(cur.dentry),
							  child, 0755);
			revert_creds(old_cred);
			if (IS_ERR(new_child)) {
				err = PTR_ERR(new_child);
				/*
				 * vfs_mkdir() (6.15+) already dput()s the
				 * passed-in dentry when it returns an error
				 * pointer — a second dput here corrupts the
				 * dcache (dput() WARN at fs/dcache.c). It does
				 * NOT unlock the parent: that stays with us
				 * (see do_mkdirat, where end_creating_path()
				 * unlocks even on vfs_mkdir error).
				 */
				inode_unlock(d_inode(cur.dentry));
				goto out_err;
			}
			if (new_child != child) {
				/* vfs_mkdir() consumed the original on replace */
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

			/* The spill-tier dir is root-owned (created above under
			 * privileged creds). That is fine: DAC is not enforced
			 * at the smoothfs layer (see smoothfs_permission) and all
			 * lower-fs ops on it run privileged, so file creates and
			 * opens placed on this tier never get blocked by the
			 * backing ownership. No ownership fix-up is needed. */
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

/*
 * All backing operations — including lookups and the path traversal they do
 * across tier dirs — run under the mounter's privileged creds. The backing
 * objects are root-owned (created privileged), so a non-root caller (or any
 * caller, against a restrictive-mode root-owned dir like pressure-vessel's
 * 0700 var/) cannot even traverse them. The smoothfs-layer DAC was already
 * enforced by the VFS against the (uniformly-owned) smoothfs inode before we
 * get here; this only elevates the lower-fs access. See smoothfs_mknod.
 */
static struct dentry *smoothfs_lookup_inner(struct inode *dir,
					    struct dentry *dentry,
					    unsigned int flags);

static struct dentry *smoothfs_lookup(struct inode *dir, struct dentry *dentry,
				      unsigned int flags)
{
	const struct cred *old = override_creds(SMOOTHFS_SB(dir->i_sb)->creator_cred);
	struct dentry *ret = smoothfs_lookup_inner(dir, dentry, flags);

	revert_creds(old);
	return ret;
}

static struct dentry *smoothfs_lookup_inner(struct inode *dir,
					    struct dentry *dentry,
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
			/* Skip a stale alias whose lower backing was already
			 * removed. When a directory that smoothfs auto-created on
			 * more than one tier (to host spilled children) is
			 * rmdir'd, smoothfs_forget_placement clears the placement
			 * identity of only the canonical inode (d_inode(dentry))
			 * and purges the lower copies — but a spill-tier alias
			 * inode keeps this rel_path in the lookup map. Reusing it
			 * resurrects a zombie directory (lower purged) that still
			 * resolves and so blocks rename-into-place (e.g. Steam's
			 * runtime swap: rm steam-runtime then rename
			 * steam-runtime.tmp onto it).
			 *
			 * Detect the stale alias by the lower backing's link
			 * count, not d_really_is_negative: the purge rmdir's the
			 * lower dir, but the alias still pins that (now unhashed)
			 * lower dentry, so it stays *positive* with i_nlink == 0.
			 * The across-tiers scan below then yields a correct
			 * negative when no tier still backs the path. */
			if (replayed) {
				struct dentry *ld = replayed->lower_path.dentry;
				struct inode *li = ld ? d_inode(ld) : NULL;

				if (!li || li->i_nlink == 0)
					replayed = NULL;
			}
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

				/*
				 * Scan EVERY tier via fresh per-tier rel_path
				 * resolution, parent_tier included (SMOOTHFS_MAX_TIERS
				 * excludes nothing). Step 1 only consulted the parent's
				 * cached lower dentry; readdir (smoothfs_build_dir_cache)
				 * unions entries across ALL tiers by the same fresh
				 * resolution. When the parent's current_tier and the tier
				 * of its cached lower_path desync -- which cross-tier
				 * directory materialization, a remount that rebuilt the
				 * path index empty, or a concurrent cross-tier rename can
				 * cause -- excluding parent_tier here made lookup cover
				 * fewer tiers than readdir, surfacing a name as listed by
				 * `ls` yet unresolvable (-ENOENT) on stat/open/rename: a
				 * "phantom" entry. Covering all tiers makes lookup's
				 * coverage a provable superset of readdir's, so a listed
				 * entry can never be unresolvable for tier-coverage
				 * reasons. A hit on parent_tier (counted below) is exactly
				 * the case the old exclusion would have missed.
				 */
				err = smoothfs_lookup_rel_across_tiers(sbi, SMOOTHFS_MAX_TIERS,
								       rel_path,
								       &lower_path,
								       &found_tier);
				if (!err && found_tier == parent_tier)
					atomic64_inc(&sbi->parent_tier_lookup_recoveries);
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
	struct path lower_path;
	int err;

	if (!smoothfs_metadata_tier_active(sbi, READ_ONCE(si->current_tier))) {
		generic_fillattr(idmap, request_mask, inode, stat);
		stat->ino = inode->i_ino;
		stat->dev = inode->i_sb->s_dev;
		smoothfs_note_metadata_tier_skip(sbi);
		return 0;
	}

	/* Snapshot the lower path under the inode lock and hold a reference
	 * across the getattr, as open_lower_now does. A placement cutover
	 * replaces si->lower_path and then, after dropping inode_lock, dputs
	 * the old lower dentry and mntputs its vfsmount; tierd removes the
	 * source-tier copy behind it. An unreferenced snapshot taken here
	 * therefore races a migration two ways: vfs_getattr_nosec runs against
	 * an unlinked lower and returns -ESTALE for a file that plainly exists,
	 * or the cutover frees the dentry while it is still being dereferenced.
	 * The lock is a shared rwsem acquisition on the stat fastpath; it
	 * excludes only the exclusive cutover writer. */
	inode_lock_shared(inode);
	lower_path = si->lower_path;
	path_get(&lower_path);
	inode_unlock_shared(inode);

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
	path_put(&lower_path);
	if (err)
		return err;

	/* Preserve smoothfs's synthesised inode identity (§0.1). */
	stat->ino = inode->i_ino;
	stat->dev = inode->i_sb->s_dev;
	/* Present the uniform owner, matching what smoothfs_copy_attrs stored
	 * on the inode (the on-disk backing owner is root for spilled data). */
	stat->uid = SMOOTHFS_SB(inode->i_sb)->force_uid;
	stat->gid = SMOOTHFS_SB(inode->i_sb)->force_gid;
	return 0;
}

/* Privileged wrapper — covers the staging materialize/lookup + notify_change.
 * setattr_prepare (DAC) already ran against the smoothfs inode. See smoothfs_lookup. */
static int smoothfs_setattr_inner(struct mnt_idmap *idmap, struct dentry *dentry,
				  struct iattr *attr);

static int smoothfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			    struct iattr *attr)
{
	const struct cred *old =
		override_creds(SMOOTHFS_SB(dentry->d_sb)->creator_cred);
	int err = smoothfs_setattr_inner(idmap, dentry, attr);

	revert_creds(old);
	return err;
}

static int smoothfs_setattr_inner(struct mnt_idmap *idmap, struct dentry *dentry,
				  struct iattr *attr)
{
	struct dentry *lower = smoothfs_lower_dentry(dentry);
	struct inode *inode = d_inode(dentry);
	int err;
	int srcu_idx;

	/*
	 * Unlinked-but-open: smoothfs_unlink clears the dentry's d_fsdata,
	 * but the inode (and si->lower_path, which unlink deliberately
	 * keeps — see smoothfs_unlink_inner) stays live while anyone holds
	 * the file open. nfsd reaches ->setattr on exactly such a dentry:
	 * NFSv4.2 delegated timestamps make DELEGRETURN processing call
	 * nfsd4_finalize_deleg_timestamps -> notify_change on the held-open
	 * file after its last name was REMOVEd. Dereferencing the NULL
	 * d_fsdata here oopsed that nfsd thread WHILE IT HELD the inode's
	 * i_rwsem, orphaning the lock — every later op on the inode then
	 * D-state wedged (cthon04 nfsidem's second unlink; the mixed soak's
	 * mount). Fall back to si->lower_path exactly as ->getattr does.
	 */
	if (!lower)
		lower = smoothfs_lower_path(inode)->dentry;
	if (!lower)
		return -ESTALE;

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
	{
		/* setattr_prepare above already enforced DAC against the
		 * presented (uniform) owner. The backing object may be
		 * root-owned on a spill tier, so apply the change to the lower
		 * under privileged creds — otherwise chmod/utimes/chown on
		 * spilled data fails EPERM (e.g. tar setting mode/mtime on
		 * extracted files). See smoothfs_mknod. */
		const struct cred *old =
			override_creds(SMOOTHFS_SB(inode->i_sb)->creator_cred);
		err = notify_change(idmap, lower, attr, NULL);
		revert_creds(old);
	}
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

/* Privileged wrapper — covers the backing lookup + create. See smoothfs_lookup. */
static int smoothfs_create_inner(struct mnt_idmap *idmap, struct inode *dir,
				 struct dentry *dentry, umode_t mode, bool excl);

static int smoothfs_create(struct mnt_idmap *idmap, struct inode *dir,
			   struct dentry *dentry, umode_t mode, bool excl)
{
	const struct cred *old = override_creds(SMOOTHFS_SB(dir->i_sb)->creator_cred);
	int err = smoothfs_create_inner(idmap, dir, dentry, mode, excl);

	revert_creds(old);
	return err;
}

static int smoothfs_create_inner(struct mnt_idmap *idmap, struct inode *dir,
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
		{
			/* Parent may be a root-owned spill dir; create the
			 * backing file under privileged creds (DAC is not
			 * enforced at this layer — see smoothfs_permission). */
			const struct cred *old_cred =
				override_creds(sbi->creator_cred);
			err = smoothfs_compat_create(idmap,
						     d_inode(parent_path.dentry),
						     lower, mode, excl);
			revert_creds(old_cred);
		}
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

/*
 * Backing namespace mutations (create/remove/rename) must run under the
 * mounter's privileged creds. A target may live on a spill tier whose backing
 * dir is root-owned (materialized under creator_cred), so the calling user
 * cannot create within, unlink from, rmdir, or rename it otherwise — this is
 * what left steam.sh's "rm -rf steam-runtime; mv tmp final" failing forever.
 * Ownership is still presented uniformly (smoothfs_copy_attrs) and DAC is
 * enforced against that presented owner; only the lower-fs op is elevated.
 * Each op below is a thin privileged wrapper around an _inner implementation.
 */
static int smoothfs_mknod_inner(struct mnt_idmap *idmap, struct inode *dir,
				struct dentry *dentry, umode_t mode, dev_t rdev);

static int smoothfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
			  struct dentry *dentry, umode_t mode, dev_t rdev)
{
	const struct cred *old = override_creds(SMOOTHFS_SB(dir->i_sb)->creator_cred);
	int err = smoothfs_mknod_inner(idmap, dir, dentry, mode, rdev);

	revert_creds(old);
	return err;
}

static int smoothfs_mknod_inner(struct mnt_idmap *idmap, struct inode *dir,
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

/* Privileged wrapper — see smoothfs_mknod. */
static int smoothfs_symlink_inner(struct mnt_idmap *idmap, struct inode *dir,
				  struct dentry *dentry, const char *symname);

static int smoothfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			    struct dentry *dentry, const char *symname)
{
	const struct cred *old = override_creds(SMOOTHFS_SB(dir->i_sb)->creator_cred);
	int err = smoothfs_symlink_inner(idmap, dir, dentry, symname);

	revert_creds(old);
	return err;
}

static int smoothfs_symlink_inner(struct mnt_idmap *idmap, struct inode *dir,
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
/* Privileged wrapper — see smoothfs_mknod. */
static int smoothfs_link_inner(struct dentry *old_dentry, struct inode *dir,
			       struct dentry *dentry);

static int smoothfs_link(struct dentry *old_dentry, struct inode *dir,
			 struct dentry *dentry)
{
	const struct cred *old = override_creds(SMOOTHFS_SB(dir->i_sb)->creator_cred);
	int err = smoothfs_link_inner(old_dentry, dir, dentry);

	revert_creds(old);
	return err;
}

static int smoothfs_link_inner(struct dentry *old_dentry, struct inode *dir,
			       struct dentry *dentry)
{
	struct dentry *lower_old = smoothfs_lower_dentry(old_dentry);
	struct dentry *lower_parent = smoothfs_lower_dentry(dentry->d_parent);
	struct smoothfs_inode_info *si = SMOOTHFS_I(d_inode(old_dentry));
	struct dentry *lower;
	int err;

	/* smoothfs_unlink_inner clears d_fsdata on success, so a cached
	 * dentry for a just-unlinked name returns NULL here. Dereferencing
	 * it oopsed the kernel from an unprivileged link(2):
	 *   BUG: kernel NULL pointer dereference, address: 0x68
	 *   RIP: smoothfs_link+0x75  (lower_old->d_sb)
	 * The source name is gone, which is exactly ENOENT. */
	if (!lower_old || !lower_parent)
		return -ENOENT;

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

/* Privileged wrapper — see smoothfs_mknod. */
static int smoothfs_unlink_inner(struct inode *dir, struct dentry *dentry);

static int smoothfs_unlink(struct inode *dir, struct dentry *dentry)
{
	const struct cred *old = override_creds(SMOOTHFS_SB(dir->i_sb)->creator_cred);
	int err = smoothfs_unlink_inner(dir, dentry);

	revert_creds(old);
	return err;
}

static int smoothfs_unlink_inner(struct inode *dir, struct dentry *dentry)
{
	struct dentry *lower = smoothfs_lower_dentry(dentry);
	struct dentry *removing;
	struct inode *lower_dir = NULL;
	struct smoothfs_inode_info *si = SMOOTHFS_I(d_inode(dentry));
	struct inode *lower_inode;
	int err;

	/* Already unlinked: d_fsdata was cleared below on a previous pass. */
	if (!lower)
		return -ENOENT;

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
	if (IS_ERR(removing)) {
		/* -ENOENT means start_removing revalidated the name under the
		 * parent lock and found the lower entry already gone. Our upper
		 * dentry is stale rather than wrong, so fall through to the same
		 * bookkeeping the i_nlink == 0 case below uses: the caller asked
		 * for this name to be gone and it is. */
		if (PTR_ERR(removing) != -ENOENT)
			return PTR_ERR(removing);
		err = 0;
	} else {
		lower_inode = d_inode(removing);
		if (lower_inode && lower_inode->i_nlink == 0)
			err = 0;
		else
			err = vfs_unlink(&nop_mnt_idmap, lower_dir, removing,
					 NULL);
		smoothfs_compat_end_removing(removing, lower_dir);
	}

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
		/* Last link gone: drop the placement identity so a stale
		 * si->rel_path cannot resurrect this backing-less inode on the
		 * next lookup of the path. A surviving hardlink keeps i_nlink
		 * > 0, where the inode is still reachable via its other name. */
		if (d_inode(dentry)->i_nlink == 0)
			smoothfs_forget_placement(dir->i_sb, d_inode(dentry),
						  false);
	}
	return err;
}

/* Privileged wrapper — covers the backing lookup + mkdir. See smoothfs_lookup. */
static struct dentry *smoothfs_mkdir_inner(struct mnt_idmap *idmap,
					   struct inode *dir,
					   struct dentry *dentry, umode_t mode);

static struct dentry *smoothfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				     struct dentry *dentry, umode_t mode)
{
	const struct cred *old = override_creds(SMOOTHFS_SB(dir->i_sb)->creator_cred);
	struct dentry *ret = smoothfs_mkdir_inner(idmap, dir, dentry, mode);

	revert_creds(old);
	return ret;
}

static struct dentry *smoothfs_mkdir_inner(struct mnt_idmap *idmap,
					   struct inode *dir,
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
			const struct cred *old_cred =
				override_creds(sbi->creator_cred);
			struct dentry *new_lower = smoothfs_compat_mkdir(idmap,
							d_inode(parent_path.dentry),
							lower, mode);
			revert_creds(old_cred);
			if (IS_ERR(new_lower)) {
				err = PTR_ERR(new_lower);
				/*
				 * vfs_mkdir() already dput()'d lower and
				 * unlocked the parent on error; do not repeat.
				 */
				path_put(&parent_path);
				if (err == -ENOSPC)
					continue;
				goto out_err;
			}
			inode_unlock(d_inode(parent_path.dentry));
			if (new_lower != lower) {
				/* vfs_mkdir() consumed the original on replace */
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

/* Privileged wrapper — see smoothfs_mknod. */
static int smoothfs_rmdir_inner(struct inode *dir, struct dentry *dentry);

static int smoothfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	const struct cred *old = override_creds(SMOOTHFS_SB(dir->i_sb)->creator_cred);
	int err = smoothfs_rmdir_inner(dir, dentry);

	revert_creds(old);
	return err;
}

static int smoothfs_rmdir_inner(struct inode *dir, struct dentry *dentry)
{
	struct dentry *lower = smoothfs_lower_dentry(dentry);
	struct dentry *removing;
	struct inode *lower_dir = NULL;
	int err;

	/* See smoothfs_link_inner: d_fsdata is cleared on a successful
	 * removal, so a cached dentry can reach us with no lower. */
	if (!lower)
		return -ENOENT;

	/* See smoothfs_unlink for the reason we use lower->d_parent rather
	 * than smoothfs_lower_dentry(dentry->d_parent): the latter is the
	 * canonical-tier parent and may live on a different lower fs from
	 * the directory we are removing, which trips the dentry-parent
	 * identity check inside smoothfs_compat_start_removing. */
	removing = smoothfs_compat_start_removing(lower->d_parent, lower, &lower_dir);
	if (IS_ERR(removing)) {
		/* See smoothfs_unlink_inner: -ENOENT is a stale upper dentry
		 * over a lower directory that is already gone, not a failure. */
		if (PTR_ERR(removing) != -ENOENT)
			return PTR_ERR(removing);
		err = 0;
	} else {
		err = smoothfs_compat_rmdir(&nop_mnt_idmap, lower_dir, removing);
		smoothfs_compat_end_removing(removing, lower_dir);
	}

	if (!err) {
		/* Drop d_fsdata so this dentry releases its pin on the
		 * lower dentry; si->lower_path stays until evict_inode.
		 * See smoothfs_unlink for the full rationale. */
		struct inode *removed = d_inode(dentry);
		struct smoothfs_inode_info *rsi = SMOOTHFS_I(removed);
		struct smoothfs_sb_info *sbi = SMOOTHFS_SB(dir->i_sb);

		/* A directory resolved via the canonical lower (a plain lookup
		 * hit) reaches here with si->rel_path unset — only the replay
		 * and across-tiers fallbacks call smoothfs_track_placed. Without
		 * rel_path, smoothfs_forget_placement below skips its spill-tier
		 * purge, so for a directory mirrored onto more than one tier an
		 * empty copy survives on a non-canonical tier and the next
		 * lookup resurrects it as a zombie (nlink-2, lower-backed) that
		 * blocks rename-into-place (e.g. Steam's runtime swap). Backfill
		 * rel_path from the dentry (always available here) so the purge
		 * runs. */
		if (!rsi->rel_path) {
			char *rel = smoothfs_rel_path_from_dentry(dentry);

			if (rel) {
				down_write(&sbi->inode_lock);
				if (!rsi->rel_path)
					rsi->rel_path = rel;
				else
					kfree(rel);
				up_write(&sbi->inode_lock);
			}
		}

		clear_nlink(removed);
		smoothfs_set_lower_dentry(dentry, NULL);
		d_drop(dentry);
		/* Copy from the parent we actually modified (lower->d_parent);
		 * see smoothfs_unlink for the canonical-vs-actual rationale. */
		smoothfs_copy_attrs(dir, d_inode(lower->d_parent));
		/* Purge the directory's empty copies on every spill tier
		 * (smoothfs_compat_rmdir above removed only the canonical one)
		 * and drop the placement identity + replay pin, so neither a
		 * surviving spill copy nor a stale si->rel_path resurrects the
		 * directory on the next lookup. */
		smoothfs_forget_placement(dir->i_sb, removed, true);
	}
	return err;
}

/*
 * A same-directory rename whose source and existing target landed on
 * different tiers is completed by renaming the source within its own tier
 * to the target's name (no data copy — the source already holds the new
 * content on its tier). This leaves a stale lower copy of the target on
 * its original tier, which must go: a copy on the canonical tier would
 * shadow the renamed file (dual-resolution), and a spill copy would leak.
 * Mirrors smoothfs_unlink's lower removal, then forgets the displaced
 * target inode's placement so the rel_path fallback — now reached because
 * the canonical name went negative — resolves to the renamed source rather
 * than this stale inode. The inode's lower_ino_map entry and lower_path are
 * released when it evicts; we must not touch si->lower_path here.
 */
static void smoothfs_drop_stale_rename_target(struct super_block *sb,
					      struct dentry *stale_lower,
					      struct inode *stale_inode)
{
	struct dentry *removing;
	struct inode *lower_dir = NULL;

	removing = smoothfs_compat_start_removing(stale_lower->d_parent,
						  stale_lower, &lower_dir);
	if (!IS_ERR(removing)) {
		struct inode *lower_inode = d_inode(removing);
		int uerr = 0;

		if (!(lower_inode && lower_inode->i_nlink == 0))
			uerr = vfs_unlink(&nop_mnt_idmap, lower_dir, removing,
					  NULL);
		smoothfs_compat_end_removing(removing, lower_dir);
		/* This is best-effort cleanup, so we still do not fail the
		 * rename on it. But swallowing the result silently hid a
		 * filesystem shutdown once; a failure here is worth a line. */
		if (uerr && uerr != -ENOENT)
			pr_warn("smoothfs: stale rename target unlink failed: %d\n",
				uerr);
	} else if (PTR_ERR(removing) != -ENOENT) {
		pr_warn("smoothfs: stale rename target not removable: %ld\n",
			PTR_ERR(removing));
	}

	if (stale_inode)
		smoothfs_forget_placement(sb, stale_inode, false);
}

/* Privileged wrapper — see smoothfs_mknod. */
static int smoothfs_rename_inner(struct mnt_idmap *idmap,
				 struct inode *old_dir, struct dentry *old_dentry,
				 struct inode *new_dir, struct dentry *new_dentry,
				 unsigned int flags);

static int smoothfs_rename(struct mnt_idmap *idmap,
			   struct inode *old_dir, struct dentry *old_dentry,
			   struct inode *new_dir, struct dentry *new_dentry,
			   unsigned int flags)
{
	const struct cred *old = override_creds(SMOOTHFS_SB(old_dir->i_sb)->creator_cred);
	int err = smoothfs_rename_inner(idmap, old_dir, old_dentry,
					new_dir, new_dentry, flags);

	revert_creds(old);
	return err;
}

static int smoothfs_rename_inner(struct mnt_idmap *idmap,
				 struct inode *old_dir, struct dentry *old_dentry,
				 struct inode *new_dir, struct dentry *new_dentry,
				 unsigned int flags)
{
	struct dentry *lower_old_parent = smoothfs_lower_dentry(old_dentry->d_parent);
	struct dentry *lower_new_parent = smoothfs_lower_dentry(new_dentry->d_parent);
	struct dentry *lower_old = smoothfs_lower_dentry(old_dentry);
	struct dentry *lower_new = smoothfs_lower_dentry(new_dentry);
	struct dentry *actual_old_parent = lower_old ? lower_old->d_parent : lower_old_parent;
	struct dentry *actual_new_parent = lower_new_parent;
	struct renamedata rd = {};
	char *spill_old_rel = NULL;
	bool spill_is_dir = lower_old && d_is_dir(lower_old);
	struct dentry *stale_lower = NULL;
	struct inode *stale_inode = NULL;
	struct path mat_new_parent = {};
	int err;

	/*
	 * Same-directory rename: complete it on the source's own tier no
	 * matter which tiers the source and the (possibly pre-existing)
	 * target landed on. A file or directory can live on a non-canonical
	 * tier while its visible parent still maps to the canonical tier —
	 * Samba's mkdir-then-rename of a temp name, or an atomic tmp+rename
	 * whose tmp spilled off a full fast tier onto a slower tier than the
	 * file it replaces. Renaming within the source's tier needs no data
	 * copy (the source already holds the new content there) and keeps the
	 * result co-located with its name, so apps that write atomically via
	 * tmp+rename (Steam appmanifests, SQLite, dpkg, git, editors) never
	 * see EXDEV on a same-directory rename.
	 *
	 * If an existing target sits on a different tier than the source,
	 * stash its lower and inode so the stale copy is dropped after the
	 * rename (smoothfs_drop_stale_rename_target). Cross-tier rename of a
	 * directory over an existing directory keeps returning EXDEV — that
	 * has merge/rmdir semantics this no-copy path does not implement — as
	 * do RENAME_EXCHANGE/WHITEOUT. Cross-DIRECTORY renames are completed
	 * the same no-copy way by the block further down.
	 */
	if (old_dentry->d_parent == new_dentry->d_parent) {
		if (lower_new && lower_new->d_sb != actual_old_parent->d_sb) {
			if (d_really_is_positive(lower_new)) {
				if (d_is_dir(lower_new) ||
				    (flags & (RENAME_EXCHANGE | RENAME_WHITEOUT)))
					return -EXDEV;
				if (flags & RENAME_NOREPLACE)
					return -EEXIST;
				stale_lower = dget(lower_new);
				stale_inode = d_inode(new_dentry);
			}
			smoothfs_set_lower_dentry(new_dentry, NULL);
			lower_new = NULL;
		}
		actual_new_parent = actual_old_parent;
	}

	/* Cross-DIRECTORY cross-tier rename: the destination parent resolved
	 * onto a different tier than the source (e.g. source on fast, dest dir
	 * is a tier-fallthrough hit on slow). vfs_rename across lower
	 * superblocks is invalid, and SMB/NFS servers and raw rename(2) callers
	 * do NOT fall back to copy+delete the way Windows Explorer does — they
	 * surface EXDEV/NT_STATUS_NOT_SAME_DEVICE and the move fails, even
	 * though both paths live in one smoothfs mount. Complete it on the
	 * source's own tier exactly as the same-directory block above does:
	 * materialize the destination parent's directory chain on the source's
	 * tier, then rename within that tier. No byte is copied — the source
	 * already holds its data there — and the moved name then resolves via
	 * smoothfs_lookup's across-tiers rel_path scan, like any spilled file.
	 * RENAME_EXCHANGE/WHITEOUT and renaming a directory over an existing
	 * one need merge semantics this no-copy path does not implement, so
	 * those keep returning EXDEV. */
	if (actual_old_parent->d_sb != actual_new_parent->d_sb) {
		struct smoothfs_sb_info *sbi = SMOOTHFS_SB(old_dir->i_sb);
		char *new_parent_rel;
		u8 src_tier;

		if (flags & (RENAME_EXCHANGE | RENAME_WHITEOUT))
			return -EXDEV;

		for (src_tier = 0; src_tier < sbi->ntiers; src_tier++)
			if (sbi->tiers[src_tier].lower_path.dentry->d_sb ==
			    actual_old_parent->d_sb)
				break;
		if (src_tier >= sbi->ntiers)
			return -EXDEV;

		/* An existing target on a different tier becomes stale once the
		 * source is renamed into place on its own tier — stash it for
		 * smoothfs_drop_stale_rename_target, mirroring the same-dir
		 * path. A target directory needs merge semantics: bail. */
		if (lower_new && d_really_is_positive(lower_new)) {
			if (d_is_dir(lower_new))
				return -EXDEV;
			if (flags & RENAME_NOREPLACE)
				return -EEXIST;
			stale_lower = dget(lower_new);
			stale_inode = d_inode(new_dentry);
		}

		new_parent_rel = smoothfs_rel_path_from_dentry(new_dentry->d_parent);
		if (!new_parent_rel) {
			dput(stale_lower);
			return -ENOMEM;
		}
		err = smoothfs_materialize_parent_on_tier(idmap, old_dir->i_sb,
							  sbi, src_tier,
							  new_parent_rel,
							  &mat_new_parent);
		/* A directory carries spill-tier copies that the post-rename
		 * smoothfs_rename_spill_tiers relocates to match; that helper
		 * skips any tier whose destination parent is absent, so
		 * pre-create the parent on every other active tier too. */
		if (!err && spill_is_dir) {
			u8 t;

			for (t = 0; t < sbi->ntiers; t++) {
				struct path tmp;

				if (t == src_tier ||
				    !smoothfs_metadata_tier_active(sbi, t))
					continue;
				if (!smoothfs_materialize_parent_on_tier(idmap,
						old_dir->i_sb, sbi, t,
						new_parent_rel, &tmp))
					path_put(&tmp);
			}
		}
		kfree(new_parent_rel);
		if (err) {
			dput(stale_lower);
			return err;
		}

		/* Rename now happens entirely within the source's tier; the
		 * destination resolves under the just-materialized parent. */
		actual_new_parent = mat_new_parent.dentry;
		smoothfs_set_lower_dentry(new_dentry, NULL);
		lower_new = NULL;
	} else if (lower_old && lower_new &&
		   lower_old->d_sb != lower_new->d_sb) {
		/* Parents share a tier but an existing target name resolved to
		 * a third tier — the no-copy path cannot express renaming the
		 * source onto a lower on a different superblock. Rare; fall
		 * back to EXDEV as before. */
		return -EXDEV;
	}
	if (lower_new)
		dget(lower_new);

	/*
	 * Lock the parents (looking up the target by name when it isn't already
	 * resolved), then rename. smoothfs_compat_start_rename absorbs the 6.19
	 * VFS directory-locking rework that made lock_rename_child()/
	 * unlock_rename() module-internal, and does the trap/unhashed/NOREPLACE
	 * validation this used to open-code. lower_new keeps its own ref (dget
	 * above) for the post-rename fixups; rd gets its own refs, released by
	 * smoothfs_compat_finish_rename.
	 */
	rd.mnt_idmap  = idmap;
	rd.old_parent = actual_old_parent;
	rd.new_parent = actual_new_parent;
	rd.flags      = flags;
	err = smoothfs_compat_start_rename(&rd, lower_old, lower_new,
					   &new_dentry->d_name);
	if (err) {
		if (lower_new)
			dput(lower_new);
		path_put(&mat_new_parent);
		return err;
	}

	/* Capture the source path before vfs_rename d_moves old_dentry, so the
	 * spill-tier copies of a renamed directory can be moved to match (see
	 * smoothfs_rename_spill_tiers below). */
	if (spill_is_dir)
		spill_old_rel = smoothfs_rel_path_from_dentry(old_dentry);

	err = vfs_rename(&rd);
	smoothfs_compat_finish_rename(&rd);
	if (err) {
		kfree(spill_old_rel);
		dput(lower_new);
		dput(stale_lower);
		path_put(&mat_new_parent);
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
	smoothfs_copy_attrs(old_dir, d_inode(actual_old_parent));
	smoothfs_copy_attrs(new_dir, d_inode(actual_new_parent));

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
			struct smoothfs_sb_info *sbi = SMOOTHFS_SB(old_dir->i_sb);

			si = SMOOTHFS_I(moved_inode);
			down_write(&sbi->inode_lock);
			smoothfs_path_map_del(sbi, si);
			kfree(si->rel_path);
			si->rel_path = new_rel;
			smoothfs_path_map_add(sbi, si);
			up_write(&sbi->inode_lock);
		} else {
			kfree(new_rel);
		}
	}

	/* Same-directory cross-tier rename onto an existing target: the source
	 * was renamed into place on its own tier above, so drop the target's
	 * now-stale lower on its original tier (and forget its placement) — a
	 * canonical-tier copy would shadow the renamed file and a spill copy
	 * would leak. Done after the source's rel_path is re-keyed above so the
	 * new name resolves to the source throughout. */
	if (stale_lower) {
		smoothfs_drop_stale_rename_target(old_dir->i_sb, stale_lower,
						  stale_inode);
		dput(stale_lower);
	}

	/* smoothfs_rename above renamed only the canonical-tier lower. A
	 * directory smoothfs replicated onto spill tiers still has its copy
	 * under the OLD name there, so the renamed directory is missing its
	 * spill-tier contents until this moves them. (Files live on a single
	 * tier, so only directories need this.) */
	if (spill_is_dir && spill_old_rel) {
		char *new_rel = smoothfs_rel_path_from_dentry(new_dentry);

		if (new_rel) {
			smoothfs_rename_spill_tiers(SMOOTHFS_SB(old_dir->i_sb),
						   spill_old_rel, new_rel);
			kfree(new_rel);
		}
	}
	kfree(spill_old_rel);
	path_put(&mat_new_parent);
	return 0;
}

/*
 * .permission is deliberately NOT installed. The VFS falls back to
 * generic_permission(inode, mask) on the smoothfs inode. Ownership is
 * presented uniformly as the appliance's primary user (see
 * smoothfs_force_owner / SMOOTHFS_FORCE_UID) so that user always passes
 * the owner check regardless of which tier a file's backing landed on —
 * spilled backing objects are created under privileged creds and are
 * thus root-owned on disk, but that is invisible here. Other uids remain
 * subject to the mode bits, so DAC is still enforced.
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

	/*
	 * A negative result is only a snapshot of the union at lookup time.
	 * Backing files can appear later without going through this superblock:
	 * tierd moves/copies objects between lowers out-of-band, and an inactive
	 * metadata tier can become visible again.  None of those operations
	 * invalidates the smoothfs dcache entry.  Trusting the negative forever
	 * therefore leaves a file readable on a lower but permanently ENOENT in
	 * the merged mount until drop_caches or remount.
	 *
	 * Force negative dentries back through ->lookup, which performs the fresh
	 * all-active-tiers scan.  The test is RCU-walk safe and avoids penalising
	 * positive dentries, which retain the lower-revalidation fast path below.
	 */
	if (d_really_is_negative(dentry))
		return 0;

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

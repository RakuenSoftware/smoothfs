// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - superblock ops and mount via fs_context.
 *
 * Mount syntax (Phase 1):
 *   mount -t smoothfs -o pool=<name>,uuid=<uuid>,tiers=<path>[:<path>...]
 *         none /mnt/<pool>
 *
 * The first path in 'tiers' is the fastest tier (rank 0). The placement
 * log lives on the fastest tier per Phase 0 §0.2.
 */

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/namei.h>
#include <linux/limits.h>
#include <linux/parser.h>
#include <linux/rhashtable.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uuid.h>
#include <linux/version.h>
#include <linux/xxhash.h>

#include "smoothfs.h"

#define SMOOTHFS_DEFAULT_FULL_PCT 98

extern struct kobject *smoothfs_sysfs_root;

struct smoothfs_sysfs_pool {
	struct kobject         kobj;
	struct smoothfs_sb_info *sbi;
};

static inline struct smoothfs_sysfs_pool *to_smoothfs_sysfs_pool(struct kobject *kobj)
{
	return container_of(kobj, struct smoothfs_sysfs_pool, kobj);
}

static void smoothfs_write_staging_set_reason(struct smoothfs_sb_info *sbi,
					      const char *reason);
static int smoothfs_write_staging_drain_rehomes(struct smoothfs_sb_info *sbi);

static ssize_t spill_creates_total_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%lld\n",
			  (long long)atomic64_read(&pool->sbi->spill_creates_total));
}

static ssize_t spill_creates_failed_all_tiers_show(struct kobject *kobj,
						   struct kobj_attribute *attr,
						   char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%lld\n",
		(long long)atomic64_read(&pool->sbi->spill_creates_failed_all_tiers));
}

static ssize_t any_spill_since_mount_show(struct kobject *kobj,
					  struct kobj_attribute *attr, char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%d\n",
			  atomic_read(&pool->sbi->any_spill_since_mount) ? 1 : 0);
}

static ssize_t write_staging_supported_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	return sysfs_emit(buf, "1\n");
}

static ssize_t write_staging_enabled_show(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%d\n",
			  READ_ONCE(pool->sbi->write_staging_enabled) ? 1 : 0);
}

static ssize_t write_staging_enabled_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);
	bool enabled;
	int err;

	err = kstrtobool(buf, &enabled);
	if (err)
		return err;
	WRITE_ONCE(pool->sbi->write_staging_enabled, enabled);
	return count;
}

static ssize_t write_staging_full_pct_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%u\n",
			  (unsigned int)READ_ONCE(pool->sbi->write_staging_full_pct));
}

static ssize_t write_staging_full_pct_store(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    const char *buf, size_t count)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);
	unsigned int pct;
	int err;

	err = kstrtouint(buf, 10, &pct);
	if (err)
		return err;
	if (pct < 1 || pct > 100)
		return -EINVAL;
	WRITE_ONCE(pool->sbi->write_staging_full_pct, (u8)pct);
	return count;
}

static ssize_t staged_bytes_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%lld\n",
			  (long long)atomic64_read(&pool->sbi->staged_bytes));
}

static ssize_t staged_rehome_bytes_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%lld\n",
		(long long)atomic64_read(&pool->sbi->staged_rehome_bytes));
}

static ssize_t range_staged_bytes_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%lld\n",
		(long long)atomic64_read(&pool->sbi->range_staged_bytes));
}

static ssize_t range_staged_writes_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%lld\n",
		(long long)atomic64_read(&pool->sbi->range_staged_writes));
}

static ssize_t staged_rehomes_total_show(struct kobject *kobj,
					 struct kobj_attribute *attr, char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%lld\n",
		(long long)atomic64_read(&pool->sbi->staged_rehomes_total));
}

static unsigned int smoothfs_write_staging_rehome_count(struct smoothfs_sb_info *sbi,
							bool drainable_only)
{
	struct smoothfs_inode_info *si;
	unsigned int count = 0;

	down_read(&sbi->inode_lock);
	list_for_each_entry(si, &sbi->inode_list, sb_link) {
		u8 tier;

		if (!READ_ONCE(si->write_staged))
			continue;
		if (!drainable_only) {
			count++;
			continue;
		}
		tier = READ_ONCE(si->write_staged_drain_tier);
		if (smoothfs_write_staging_drain_tier_active(sbi, tier))
			count++;
	}
	up_read(&sbi->inode_lock);
	return count;
}

static unsigned int smoothfs_write_staging_pending_rehomes(struct smoothfs_sb_info *sbi)
{
	return smoothfs_write_staging_rehome_count(sbi, false);
}

static ssize_t staged_rehomes_pending_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%u\n",
		smoothfs_write_staging_pending_rehomes(pool->sbi));
}

static unsigned int smoothfs_write_staging_drainable_rehomes(struct smoothfs_sb_info *sbi)
{
	return smoothfs_write_staging_rehome_count(sbi, true);
}

static ssize_t write_staging_drainable_rehomes_show(struct kobject *kobj,
						    struct kobj_attribute *attr,
						    char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%u\n",
		smoothfs_write_staging_drainable_rehomes(pool->sbi));
}

static bool smoothfs_write_staging_has_work(struct smoothfs_sb_info *sbi)
{
	return atomic64_read(&sbi->staged_bytes) > 0 ||
	       smoothfs_write_staging_pending_rehomes(sbi) > 0;
}

static bool smoothfs_write_staging_fastest_pressure(struct smoothfs_sb_info *sbi)
{
	struct kstatfs st;
	u8 tier = sbi->fastest_tier;
	u8 full_pct = READ_ONCE(sbi->write_staging_full_pct);
	int err;

	if (!smoothfs_write_staging_has_work(sbi))
		return false;
	if (tier >= sbi->ntiers)
		return false;

	err = vfs_statfs(&sbi->tiers[tier].lower_path, &st);
	if (err || st.f_blocks == 0)
		return false;

	if (full_pct == 0 || full_pct > 100)
		full_pct = SMOOTHFS_DEFAULT_FULL_PCT;
	return (st.f_blocks - st.f_bavail) * 100 >= st.f_blocks * full_pct;
}

static ssize_t write_staging_drain_pressure_show(struct kobject *kobj,
						 struct kobj_attribute *attr,
						 char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%d\n",
		smoothfs_write_staging_fastest_pressure(pool->sbi) ? 1 : 0);
}

static ssize_t write_staging_drainable_tier_mask_show(struct kobject *kobj,
						      struct kobj_attribute *attr,
						      char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);
	struct smoothfs_sb_info *sbi = pool->sbi;
	u32 mask = 0;
	u32 valid_mask;

	if (smoothfs_write_staging_has_work(sbi) &&
	    sbi->ntiers > 0 && sbi->ntiers <= SMOOTHFS_MAX_TIERS) {
		valid_mask = BIT(sbi->ntiers) - 1;
		mask = READ_ONCE(sbi->write_staging_drain_active_tier_mask);
		mask &= valid_mask;
		mask &= ~BIT(sbi->fastest_tier);
	}

	return sysfs_emit(buf, "0x%x\n", mask);
}

static ssize_t oldest_staged_write_at_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%lld\n",
		(long long)atomic64_read(&pool->sbi->oldest_staged_write_ns));
}

static ssize_t last_drain_at_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%lld\n",
			  (long long)atomic64_read(&pool->sbi->last_drain_ns));
}

static ssize_t last_drain_reason_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);
	struct smoothfs_sb_info *sbi = pool->sbi;
	char reason[sizeof(sbi->last_drain_reason)];

	spin_lock(&sbi->write_staging_lock);
	strscpy(reason, sbi->last_drain_reason, sizeof(reason));
	spin_unlock(&sbi->write_staging_lock);
	return sysfs_emit(buf, "%s\n", reason);
}

static ssize_t metadata_active_tier_mask_show(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "0x%x\n",
			  READ_ONCE(pool->sbi->metadata_active_tier_mask));
}

static ssize_t metadata_active_tier_mask_store(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       const char *buf, size_t count)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);
	struct smoothfs_sb_info *sbi = pool->sbi;
	u32 mask;
	u32 valid_mask;
	int err;

	err = kstrtou32(buf, 0, &mask);
	if (err)
		return err;
	if (sbi->ntiers == 0 || sbi->ntiers > SMOOTHFS_MAX_TIERS)
		return -EINVAL;

	valid_mask = BIT(sbi->ntiers) - 1;
	mask &= valid_mask;
	mask |= BIT(sbi->fastest_tier);
	WRITE_ONCE(sbi->metadata_active_tier_mask, mask);
	return count;
}

static ssize_t write_staging_drain_active_tier_mask_show(struct kobject *kobj,
							 struct kobj_attribute *attr,
							 char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "0x%x\n",
		READ_ONCE(pool->sbi->write_staging_drain_active_tier_mask));
}

static ssize_t write_staging_drain_active_tier_mask_store(struct kobject *kobj,
							  struct kobj_attribute *attr,
							  const char *buf,
							  size_t count)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);
	struct smoothfs_sb_info *sbi = pool->sbi;
	u32 mask;
	u32 valid_mask;
	int err;

	err = kstrtou32(buf, 0, &mask);
	if (err)
		return err;
	if (sbi->ntiers == 0 || sbi->ntiers > SMOOTHFS_MAX_TIERS)
		return -EINVAL;

	valid_mask = BIT(sbi->ntiers) - 1;
	mask &= valid_mask;
	mask |= BIT(sbi->fastest_tier);
	WRITE_ONCE(sbi->write_staging_drain_active_tier_mask, mask);
	err = smoothfs_write_staging_drain_rehomes(sbi);
	if (err)
		pr_warn_ratelimited("smoothfs: write-staging rehome drain failed: %d\n",
				    err);
	return count;
}

static ssize_t metadata_tier_skips_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct smoothfs_sysfs_pool *pool = to_smoothfs_sysfs_pool(kobj);

	return sysfs_emit(buf, "%lld\n",
		(long long)atomic64_read(&pool->sbi->metadata_tier_skips));
}

static struct kobj_attribute spill_creates_total_attr =
	__ATTR_RO(spill_creates_total);
static struct kobj_attribute spill_creates_failed_all_tiers_attr =
	__ATTR_RO(spill_creates_failed_all_tiers);
static struct kobj_attribute any_spill_since_mount_attr =
	__ATTR_RO(any_spill_since_mount);
static struct kobj_attribute write_staging_supported_attr =
	__ATTR_RO(write_staging_supported);
static struct kobj_attribute write_staging_enabled_attr =
	__ATTR_RW(write_staging_enabled);
static struct kobj_attribute write_staging_full_pct_attr =
	__ATTR_RW(write_staging_full_pct);
static struct kobj_attribute staged_bytes_attr =
	__ATTR_RO(staged_bytes);
static struct kobj_attribute staged_rehome_bytes_attr =
	__ATTR_RO(staged_rehome_bytes);
static struct kobj_attribute range_staged_bytes_attr =
	__ATTR_RO(range_staged_bytes);
static struct kobj_attribute range_staged_writes_attr =
	__ATTR_RO(range_staged_writes);
static struct kobj_attribute staged_rehomes_total_attr =
	__ATTR_RO(staged_rehomes_total);
static struct kobj_attribute staged_rehomes_pending_attr =
	__ATTR_RO(staged_rehomes_pending);
static struct kobj_attribute write_staging_drainable_rehomes_attr =
	__ATTR_RO(write_staging_drainable_rehomes);
static struct kobj_attribute write_staging_drain_pressure_attr =
	__ATTR_RO(write_staging_drain_pressure);
static struct kobj_attribute write_staging_drainable_tier_mask_attr =
	__ATTR_RO(write_staging_drainable_tier_mask);
static struct kobj_attribute oldest_staged_write_at_attr =
	__ATTR_RO(oldest_staged_write_at);
static struct kobj_attribute last_drain_at_attr =
	__ATTR_RO(last_drain_at);
static struct kobj_attribute last_drain_reason_attr =
	__ATTR_RO(last_drain_reason);
static struct kobj_attribute metadata_active_tier_mask_attr =
	__ATTR_RW(metadata_active_tier_mask);
static struct kobj_attribute write_staging_drain_active_tier_mask_attr =
	__ATTR_RW(write_staging_drain_active_tier_mask);
static struct kobj_attribute metadata_tier_skips_attr =
	__ATTR_RO(metadata_tier_skips);

static struct attribute *smoothfs_pool_attrs[] = {
	&spill_creates_total_attr.attr,
	&spill_creates_failed_all_tiers_attr.attr,
	&any_spill_since_mount_attr.attr,
	&write_staging_supported_attr.attr,
	&write_staging_enabled_attr.attr,
	&write_staging_full_pct_attr.attr,
	&staged_bytes_attr.attr,
	&staged_rehome_bytes_attr.attr,
	&range_staged_bytes_attr.attr,
	&range_staged_writes_attr.attr,
	&staged_rehomes_total_attr.attr,
	&staged_rehomes_pending_attr.attr,
	&write_staging_drainable_rehomes_attr.attr,
	&write_staging_drain_pressure_attr.attr,
	&write_staging_drainable_tier_mask_attr.attr,
	&oldest_staged_write_at_attr.attr,
	&last_drain_at_attr.attr,
	&last_drain_reason_attr.attr,
	&metadata_active_tier_mask_attr.attr,
	&write_staging_drain_active_tier_mask_attr.attr,
	&metadata_tier_skips_attr.attr,
	NULL,
};

ATTRIBUTE_GROUPS(smoothfs_pool);

static void smoothfs_sysfs_pool_release(struct kobject *kobj)
{
	kfree(to_smoothfs_sysfs_pool(kobj));
}

static const struct kobj_type smoothfs_pool_ktype = {
	.release        = smoothfs_sysfs_pool_release,
	.sysfs_ops      = &kobj_sysfs_ops,
	.default_groups = smoothfs_pool_groups,
};

int smoothfs_sysfs_pool_add(struct smoothfs_sb_info *sbi)
{
	struct smoothfs_sysfs_pool *pool;
	int err;

	if (!smoothfs_sysfs_root)
		return 0;

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return -ENOMEM;
	pool->sbi = sbi;
	err = kobject_init_and_add(&pool->kobj, &smoothfs_pool_ktype,
				   smoothfs_sysfs_root, "%pUb",
				   &sbi->pool_uuid);
	if (err) {
		kobject_put(&pool->kobj);
		return err;
	}
	sbi->sysfs_pool = pool;
	return 0;
}

void smoothfs_sysfs_pool_remove(struct smoothfs_sb_info *sbi)
{
	struct smoothfs_sysfs_pool *pool = sbi->sysfs_pool;

	sbi->sysfs_pool = NULL;
	if (pool)
		kobject_put(&pool->kobj);
}

void smoothfs_spill_note_success(struct smoothfs_sb_info *sbi,
				 struct inode *inode,
				 u8 source_tier, u8 dest_tier)
{
	atomic64_inc(&sbi->spill_creates_total);
	atomic_set(&sbi->any_spill_since_mount, 1);
	smoothfs_netlink_emit_spill(sbi, SMOOTHFS_I(inode)->oid,
				    source_tier, dest_tier,
				    i_size_read(inode));
}

void smoothfs_spill_note_failed_all_tiers(struct smoothfs_sb_info *sbi)
{
	atomic64_inc(&sbi->spill_creates_failed_all_tiers);
}

static void smoothfs_write_staging_set_reason(struct smoothfs_sb_info *sbi,
					      const char *reason)
{
	spin_lock(&sbi->write_staging_lock);
	strscpy(sbi->last_drain_reason, reason,
		sizeof(sbi->last_drain_reason));
	spin_unlock(&sbi->write_staging_lock);
}

static int smoothfs_resolve_rel_path_on_tier(struct smoothfs_sb_info *sbi,
					     u8 tier, const char *rel_path,
					     struct path *out)
{
	char *buf, *rendered, *full = NULL;
	int err;

	if (tier >= sbi->ntiers)
		return -EINVAL;

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

static int smoothfs_unlink_rel_path_on_tier(struct smoothfs_sb_info *sbi,
					    u8 tier, const char *rel_path)
{
	struct path parent_path;
	struct dentry *lower;
	struct qstr qname;
	char *work, *slash, *name, *parent_rel;
	int err;

	if (!rel_path || !*rel_path)
		return -EINVAL;

	work = kstrdup(rel_path, GFP_KERNEL);
	if (!work)
		return -ENOMEM;

	slash = strrchr(work, '/');
	if (slash) {
		*slash = '\0';
		parent_rel = work;
		name = slash + 1;
	} else {
		parent_rel = "";
		name = work;
	}
	if (!*name) {
		kfree(work);
		return -EINVAL;
	}

	err = smoothfs_resolve_rel_path_on_tier(sbi, tier, parent_rel,
						&parent_path);
	if (err) {
		kfree(work);
		return err;
	}

	qname = (struct qstr)QSTR_INIT(name, strlen(name));
	inode_lock(d_inode(parent_path.dentry));
	lower = smoothfs_compat_lookup(&nop_mnt_idmap, &qname,
				       parent_path.dentry);
	if (IS_ERR(lower)) {
		err = PTR_ERR(lower);
		goto out_unlock;
	}
	if (d_really_is_negative(lower)) {
		dput(lower);
		err = 0;
		goto out_unlock;
	}
	if (!d_is_reg(lower)) {
		dput(lower);
		err = -EISDIR;
		goto out_unlock;
	}
	err = vfs_unlink(&nop_mnt_idmap, d_inode(parent_path.dentry), lower,
			 NULL);
	dput(lower);

out_unlock:
	inode_unlock(d_inode(parent_path.dentry));
	path_put(&parent_path);
	kfree(work);
	return err;
}

static struct inode *smoothfs_write_staging_find_drainable_rehome(struct smoothfs_sb_info *sbi)
{
	struct smoothfs_inode_info *si;
	struct inode *inode = NULL;

	down_read(&sbi->inode_lock);
	list_for_each_entry(si, &sbi->inode_list, sb_link) {
		u8 tier;

		if (!READ_ONCE(si->write_staged))
			continue;
		tier = READ_ONCE(si->write_staged_drain_tier);
		if (!smoothfs_write_staging_drain_tier_active(sbi, tier))
			continue;
		inode = igrab(&si->vfs_inode);
		if (inode)
			break;
	}
	up_read(&sbi->inode_lock);
	return inode;
}

static int smoothfs_write_staging_drain_one_rehome(struct smoothfs_sb_info *sbi,
						   struct inode *inode)
{
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	char *rel_path = NULL;
	u8 tier;
	bool cleared = false;
	int err = 0;

	inode_lock(inode);
	if (!READ_ONCE(si->write_staged)) {
		inode_unlock(inode);
		return 0;
	}
	tier = READ_ONCE(si->write_staged_drain_tier);
	if (!smoothfs_write_staging_drain_tier_active(sbi, tier)) {
		inode_unlock(inode);
		return 0;
	}
	if (si->rel_path)
		rel_path = kstrdup(si->rel_path, GFP_KERNEL);
	inode_unlock(inode);
	if (!rel_path)
		return -ENOMEM;

	err = smoothfs_unlink_rel_path_on_tier(sbi, tier, rel_path);
	kfree(rel_path);
	if (err)
		return err;

	inode_lock(inode);
	if (READ_ONCE(si->write_staged) &&
	    READ_ONCE(si->write_staged_drain_tier) == tier) {
		WRITE_ONCE(si->write_staged, false);
		WRITE_ONCE(si->write_staged_drain_tier, SMOOTHFS_MAX_TIERS);
		cleared = true;
	}
	inode_unlock(inode);

	if (cleared) {
		atomic64_set(&sbi->last_drain_ns, ktime_get_real_ns());
		smoothfs_write_staging_set_reason(sbi, "truncate-rehome-drain");
		if (smoothfs_write_staging_pending_rehomes(sbi) == 0) {
			atomic64_set(&sbi->staged_bytes, 0);
			atomic64_set(&sbi->oldest_staged_write_ns, 0);
		}
	}
	return 0;
}

static int smoothfs_write_staging_drain_rehomes(struct smoothfs_sb_info *sbi)
{
	struct inode *inode;
	int err;

	while ((inode = smoothfs_write_staging_find_drainable_rehome(sbi)) != NULL) {
		err = smoothfs_write_staging_drain_one_rehome(sbi, inode);
		iput(inode);
		if (err)
			return err;
	}
	return 0;
}

void smoothfs_write_staging_note_rehome(struct smoothfs_sb_info *sbi)
{
	u64 now = ktime_get_real_ns();

	atomic64_inc(&sbi->staged_rehomes_total);
	atomic64_cmpxchg(&sbi->oldest_staged_write_ns, 0, now);
	smoothfs_write_staging_set_reason(sbi, "truncate-rehome");
}

void smoothfs_write_staging_note_write(struct smoothfs_sb_info *sbi,
				       ssize_t bytes)
{
	u64 now;

	if (bytes <= 0)
		return;
	now = ktime_get_real_ns();
	atomic64_add(bytes, &sbi->staged_bytes);
	atomic64_add(bytes, &sbi->staged_rehome_bytes);
	atomic64_cmpxchg(&sbi->oldest_staged_write_ns, 0, now);
	smoothfs_write_staging_set_reason(sbi, "staged-write");
}

void smoothfs_write_staging_note_range_write(struct smoothfs_sb_info *sbi,
					     ssize_t bytes)
{
	u64 now;

	if (bytes <= 0)
		return;
	now = ktime_get_real_ns();
	atomic64_add(bytes, &sbi->staged_bytes);
	atomic64_add(bytes, &sbi->range_staged_bytes);
	atomic64_inc(&sbi->range_staged_writes);
	atomic64_cmpxchg(&sbi->oldest_staged_write_ns, 0, now);
	smoothfs_write_staging_set_reason(sbi, "range-staged-write");
}

const struct rhashtable_params smoothfs_oid_rht_params = {
	.head_offset = offsetof(struct smoothfs_inode_info, hash_node),
	.key_offset  = offsetof(struct smoothfs_inode_info, oid),
	.key_len     = SMOOTHFS_OID_LEN,
	.automatic_shrinking = true,
};

int smoothfs_oid_map_init(struct smoothfs_sb_info *sbi)
{
	int err;

	err = rhashtable_init(&sbi->oid_map, &smoothfs_oid_rht_params);
	if (err)
		return err;
	WRITE_ONCE(sbi->oid_map_ready, true);
	return 0;
}

void smoothfs_oid_map_destroy(struct smoothfs_sb_info *sbi)
{
	if (!sbi->oid_map_ready)
		return;
	WRITE_ONCE(sbi->oid_map_ready, false);
	rhashtable_destroy(&sbi->oid_map);
}

int smoothfs_oid_map_insert(struct smoothfs_sb_info *sbi,
			    struct smoothfs_inode_info *si)
{
	static const u8 zero_oid[SMOOTHFS_OID_LEN] = { 0 };

	/* The root inode has an all-zero oid (see smoothfs_iget). Leave it
	 * out of the rhashtable — it's never looked up by oid, and keeping
	 * zero keys out avoids collisions with pre-xattr allocation gaps. */
	if (memcmp(si->oid, zero_oid, SMOOTHFS_OID_LEN) == 0)
		return 0;

	return rhashtable_insert_fast(&sbi->oid_map, &si->hash_node,
				      smoothfs_oid_rht_params);
}

void smoothfs_oid_map_remove(struct smoothfs_sb_info *sbi,
			     struct smoothfs_inode_info *si)
{
	static const u8 zero_oid[SMOOTHFS_OID_LEN] = { 0 };

	if (!READ_ONCE(sbi->oid_map_ready))
		return;
	if (memcmp(si->oid, zero_oid, SMOOTHFS_OID_LEN) == 0)
		return;
	rhashtable_remove_fast(&sbi->oid_map, &si->hash_node,
			       smoothfs_oid_rht_params);
}

/* ---- (tier_idx, lower_ino) -> smoothfs ino_no cache ---- */

const struct rhashtable_params smoothfs_lower_ino_rht_params = {
	.head_offset         = offsetof(struct smoothfs_lower_ino_entry, hnode),
	.key_offset          = offsetof(struct smoothfs_lower_ino_entry, key),
	.key_len             = sizeof(u64),
	.automatic_shrinking = true,
};

static __always_inline u64 smoothfs_lower_key(u8 tier_idx,
					      unsigned long lower_ino)
{
	return ((u64)tier_idx << 56) | ((u64)lower_ino & ((1ULL << 56) - 1));
}

int smoothfs_lower_ino_map_init(struct smoothfs_sb_info *sbi)
{
	int err = rhashtable_init(&sbi->lower_ino_map,
				  &smoothfs_lower_ino_rht_params);
	if (err)
		return err;
	WRITE_ONCE(sbi->lower_ino_map_ready, true);
	return 0;
}

static void free_lower_ino_entry(void *ptr, void *arg)
{
	kfree(ptr);
}

void smoothfs_lower_ino_map_destroy(struct smoothfs_sb_info *sbi)
{
	if (!sbi->lower_ino_map_ready)
		return;
	WRITE_ONCE(sbi->lower_ino_map_ready, false);
	rhashtable_free_and_destroy(&sbi->lower_ino_map,
				    free_lower_ino_entry, NULL);
}

u64 smoothfs_lower_ino_map_get(struct smoothfs_sb_info *sbi, u8 tier_idx,
			       unsigned long lower_ino)
{
	struct smoothfs_lower_ino_entry *e;
	u64 key = smoothfs_lower_key(tier_idx, lower_ino);

	if (!READ_ONCE(sbi->lower_ino_map_ready))
		return 0;

	e = rhashtable_lookup_fast(&sbi->lower_ino_map, &key,
				   smoothfs_lower_ino_rht_params);
	return e ? e->ino_no : 0;
}

int smoothfs_lower_ino_map_insert(struct smoothfs_sb_info *sbi, u8 tier_idx,
				  unsigned long lower_ino, u64 ino_no)
{
	struct smoothfs_lower_ino_entry *e;
	int err;

	if (!READ_ONCE(sbi->lower_ino_map_ready))
		return 0;

	e = kmalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;
	e->key    = smoothfs_lower_key(tier_idx, lower_ino);
	e->ino_no = ino_no;

	err = rhashtable_insert_fast(&sbi->lower_ino_map, &e->hnode,
				     smoothfs_lower_ino_rht_params);
	if (err) {
		kfree(e);
		/* EEXIST means another task raced us with the same lower —
		 * harmless, the other entry has the same ino_no. */
		if (err == -EEXIST)
			return 0;
	}
	return err;
}

void smoothfs_lower_ino_map_remove(struct smoothfs_sb_info *sbi, u8 tier_idx,
				   unsigned long lower_ino)
{
	struct smoothfs_lower_ino_entry *e;
	u64 key = smoothfs_lower_key(tier_idx, lower_ino);

	if (!READ_ONCE(sbi->lower_ino_map_ready))
		return;

	e = rhashtable_lookup_fast(&sbi->lower_ino_map, &key,
				   smoothfs_lower_ino_rht_params);
	if (!e)
		return;
	if (rhashtable_remove_fast(&sbi->lower_ino_map, &e->hnode,
				   smoothfs_lower_ino_rht_params) == 0)
		kfree(e);
}

/* ---- OID xattr writeback queue ---- */

#define SMOOTHFS_OID_WB_INTERVAL_MS  5000
#define SMOOTHFS_OID_WB_HIGH_WATER   65536

static void smoothfs_oid_wb_worker(struct work_struct *work)
{
	struct smoothfs_sb_info *sbi = container_of(to_delayed_work(work),
						     struct smoothfs_sb_info,
						     oid_wb_work);
	LIST_HEAD(local);
	struct smoothfs_oid_wb_entry *e, *tmp;

	spin_lock(&sbi->oid_wb_lock);
	list_splice_init(&sbi->oid_wb_pending, &local);
	sbi->oid_wb_pending_count = 0;
	spin_unlock(&sbi->oid_wb_lock);

	list_for_each_entry_safe(e, tmp, &local, link) {
		struct dentry *lower = e->lower_path.dentry;
		int err;

		/* Skip if the lower dentry went negative before we got to
		 * it — the file was unlinked after CREATE, there's nothing
		 * to persist the OID on. */
		if (d_really_is_positive(lower)) {
			err = __vfs_setxattr(&nop_mnt_idmap, lower,
					     d_inode(lower),
					     SMOOTHFS_OID_XATTR,
					     e->oid, SMOOTHFS_OID_LEN,
					     XATTR_CREATE);
			if (err && err != -EEXIST)
				pr_warn_ratelimited("smoothfs: deferred OID xattr write failed: %d\n",
						    err);
		}
		path_put(&e->lower_path);
		kfree(e);
	}
}

int smoothfs_oid_wb_init(struct smoothfs_sb_info *sbi)
{
	spin_lock_init(&sbi->oid_wb_lock);
	INIT_LIST_HEAD(&sbi->oid_wb_pending);
	sbi->oid_wb_pending_count = 0;
	INIT_DELAYED_WORK(&sbi->oid_wb_work, smoothfs_oid_wb_worker);
	sbi->oid_wb_wq = alloc_workqueue("smoothfs-oid-wb",
					 WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!sbi->oid_wb_wq)
		return -ENOMEM;
	WRITE_ONCE(sbi->oid_wb_ready, true);
	return 0;
}

void smoothfs_oid_wb_drain(struct smoothfs_sb_info *sbi)
{
	if (!READ_ONCE(sbi->oid_wb_ready))
		return;
	/* Run the worker now if not already, then wait for completion. */
	mod_delayed_work(sbi->oid_wb_wq, &sbi->oid_wb_work, 0);
	flush_delayed_work(&sbi->oid_wb_work);
}

void smoothfs_oid_wb_destroy(struct smoothfs_sb_info *sbi)
{
	if (!sbi->oid_wb_ready)
		return;
	WRITE_ONCE(sbi->oid_wb_ready, false);
	cancel_delayed_work_sync(&sbi->oid_wb_work);
	/* Drain anything that got queued between WRITE_ONCE and cancel —
	 * sync_fs couldn't reach the workqueue after we flipped the ready
	 * flag, so we run the worker callback directly one last time. */
	smoothfs_oid_wb_worker(&sbi->oid_wb_work.work);
	destroy_workqueue(sbi->oid_wb_wq);
	sbi->oid_wb_wq = NULL;
}

int smoothfs_oid_wb_queue(struct smoothfs_sb_info *sbi,
			  struct path *lower_path,
			  const u8 oid[SMOOTHFS_OID_LEN])
{
	struct smoothfs_oid_wb_entry *e;
	unsigned int count;
	bool kick_now = false;

	if (!READ_ONCE(sbi->oid_wb_ready)) {
		/* Writeback isn't available — fall back to synchronous
		 * semantics for correctness. */
		return smoothfs_write_oid_xattr(lower_path->dentry, oid);
	}

	e = kmalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;
	e->lower_path = *lower_path;
	path_get(&e->lower_path);
	memcpy(e->oid, oid, SMOOTHFS_OID_LEN);

	spin_lock(&sbi->oid_wb_lock);
	list_add_tail(&e->link, &sbi->oid_wb_pending);
	count = ++sbi->oid_wb_pending_count;
	if (count >= SMOOTHFS_OID_WB_HIGH_WATER)
		kick_now = true;
	spin_unlock(&sbi->oid_wb_lock);

	if (kick_now)
		mod_delayed_work(sbi->oid_wb_wq, &sbi->oid_wb_work, 0);
	else
		queue_delayed_work(sbi->oid_wb_wq, &sbi->oid_wb_work,
				   msecs_to_jiffies(SMOOTHFS_OID_WB_INTERVAL_MS));
	return 0;
}

/* From module.c — slab cache backing the alloc_inode hook below. */
extern struct kmem_cache *smoothfs_inode_cachep;

static struct inode *smoothfs_alloc_inode(struct super_block *sb)
{
	struct smoothfs_inode_info *si;

	si = alloc_inode_sb(sb, smoothfs_inode_cachep, GFP_KERNEL);
	if (!si)
		return NULL;

	memset(si->oid, 0, SMOOTHFS_OID_LEN);
	si->gen = 0;
	si->current_tier = 0;
	si->intended_tier = 0;
	si->movement_state = SMOOTHFS_MS_PLACED;
	si->pin_state = SMOOTHFS_PIN_NONE;
	si->cutover_gen = 0;
	si->transaction_seq = 0;
	atomic_set(&si->nlink_observed, 1);
	si->lower_path.mnt = NULL;
	si->lower_path.dentry = NULL;
	atomic_set(&si->open_count, 0);
	atomic64_set(&si->read_bytes, 0);
	atomic64_set(&si->write_bytes, 0);
	atomic_set(&si->write_reservation, 0);
	si->last_access_ns = 0;
	init_waitqueue_head(&si->cutover_wq);
	si->mappings_quiesced = false;
	si->write_staged = false;
	si->write_staged_drain_tier = SMOOTHFS_MAX_TIERS;
	si->range_staged = false;
	si->range_staged_source_tier = SMOOTHFS_MAX_TIERS;
	si->range_staged_path.mnt = NULL;
	si->range_staged_path.dentry = NULL;
	mutex_init(&si->range_staging_lock);
	INIT_LIST_HEAD(&si->range_staged_ranges);
	si->rel_path = NULL;
	atomic_set(&si->replay_pinned, 0);
	INIT_LIST_HEAD(&si->sb_link);
	return &si->vfs_inode;
}

static void smoothfs_free_inode(struct inode *inode)
{
	kmem_cache_free(smoothfs_inode_cachep, SMOOTHFS_I(inode));
}

static void smoothfs_evict_inode(struct inode *inode)
{
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(inode->i_sb);

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);

	if (sbi) {
		smoothfs_clear_write_reservation(sbi, si);
		smoothfs_oid_map_remove(sbi, si);
		if (si->lower_path.dentry && si->lower_path.mnt) {
			u8 tier_idx = smoothfs_tier_of(sbi, si->lower_path.mnt);
			if (tier_idx < SMOOTHFS_MAX_TIERS)
				smoothfs_lower_ino_map_remove(sbi, tier_idx,
					d_inode(si->lower_path.dentry)->i_ino);
		}
		down_write(&sbi->inode_lock);
		if (!list_empty(&si->sb_link))
			list_del_init(&si->sb_link);
		up_write(&sbi->inode_lock);
	}

	if (si->lower_path.dentry) {
		path_put(&si->lower_path);
		si->lower_path.dentry = NULL;
		si->lower_path.mnt = NULL;
	}
	if (si->range_staged_path.dentry) {
		path_put(&si->range_staged_path);
		si->range_staged_path.dentry = NULL;
		si->range_staged_path.mnt = NULL;
	}
	while (!list_empty(&si->range_staged_ranges)) {
		struct smoothfs_staged_range *range;

		range = list_first_entry(&si->range_staged_ranges,
					 struct smoothfs_staged_range, link);
		list_del(&range->link);
		kfree(range);
	}
	kfree(si->rel_path);
	si->rel_path = NULL;
}

enum smoothfs_param {
	Opt_pool,
	Opt_uuid,
	Opt_tiers,
};

static const struct fs_parameter_spec smoothfs_fs_parameters[] = {
	fsparam_string("pool",  Opt_pool),
	fsparam_string("uuid",  Opt_uuid),
	fsparam_string("tiers", Opt_tiers),
	{}
};

struct smoothfs_fc_ctx {
	char    *pool;
	char    *uuid_str;
	char    *tiers;     /* colon-separated lower paths */
};

static int smoothfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct smoothfs_fc_ctx *ctx = fc->fs_private;
	struct fs_parse_result result;
	int opt;

	opt = fs_parse(fc, smoothfs_fs_parameters, param, &result);
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_pool:
		kfree(ctx->pool);
		ctx->pool = kstrdup(param->string, GFP_KERNEL);
		if (!ctx->pool)
			return -ENOMEM;
		break;
	case Opt_uuid:
		kfree(ctx->uuid_str);
		ctx->uuid_str = kstrdup(param->string, GFP_KERNEL);
		if (!ctx->uuid_str)
			return -ENOMEM;
		break;
	case Opt_tiers:
		kfree(ctx->tiers);
		ctx->tiers = kstrdup(param->string, GFP_KERNEL);
		if (!ctx->tiers)
			return -ENOMEM;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int smoothfs_resolve_tiers(struct smoothfs_sb_info *sbi,
				  const char *tiers_csv)
{
	char *spec, *cursor, *p;
	u8 rank = 0;
	int err = 0;

	if (!tiers_csv || !*tiers_csv)
		return -EINVAL;

	spec = kstrdup(tiers_csv, GFP_KERNEL);
	if (!spec)
		return -ENOMEM;
	cursor = spec;

	while ((p = strsep(&cursor, ":")) != NULL) {
		struct smoothfs_tier *t;

		if (!*p)
			continue;
		if (rank >= SMOOTHFS_MAX_TIERS) {
			err = -E2BIG;
			break;
		}
		t = &sbi->tiers[rank];
		err = kern_path(p, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &t->lower_path);
		if (err) {
			pr_err("smoothfs: tier %u path %s lookup failed: %d\n",
			       rank, p, err);
			break;
		}
		t->rank = rank;
		t->lower_id = NULL;     /* set later via SMOOTHFS_CMD_REGISTER_POOL */

		err = smoothfs_probe_capabilities(t);
		if (err) {
			pr_err("smoothfs: tier %u capability probe failed: %d\n",
			       rank, err);
			path_put(&t->lower_path);
			break;
		}
		if ((t->caps & SMOOTHFS_CAPS_REQUIRED) != SMOOTHFS_CAPS_REQUIRED) {
			pr_err("smoothfs: tier %u (%s) missing required caps "
			       "(have 0x%lx need 0x%lx)\n",
			       rank, p, (unsigned long)t->caps,
			       (unsigned long)SMOOTHFS_CAPS_REQUIRED);
			path_put(&t->lower_path);
			err = -EOPNOTSUPP;
			break;
		}
		if (smoothfs_lower_has_revalidate(t))
			sbi->any_lower_revalidates = true;
		rank++;
	}
	kfree(spec);

	if (err) {
		while (rank > 0) {
			--rank;
			path_put(&sbi->tiers[rank].lower_path);
		}
		return err;
	}
	if (rank == 0)
		return -EINVAL;

	sbi->ntiers = rank;
	sbi->fastest_tier = 0;
	return 0;
}

static int smoothfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct smoothfs_fc_ctx *ctx = fc->fs_private;
	struct smoothfs_sb_info *sbi;
	struct path root_lower;
	struct inode *root_inode;
	int err;

	if (!ctx->pool || !ctx->tiers) {
		pr_err("smoothfs: pool= and tiers= are required\n");
		return -EINVAL;
	}

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	mutex_init(&sbi->placement_lock);
	init_rwsem(&sbi->inode_lock);
	INIT_LIST_HEAD(&sbi->inode_list);
	atomic64_set(&sbi->oid_monotonic, 0);

	err = smoothfs_oid_map_init(sbi);
	if (err)
		goto out_sbi;
	err = smoothfs_lower_ino_map_init(sbi);
	if (err)
		goto out_sbi;
	err = init_srcu_struct(&sbi->cutover_srcu);
	if (err)
		goto out_sbi;
	sbi->cutover_srcu_ready = true;
	err = smoothfs_oid_wb_init(sbi);
	if (err)
		goto out_sbi;
	err = smoothfs_placement_wb_init(sbi);
	if (err)
		goto out_sbi;

	strscpy(sbi->pool_name, ctx->pool, sizeof(sbi->pool_name));
	if (ctx->uuid_str) {
		err = uuid_parse(ctx->uuid_str, &sbi->pool_uuid);
		if (err) {
			pr_err("smoothfs: pool uuid parse failed: %d\n", err);
			goto out_sbi;
		}
	} else {
		uuid_gen(&sbi->pool_uuid);
	}
	sbi->fsid = xxh32(sbi->pool_uuid.b, sizeof(sbi->pool_uuid.b), 0);
	atomic64_set(&sbi->spill_creates_total, 0);
	atomic64_set(&sbi->spill_creates_failed_all_tiers, 0);
	atomic_set(&sbi->any_spill_since_mount, 0);
	WRITE_ONCE(sbi->write_staging_enabled, false);
	WRITE_ONCE(sbi->write_staging_full_pct, 98);
	atomic64_set(&sbi->staged_bytes, 0);
	atomic64_set(&sbi->staged_rehome_bytes, 0);
	atomic64_set(&sbi->staged_rehomes_total, 0);
	atomic64_set(&sbi->range_staged_bytes, 0);
	atomic64_set(&sbi->range_staged_writes, 0);
	atomic64_set(&sbi->oldest_staged_write_ns, 0);
	atomic64_set(&sbi->last_drain_ns, 0);
	atomic64_set(&sbi->metadata_tier_skips, 0);
	spin_lock_init(&sbi->write_staging_lock);
	WRITE_ONCE(sbi->metadata_active_tier_mask, BIT(sbi->fastest_tier));
	WRITE_ONCE(sbi->write_staging_drain_active_tier_mask,
		   BIT(sbi->fastest_tier));
	sbi->last_drain_reason[0] = '\0';

	err = smoothfs_resolve_tiers(sbi, ctx->tiers);
	if (err)
		goto out_sbi;
	WRITE_ONCE(sbi->metadata_active_tier_mask, BIT(sbi->ntiers) - 1);
	WRITE_ONCE(sbi->write_staging_drain_active_tier_mask,
		   BIT(sbi->fastest_tier));

	sb->s_magic     = SMOOTHFS_MAGIC;
	sb->s_op        = &smoothfs_super_ops;
	sb->s_export_op = &smoothfs_export_ops;
	smoothfs_compat_set_dentry_ops(sb, &smoothfs_dentry_ops);
	sb->s_xattr     = smoothfs_xattr_handlers;
	sb->s_maxbytes  = MAX_LFS_FILESIZE;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_time_gran = 1;
	sb->s_flags     |= SB_POSIXACL | SB_NOSEC;
	sb->s_fs_info   = sbi;

	root_lower = sbi->tiers[sbi->fastest_tier].lower_path;
	/* iget consumes one path ref; mirror that to the root dentry's
	 * d_fsdata via dget(). */
	path_get(&root_lower);

	root_inode = smoothfs_iget(sb, &root_lower, true, false);
	if (IS_ERR(root_inode)) {
		err = PTR_ERR(root_inode);
		path_put(&root_lower);
		goto out_tiers;
	}
	/* iget took its own ref via path_get; release ours. */
	path_put(&root_lower);

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		err = -ENOMEM;
		goto out_tiers;
	}
	smoothfs_set_lower_dentry(sb->s_root, root_lower.dentry);

	err = smoothfs_placement_open(sbi);
	if (err) {
		pr_err("smoothfs: placement log open failed: %d\n", err);
		goto out_tiers;
	}
	err = smoothfs_placement_replay(sb, sbi);
	if (err) {
		pr_err("smoothfs: placement log replay failed: %d\n", err);
		smoothfs_placement_close(sbi);
		goto out_tiers;
	}

	atomic64_set(&sbi->transaction_seq, 1);
	sbi->quiesced = false;

	err = smoothfs_sb_register(sbi);
	if (err) {
		pr_err("smoothfs: sb register failed: %d\n", err);
		smoothfs_placement_close(sbi);
		goto out_tiers;
	}
	err = smoothfs_sysfs_pool_add(sbi);
	if (err) {
		pr_err("smoothfs: sysfs pool add failed: %d\n", err);
		smoothfs_sb_unregister(sbi);
		smoothfs_placement_close(sbi);
		goto out_tiers;
	}
	smoothfs_heat_init(sbi);

	smoothfs_netlink_emit_mount_ready(sbi);
	pr_info("smoothfs: mounted pool '%s' with %u tier(s)\n",
		sbi->pool_name, sbi->ntiers);
	return 0;

out_tiers:
	{
		u8 i;
		for (i = 0; i < sbi->ntiers; i++)
			path_put(&sbi->tiers[i].lower_path);
	}
out_sbi:
	smoothfs_placement_wb_destroy(sbi);
	smoothfs_oid_wb_destroy(sbi);
	if (sbi->cutover_srcu_ready)
		cleanup_srcu_struct(&sbi->cutover_srcu);
	smoothfs_lower_ino_map_destroy(sbi);
	smoothfs_oid_map_destroy(sbi);
	kfree(sbi);
	sb->s_fs_info = NULL;
	return err;
}

static int smoothfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, smoothfs_fill_super);
}

static void smoothfs_free_fc(struct fs_context *fc)
{
	struct smoothfs_fc_ctx *ctx = fc->fs_private;

	if (!ctx)
		return;
	kfree(ctx->pool);
	kfree(ctx->uuid_str);
	kfree(ctx->tiers);
	kfree(ctx);
	fc->fs_private = NULL;
}

static const struct fs_context_operations smoothfs_context_ops = {
	.parse_param = smoothfs_parse_param,
	.get_tree    = smoothfs_get_tree,
	.free        = smoothfs_free_fc,
};

int smoothfs_init_fs_context(struct fs_context *fc)
{
	struct smoothfs_fc_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	fc->fs_private = ctx;
	fc->ops = &smoothfs_context_ops;
	return 0;
}

/* ----- super_operations beyond alloc/free (those live in module.c) ----- */

static void smoothfs_put_super(struct super_block *sb)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(sb);
	u8 i;

	if (!sbi)
		return;
	smoothfs_heat_destroy(sbi);
	smoothfs_sysfs_pool_remove(sbi);
	smoothfs_sb_unregister(sbi);
	/* Drain deferred metadata before we release sb state; otherwise
	 * queued entries would reference dentries/files whose sb is torn down. */
	smoothfs_oid_wb_destroy(sbi);
	smoothfs_placement_close(sbi);
	smoothfs_placement_wb_destroy(sbi);
	if (sbi->cutover_srcu_ready)
		cleanup_srcu_struct(&sbi->cutover_srcu);
	smoothfs_lower_ino_map_destroy(sbi);
	smoothfs_oid_map_destroy(sbi);
	for (i = 0; i < sbi->ntiers; i++)
		path_put(&sbi->tiers[i].lower_path);
	kfree(sbi);
	sb->s_fs_info = NULL;
}

/* §POSIX statfs: aggregate across all tier targets. */
static int smoothfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(dentry->d_sb);
	struct kstatfs tmp;
	u8 i;
	int err;

	memset(buf, 0, sizeof(*buf));
	buf->f_type    = SMOOTHFS_MAGIC;
	buf->f_bsize   = PAGE_SIZE;
	buf->f_namelen = NAME_MAX;
	buf->f_frsize  = PAGE_SIZE;

	for (i = 0; i < sbi->ntiers; i++) {
		err = vfs_statfs(&sbi->tiers[i].lower_path, &tmp);
		if (err) {
			smoothfs_netlink_emit_tier_fault(sbi, i);
			continue;
		}
		buf->f_blocks += (tmp.f_blocks * tmp.f_bsize) / PAGE_SIZE;
		buf->f_bfree  += (tmp.f_bfree  * tmp.f_bsize) / PAGE_SIZE;
		buf->f_bavail += (tmp.f_bavail * tmp.f_bsize) / PAGE_SIZE;
		buf->f_files  += tmp.f_files;
		buf->f_ffree  += tmp.f_ffree;
	}
	return 0;
}

static int smoothfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(root->d_sb);
	u8 i;

	seq_printf(m, ",pool=%s", sbi->pool_name);
	seq_printf(m, ",fsid=0x%08x", sbi->fsid);
	for (i = 0; i < sbi->ntiers; i++)
		seq_printf(m, ",tier%u=present", i);
	return 0;
}

/*
 * sync_fs: drain deferred SmoothFS metadata so `sync` / `syncfs` /
 * the periodic writeback pass all see up-to-date on-disk state.
 * Called with wait=1 for "make it durable", wait=0 for "kick it".
 * We flush synchronously when asked to wait and just nudge workers
 * when we aren't.
 */
static int smoothfs_sync_fs(struct super_block *sb, int wait)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(sb);

	if (!sbi)
		return 0;
	if (wait) {
		smoothfs_oid_wb_drain(sbi);
		smoothfs_placement_wb_drain(sbi);
		if (sbi->placement_log)
			vfs_fsync(sbi->placement_log, 0);
	} else {
		smoothfs_placement_wb_kick(sbi);
		if (sbi->oid_wb_ready)
			mod_delayed_work(sbi->oid_wb_wq, &sbi->oid_wb_work, 0);
	}
	return 0;
}

const struct super_operations smoothfs_super_ops = {
	.alloc_inode   = smoothfs_alloc_inode,
	.free_inode    = smoothfs_free_inode,
	.evict_inode   = smoothfs_evict_inode,
	.put_super     = smoothfs_put_super,
	.sync_fs       = smoothfs_sync_fs,
	.statfs        = smoothfs_statfs,
	.show_options  = smoothfs_show_options,
};

/*
 * smoothfs_iget — create a smoothfs inode that wraps a lower path.
 *
 * For the root inode (root=true) we use iget_locked with inode #1 so the
 * VFS root has a stable identity. For non-root inodes we synthesise the
 * inode number from the object_id per Phase 0 §0.1; if the lower has no
 * object_id xattr yet (a freshly-discovered file), one is allocated and
 * persisted before iget completes.
 */
struct inode *smoothfs_iget(struct super_block *sb, struct path *lower,
			    bool root, bool fresh)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(sb);
	struct inode *lower_inode = d_inode(lower->dentry);
	struct smoothfs_inode_info *si;
	struct inode *inode;
	u8 oid[SMOOTHFS_OID_LEN];
	u32 gen = 0;
	u64 ino_no;
	u8 tier_idx = SMOOTHFS_MAX_TIERS;
	bool need_cache_insert = false;
	int err;

	if (root) {
		ino_no = 1;
		memset(oid, 0, SMOOTHFS_OID_LEN);
	} else {
		/* Fast path — if we've seen this (tier, lower_ino) before, we
		 * already know the smoothfs ino_no and can skip both xattr
		 * reads. iget_locked's hash check will match an in-core
		 * smoothfs inode if one exists; otherwise we hit the slow
		 * path below. */
		tier_idx = smoothfs_tier_of(sbi, lower->mnt);
		ino_no = smoothfs_lower_ino_map_get(sbi, tier_idx,
						    lower_inode->i_ino);
		if (ino_no) {
			inode = ilookup(sb, ino_no);
			if (inode) {
				path_get(lower);
				return inode;
			}
			/* Cache hit but inode was evicted — treat as miss
			 * and rebuild state from xattrs. */
		}

		if (fresh) {
			err = -ENODATA;
		} else {
			err = smoothfs_read_oid_xattr(lower->dentry, oid);
		}
		if (err == -ENODATA) {
			/* Fresh file. Mint an OID in memory and queue the
			 * xattr write to the per-sb writeback worker — we
			 * don't block CREATE on the synchronous setxattr.
			 * A crash between CREATE and the next flush loses
			 * the OID (the file itself survives on the lower);
			 * next mount re-mints one. See the §0.1 addendum
			 * in the smoothfs proposal.
			 *
			 * We also know gen == 0 for a freshly-minted OID,
			 * so skip the second vfs_getxattr call that would
			 * otherwise return -ENODATA. */
			err = smoothfs_alloc_oid(sbi, oid);
			if (err)
				return ERR_PTR(err);
			err = smoothfs_oid_wb_queue(sbi, lower, oid);
			if (err)
				return ERR_PTR(err);
			gen = 0;
		} else if (err) {
			return ERR_PTR(err);
		} else {
			(void)smoothfs_read_gen_xattr(lower->dentry, &gen);
		}
		ino_no = smoothfs_inode_no_from_oid(oid);
		need_cache_insert = (tier_idx < SMOOTHFS_MAX_TIERS);
	}

	inode = iget_locked(sb, ino_no);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!smoothfs_compat_inode_is_new(inode)) {
		/*
		 * The existing inode already has its own si->lower_path with
		 * its own reference. Take a matching ref on the caller's
		 * local lower_path so every caller can path_put() on return
		 * without caring whether the inode was fresh or cached.
		 */
		path_get(lower);
		return inode;
	}

	si = SMOOTHFS_I(inode);
	memcpy(si->oid, oid, SMOOTHFS_OID_LEN);
	si->gen = gen;
	si->lower_path = *lower;
	path_get(&si->lower_path);

	smoothfs_copy_attrs(inode, lower_inode);
	inode->i_mapping->a_ops = &smoothfs_aops;

	if (S_ISDIR(inode->i_mode)) {
		inode->i_op  = &smoothfs_dir_inode_ops;
		inode->i_fop = &smoothfs_dir_ops;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op  = &smoothfs_symlink_inode_ops;
	} else if (S_ISREG(inode->i_mode)) {
		inode->i_op  = &smoothfs_file_inode_ops;
		inode->i_fop = &smoothfs_file_ops;
	} else {
		inode->i_op  = &smoothfs_special_inode_ops;
		init_special_inode(inode, inode->i_mode, lower_inode->i_rdev);
	}

	down_write(&sbi->inode_lock);
	list_add(&si->sb_link, &sbi->inode_list);
	up_write(&sbi->inode_lock);

	err = smoothfs_oid_map_insert(sbi, si);
	if (err && err != -EEXIST) {
		/* EEXIST only happens if a concurrent iget races to insert
		 * the same oid; iget_locked's singleton rule prevents that
		 * for real oids. Any other error is fatal. evict_inode will
		 * run the list cleanup (and the oid_map_remove no-ops). */
		pr_err("smoothfs: oid map insert failed: %d\n", err);
		iget_failed(inode);
		return ERR_PTR(err);
	}

	if (need_cache_insert) {
		/* Best-effort cache seed; a failure here is not fatal, just
		 * means the next iget for this lower will read xattrs. */
		(void)smoothfs_lower_ino_map_insert(sbi, tier_idx,
						    lower_inode->i_ino, ino_no);
	}

	unlock_new_inode(inode);
	return inode;
}

void smoothfs_copy_attrs(struct inode *dst, struct inode *src)
{
	dst->i_mode  = src->i_mode;
	dst->i_uid   = src->i_uid;
	dst->i_gid   = src->i_gid;
	dst->i_rdev  = src->i_rdev;
	inode_set_atime_to_ts(dst, inode_get_atime(src));
	inode_set_mtime_to_ts(dst, inode_get_mtime(src));
	inode_set_ctime_to_ts(dst, inode_get_ctime(src));
	i_size_write(dst, i_size_read(src));
	set_nlink(dst, src->i_nlink);
}

struct smoothfs_inode_info *smoothfs_lookup_rel_path(struct smoothfs_sb_info *sbi,
						     const char *rel_path)
{
	struct smoothfs_inode_info *si, *found = NULL;

	if (!rel_path)
		return NULL;

	down_read(&sbi->inode_lock);
	list_for_each_entry(si, &sbi->inode_list, sb_link) {
		if (si->rel_path && strcmp(si->rel_path, rel_path) == 0) {
			found = si;
			break;
		}
	}
	up_read(&sbi->inode_lock);
	return found;
}

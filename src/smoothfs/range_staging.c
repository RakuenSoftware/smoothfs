// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - per-inode range-staging recovery (Phase 6O).
 *
 * Persists per-inode range-staging metadata to a sidecar file alongside
 * the .stage byte file, so that buffered range-staged writes survive a
 * crash or unclean unmount. Replay runs on mount after the placement
 * log replay (so the OID -> inode hash is populated) and restores the
 * in-memory range-staging state without opening the source tier — the
 * existing drain path will flush the recovered bytes once SmoothNAS
 * marks the source tier drain-active.
 *
 * Sidecar layout (one file per inode under
 * <fastest_tier>/.smoothfs/range-<oid_hex>.meta):
 *
 *  offset  size  field
 *  0       8     magic       SMOOTHFS_RANGE_REC_MAGIC
 *  8       4     version     SMOOTHFS_RANGE_REC_VERSION
 *  12      1     source_tier
 *  13      3     reserved (0)
 *  16      8     oldest_write_ns
 *  24      4     range_count
 *  28      4     reserved (0)
 *  32     ...    range_count * (start: __le64, end: __le64)
 *
 * Atomic update: write to .meta.tmp, fsync, rename. The rename is
 * atomic on the lower fs (XFS/ext4 in the supported matrix).
 */

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/hex.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/dirent.h>

#include "smoothfs.h"

#define SMOOTHFS_RANGE_REC_MAGIC    0x534D46525352454CULL  /* "SMFRSREL" */
#define SMOOTHFS_RANGE_REC_VERSION  1u
#define SMOOTHFS_RANGE_REC_HEADER   32
#define SMOOTHFS_RANGE_REC_RANGE    16
#define SMOOTHFS_RANGE_REC_DIR      ".smoothfs"
#define SMOOTHFS_RANGE_REC_PREFIX   "range-"
#define SMOOTHFS_RANGE_REC_SUFFIX   ".meta"
#define SMOOTHFS_RANGE_REC_TMP_SUFFIX ".meta.tmp"
#define SMOOTHFS_RANGE_REC_STAGE_SUFFIX ".stage"
/* Hard cap on per-inode ranges to bound replay memory. Real workloads
 * coalesce — typical files have <100 active ranges. */
#define SMOOTHFS_RANGE_REC_MAX_RANGES 65536

struct smoothfs_range_replay_ctx {
	struct dir_context ctx;
	struct list_head names;   /* of struct smoothfs_range_replay_name */
	bool oom;
};

struct smoothfs_range_replay_name {
	struct list_head link;
	char name[NAME_MAX + 1];
};

static char *smoothfs_range_meta_name(const u8 oid[SMOOTHFS_OID_LEN],
				      const char *suffix)
{
	return kasprintf(GFP_KERNEL, ".smoothfs/" SMOOTHFS_RANGE_REC_PREFIX
				    "%*phN%s",
			 SMOOTHFS_OID_LEN, oid, suffix);
}

static int smoothfs_range_count_ranges(const struct list_head *ranges,
				       u32 *out)
{
	const struct smoothfs_staged_range *range;
	u32 count = 0;

	list_for_each_entry(range, ranges, link) {
		if (count >= SMOOTHFS_RANGE_REC_MAX_RANGES)
			return -E2BIG;
		count++;
	}
	*out = count;
	return 0;
}

static int smoothfs_range_write_full(struct file *f, const void *data,
				     size_t len, loff_t *pos)
{
	const u8 *p = data;
	size_t done = 0;

	while (done < len) {
		ssize_t n = kernel_write(f, p + done, len - done, pos);

		if (n <= 0)
			return n ? (int)n : -EIO;
		done += n;
	}
	return 0;
}

static int smoothfs_range_unlink_path(struct path *parent_path,
				      const char *name)
{
	struct dentry *parent = parent_path->dentry;
	struct dentry *child;
	struct qstr qname = QSTR_INIT(name, strlen(name));
	int err;

	inode_lock(d_inode(parent));
	child = smoothfs_compat_lookup(&nop_mnt_idmap, &qname, parent);
	if (IS_ERR(child)) {
		inode_unlock(d_inode(parent));
		err = PTR_ERR(child);
		return err == -ENOENT ? 0 : err;
	}
	if (d_really_is_negative(child)) {
		err = 0;
		goto out;
	}
	err = vfs_unlink(&nop_mnt_idmap, d_inode(parent), child, NULL);
	if (err == -ENOENT)
		err = 0;
out:
	dput(child);
	inode_unlock(d_inode(parent));
	return err;
}

static int smoothfs_range_rename_meta(struct smoothfs_sb_info *sbi,
				      const char *tmp_rel,
				      const char *final_rel)
{
	struct path dir_path = {};
	struct dentry *parent;
	struct dentry *old_dentry = NULL;
	struct dentry *new_dentry = NULL;
	struct qstr old_qname, new_qname;
	const char *tmp_base = strrchr(tmp_rel, '/');
	const char *final_base = strrchr(final_rel, '/');
	int err;

	if (!tmp_base || !final_base)
		return -EINVAL;
	tmp_base++;
	final_base++;

	err = vfs_path_lookup(sbi->tiers[sbi->fastest_tier].lower_path.dentry,
			      sbi->tiers[sbi->fastest_tier].lower_path.mnt,
			      SMOOTHFS_RANGE_REC_DIR, LOOKUP_FOLLOW,
			      &dir_path);
	if (err)
		return err;
	parent = dir_path.dentry;

	old_qname = (struct qstr)QSTR_INIT(tmp_base, strlen(tmp_base));
	new_qname = (struct qstr)QSTR_INIT(final_base, strlen(final_base));

	inode_lock(d_inode(parent));
	old_dentry = smoothfs_compat_lookup(&nop_mnt_idmap, &old_qname, parent);
	if (IS_ERR(old_dentry)) {
		err = PTR_ERR(old_dentry);
		old_dentry = NULL;
		goto out_unlock;
	}
	if (d_really_is_negative(old_dentry)) {
		err = -ENOENT;
		goto out_unlock;
	}
	new_dentry = smoothfs_compat_lookup(&nop_mnt_idmap, &new_qname, parent);
	if (IS_ERR(new_dentry)) {
		err = PTR_ERR(new_dentry);
		new_dentry = NULL;
		goto out_unlock;
	}

	{
		struct renamedata rd;

		smoothfs_compat_init_renamedata(&rd, &nop_mnt_idmap,
						parent, old_dentry,
						parent, new_dentry, 0);
		err = vfs_rename(&rd);
	}

out_unlock:
	if (new_dentry)
		dput(new_dentry);
	if (old_dentry)
		dput(old_dentry);
	inode_unlock(d_inode(parent));
	path_put(&dir_path);
	return err;
}

int smoothfs_range_staging_persist(struct smoothfs_sb_info *sbi,
				   const u8 oid[SMOOTHFS_OID_LEN],
				   u8 source_tier,
				   const struct list_head *ranges,
				   u64 oldest_write_ns)
{
	struct path fast_root;
	struct file *f = NULL;
	const struct smoothfs_staged_range *range;
	u8 header[SMOOTHFS_RANGE_REC_HEADER];
	__le64 magic = cpu_to_le64(SMOOTHFS_RANGE_REC_MAGIC);
	__le32 version = cpu_to_le32(SMOOTHFS_RANGE_REC_VERSION);
	__le64 ns = cpu_to_le64(oldest_write_ns);
	__le32 count_le;
	u32 count;
	loff_t pos = 0;
	char *tmp_rel = NULL;
	char *final_rel = NULL;
	int err;

	err = smoothfs_range_count_ranges(ranges, &count);
	if (err)
		return err;

	tmp_rel = smoothfs_range_meta_name(oid, SMOOTHFS_RANGE_REC_TMP_SUFFIX);
	if (!tmp_rel)
		return -ENOMEM;
	final_rel = smoothfs_range_meta_name(oid, SMOOTHFS_RANGE_REC_SUFFIX);
	if (!final_rel) {
		err = -ENOMEM;
		goto out;
	}

	fast_root = sbi->tiers[sbi->fastest_tier].lower_path;
	f = file_open_root(&fast_root, tmp_rel,
			   O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (IS_ERR(f)) {
		err = PTR_ERR(f);
		f = NULL;
		goto out;
	}

	memset(header, 0, sizeof(header));
	memcpy(&header[0], &magic, sizeof(magic));
	memcpy(&header[8], &version, sizeof(version));
	header[12] = source_tier;
	memcpy(&header[16], &ns, sizeof(ns));
	count_le = cpu_to_le32(count);
	memcpy(&header[24], &count_le, sizeof(count_le));

	err = smoothfs_range_write_full(f, header, sizeof(header), &pos);
	if (err)
		goto out_close;

	list_for_each_entry(range, ranges, link) {
		__le64 entry[2];

		entry[0] = cpu_to_le64((u64)range->start);
		entry[1] = cpu_to_le64((u64)range->end);
		err = smoothfs_range_write_full(f, entry, sizeof(entry), &pos);
		if (err)
			goto out_close;
	}

	err = vfs_fsync(f, 0);
	if (err)
		goto out_close;

	fput(f);
	f = NULL;

	err = smoothfs_range_rename_meta(sbi, tmp_rel, final_rel);
	if (err)
		goto out;

out_close:
	if (f)
		fput(f);
out:
	if (err && tmp_rel) {
		struct path dir;
		const char *base = strrchr(tmp_rel, '/');

		if (base &&
		    !vfs_path_lookup(sbi->tiers[sbi->fastest_tier].lower_path.dentry,
				     sbi->tiers[sbi->fastest_tier].lower_path.mnt,
				     SMOOTHFS_RANGE_REC_DIR, LOOKUP_FOLLOW,
				     &dir)) {
			(void)smoothfs_range_unlink_path(&dir, base + 1);
			path_put(&dir);
		}
	}
	kfree(final_rel);
	kfree(tmp_rel);
	return err;
}

void smoothfs_range_staging_clear(struct smoothfs_sb_info *sbi,
				  const u8 oid[SMOOTHFS_OID_LEN])
{
	struct path dir;
	char *base;
	int err;

	err = vfs_path_lookup(sbi->tiers[sbi->fastest_tier].lower_path.dentry,
			      sbi->tiers[sbi->fastest_tier].lower_path.mnt,
			      SMOOTHFS_RANGE_REC_DIR, LOOKUP_FOLLOW, &dir);
	if (err)
		return;

	base = kasprintf(GFP_KERNEL, SMOOTHFS_RANGE_REC_PREFIX "%*phN%s",
			 SMOOTHFS_OID_LEN, oid, SMOOTHFS_RANGE_REC_SUFFIX);
	if (base) {
		(void)smoothfs_range_unlink_path(&dir, base);
		kfree(base);
	}

	base = kasprintf(GFP_KERNEL, SMOOTHFS_RANGE_REC_PREFIX "%*phN%s",
			 SMOOTHFS_OID_LEN, oid,
			 SMOOTHFS_RANGE_REC_TMP_SUFFIX);
	if (base) {
		(void)smoothfs_range_unlink_path(&dir, base);
		kfree(base);
	}

	path_put(&dir);
}

static bool smoothfs_range_actor(struct dir_context *ctx, const char *name,
				 int len, loff_t off, u64 ino,
				 unsigned int type)
{
	struct smoothfs_range_replay_ctx *rc =
		container_of(ctx, struct smoothfs_range_replay_ctx, ctx);
	struct smoothfs_range_replay_name *entry;
	const size_t prefix_len = sizeof(SMOOTHFS_RANGE_REC_PREFIX) - 1;
	const size_t suffix_len = sizeof(SMOOTHFS_RANGE_REC_SUFFIX) - 1;

	(void)off;
	(void)ino;
	(void)type;

	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.'))
		return true;
	if (len > NAME_MAX)
		return true;
	if (len < (int)(prefix_len + suffix_len))
		return true;
	if (memcmp(name, SMOOTHFS_RANGE_REC_PREFIX, prefix_len) != 0)
		return true;
	if (memcmp(name + len - suffix_len, SMOOTHFS_RANGE_REC_SUFFIX,
		   suffix_len) != 0)
		return true;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		rc->oom = true;
		return false;
	}
	memcpy(entry->name, name, len);
	entry->name[len] = '\0';
	list_add_tail(&entry->link, &rc->names);
	return true;
}

static int smoothfs_range_parse_oid(const char *name,
				    u8 oid[SMOOTHFS_OID_LEN])
{
	const size_t prefix_len = sizeof(SMOOTHFS_RANGE_REC_PREFIX) - 1;
	const size_t suffix_len = sizeof(SMOOTHFS_RANGE_REC_SUFFIX) - 1;
	size_t len = strlen(name);
	const char *hex;
	size_t i;

	if (len != prefix_len + (SMOOTHFS_OID_LEN * 2) + suffix_len)
		return -EINVAL;
	hex = name + prefix_len;
	for (i = 0; i < SMOOTHFS_OID_LEN; i++) {
		int hi = hex_to_bin(hex[i * 2]);
		int lo = hex_to_bin(hex[i * 2 + 1]);

		if (hi < 0 || lo < 0)
			return -EINVAL;
		oid[i] = (hi << 4) | lo;
	}
	return 0;
}

static int smoothfs_range_read_full(struct file *f, void *buf, size_t len,
				    loff_t *pos)
{
	u8 *p = buf;
	size_t done = 0;

	while (done < len) {
		ssize_t n = kernel_read(f, p + done, len - done, pos);

		if (n == 0)
			return -EIO;
		if (n < 0)
			return n;
		done += n;
	}
	return 0;
}

static int smoothfs_range_open_stage(struct smoothfs_sb_info *sbi,
				     const u8 oid[SMOOTHFS_OID_LEN],
				     struct path *out_path)
{
	struct path fast_root = sbi->tiers[sbi->fastest_tier].lower_path;
	struct file *stage;
	char *rel;
	int err = 0;

	rel = kasprintf(GFP_KERNEL, ".smoothfs/" SMOOTHFS_RANGE_REC_PREFIX
				    "%*phN%s",
			SMOOTHFS_OID_LEN, oid,
			SMOOTHFS_RANGE_REC_STAGE_SUFFIX);
	if (!rel)
		return -ENOMEM;

	stage = file_open_root(&fast_root, rel, O_RDWR, 0);
	kfree(rel);
	if (IS_ERR(stage))
		return PTR_ERR(stage);

	*out_path = stage->f_path;
	path_get(out_path);
	fput(stage);
	return err;
}

static int smoothfs_range_replay_one(struct super_block *sb,
				     struct smoothfs_sb_info *sbi,
				     struct file *f, const char *name,
				     u64 *recovered_bytes_out,
				     u64 *oldest_ns_out,
				     u32 *tier_mask_out)
{
	u8 header[SMOOTHFS_RANGE_REC_HEADER];
	struct smoothfs_inode_info *si;
	struct path stage_path = {};
	loff_t pos = 0;
	u64 magic;
	u32 version, count, i;
	u8 oid[SMOOTHFS_OID_LEN];
	u8 source_tier;
	u64 oldest_ns;
	u64 recovered = 0;
	int err;

	err = smoothfs_range_parse_oid(name, oid);
	if (err)
		return err;

	err = smoothfs_range_read_full(f, header, sizeof(header), &pos);
	if (err)
		return err;

	magic = le64_to_cpup((__le64 *)&header[0]);
	if (magic != SMOOTHFS_RANGE_REC_MAGIC)
		return -EINVAL;
	version = le32_to_cpup((__le32 *)&header[8]);
	if (version != SMOOTHFS_RANGE_REC_VERSION)
		return -EINVAL;
	source_tier = header[12];
	oldest_ns = le64_to_cpup((__le64 *)&header[16]);
	count = le32_to_cpup((__le32 *)&header[24]);
	if (count > SMOOTHFS_RANGE_REC_MAX_RANGES)
		return -EINVAL;
	if (source_tier >= sbi->ntiers)
		return -EINVAL;

	si = smoothfs_lookup_oid(sbi, oid);
	if (!si) {
		/* No corresponding inode in the OID map. The placement log
		 * may not have caught up before the crash. Leave the .meta
		 * + .stage on disk so an operator can investigate; don't
		 * delete recoverable data. */
		pr_warn_ratelimited("smoothfs: range-staging meta %s has no matching inode; skipping\n",
				    name);
		return 0;
	}

	err = smoothfs_range_open_stage(sbi, oid, &stage_path);
	if (err) {
		pr_warn_ratelimited("smoothfs: range-staging stage open for %s failed: %d\n",
				    name, err);
		return 0;  /* skip, do not abort the whole replay */
	}

	mutex_lock(&si->range_staging_lock);
	if (si->range_staged_path.dentry)
		path_put(&si->range_staged_path);
	si->range_staged_path = stage_path;
	/* Don't path_put: we transferred the ref into si. */

	while (!list_empty(&si->range_staged_ranges)) {
		struct smoothfs_staged_range *range;

		range = list_first_entry(&si->range_staged_ranges,
					 struct smoothfs_staged_range, link);
		list_del(&range->link);
		kfree(range);
	}
	for (i = 0; i < count; i++) {
		__le64 entry[2];
		struct smoothfs_staged_range *range;
		s64 start, end;

		err = smoothfs_range_read_full(f, entry, sizeof(entry), &pos);
		if (err)
			goto out_unlock;
		start = (s64)le64_to_cpu(entry[0]);
		end = (s64)le64_to_cpu(entry[1]);
		if (end <= start) {
			err = -EINVAL;
			goto out_unlock;
		}
		range = kmalloc(sizeof(*range), GFP_KERNEL);
		if (!range) {
			err = -ENOMEM;
			goto out_unlock;
		}
		range->start = start;
		range->end = end;
		list_add_tail(&range->link, &si->range_staged_ranges);
		recovered += (u64)(end - start);
	}

	WRITE_ONCE(si->range_staged, true);
	WRITE_ONCE(si->range_staged_recovered, true);
	WRITE_ONCE(si->range_staged_source_tier, source_tier);

out_unlock:
	if (err) {
		while (!list_empty(&si->range_staged_ranges)) {
			struct smoothfs_staged_range *range;

			range = list_first_entry(&si->range_staged_ranges,
						 struct smoothfs_staged_range,
						 link);
			list_del(&range->link);
			kfree(range);
		}
		path_put(&si->range_staged_path);
		si->range_staged_path.dentry = NULL;
		si->range_staged_path.mnt = NULL;
	}
	mutex_unlock(&si->range_staging_lock);
	if (err)
		return 0;  /* skip this entry */

	*recovered_bytes_out += recovered;
	if (*oldest_ns_out == 0 ||
	    (oldest_ns != 0 && oldest_ns < *oldest_ns_out))
		*oldest_ns_out = oldest_ns;
	*tier_mask_out |= BIT(source_tier);
	return 0;
}

int smoothfs_range_staging_replay(struct super_block *sb,
				  struct smoothfs_sb_info *sbi)
{
	struct path dir = {};
	struct file *dirf = NULL;
	struct smoothfs_range_replay_ctx ctx = {
		.ctx.actor = smoothfs_range_actor,
	};
	struct smoothfs_range_replay_name *entry, *tmp;
	u64 recovered_bytes = 0;
	u64 oldest_ns = 0;
	u32 tier_mask = 0;
	u32 recovered_files = 0;
	int err;

	INIT_LIST_HEAD(&ctx.names);

	err = vfs_path_lookup(sbi->tiers[sbi->fastest_tier].lower_path.dentry,
			      sbi->tiers[sbi->fastest_tier].lower_path.mnt,
			      SMOOTHFS_RANGE_REC_DIR, LOOKUP_FOLLOW, &dir);
	if (err == -ENOENT) {
		/* No .smoothfs dir on the fastest tier — nothing to replay.
		 * Still publish a "remount-replay" reason so operators see
		 * the path was exercised. */
		spin_lock(&sbi->write_staging_lock);
		strscpy(sbi->last_recovery_reason, "remount-replay-empty",
			sizeof(sbi->last_recovery_reason));
		spin_unlock(&sbi->write_staging_lock);
		atomic64_set(&sbi->last_recovery_ns, ktime_get_real_ns());
		return 0;
	}
	if (err)
		return err;

	dirf = dentry_open(&dir, O_RDONLY | O_DIRECTORY, current_cred());
	if (IS_ERR(dirf)) {
		err = PTR_ERR(dirf);
		dirf = NULL;
		goto out;
	}
	err = iterate_dir(dirf, &ctx.ctx);
	fput(dirf);
	dirf = NULL;
	if (err < 0)
		goto out;
	if (ctx.oom) {
		err = -ENOMEM;
		goto out;
	}

	list_for_each_entry(entry, &ctx.names, link) {
		struct file *f;
		char *rel;

		rel = kasprintf(GFP_KERNEL, ".smoothfs/%s", entry->name);
		if (!rel) {
			err = -ENOMEM;
			break;
		}
		f = file_open_root(&sbi->tiers[sbi->fastest_tier].lower_path,
				   rel, O_RDONLY, 0);
		kfree(rel);
		if (IS_ERR(f)) {
			pr_warn_ratelimited("smoothfs: range-staging meta open %s failed: %ld\n",
					    entry->name, PTR_ERR(f));
			continue;
		}
		err = smoothfs_range_replay_one(sb, sbi, f, entry->name,
						&recovered_bytes,
						&oldest_ns, &tier_mask);
		fput(f);
		if (err)
			break;
		recovered_files++;
	}

	atomic64_add((s64)recovered_bytes,
		     &sbi->range_staging_recovered_bytes);
	atomic64_add((s64)recovered_files,
		     &sbi->range_staging_recovered_writes);
	atomic64_add((s64)recovered_bytes,
		     &sbi->range_staging_recovery_pending);
	atomic64_add((s64)recovered_bytes, &sbi->staged_bytes);
	atomic64_add((s64)recovered_bytes, &sbi->range_staged_bytes);
	if (oldest_ns) {
		atomic64_set(&sbi->oldest_recovered_write_ns, (s64)oldest_ns);
		if (atomic64_read(&sbi->oldest_staged_write_ns) == 0 ||
		    (s64)oldest_ns <
		    atomic64_read(&sbi->oldest_staged_write_ns))
			atomic64_set(&sbi->oldest_staged_write_ns,
				     (s64)oldest_ns);
	}
	atomic_or((int)tier_mask, &sbi->recovered_range_tier_mask);
	atomic64_set(&sbi->last_recovery_ns, ktime_get_real_ns());

	spin_lock(&sbi->write_staging_lock);
	if (err)
		scnprintf(sbi->last_recovery_reason,
			  sizeof(sbi->last_recovery_reason),
			  "remount-replay-error:%d", err);
	else if (recovered_files == 0)
		strscpy(sbi->last_recovery_reason, "remount-replay-empty",
			sizeof(sbi->last_recovery_reason));
	else
		strscpy(sbi->last_recovery_reason, "remount-replay",
			sizeof(sbi->last_recovery_reason));
	spin_unlock(&sbi->write_staging_lock);

out:
	list_for_each_entry_safe(entry, tmp, &ctx.names, link) {
		list_del(&entry->link);
		kfree(entry);
	}
	if (dir.dentry)
		path_put(&dir);
	return err;
}

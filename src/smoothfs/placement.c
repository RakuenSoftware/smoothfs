// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - per-pool placement log.
 *
 * Format (Phase 0 §0.2): append-only binary log at
 * <fastest_tier>/.smoothfs/placement.log. Each record is fixed 64 bytes
 * for forward compatibility (later phases bolt on varint payloads beyond
 * the header).
 *
 *  offset  size  field
 *  0       8     magic         "SMFPLOG\n" (0x534D46504C4F470A)
 *  8       8     seq           per-pool monotonic uint64
 *  16     16     object_id     UUIDv7
 *  32      1     movement_state
 *  33      1     current_tier
 *  34      1     intended_tier
 *  35      1     pin_state
 *  36      4     gen
 *  40      8     timestamp_ns  ktime_get_real_ns
 *  48     16     reserved (0)
 */

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/uio.h>
#include <linux/cred.h>
#include <linux/path.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/dirent.h>
#include <linux/hashtable.h>
#include <linux/jhash.h>

#include "smoothfs.h"

#define SMOOTHFS_PLACEMENT_MAGIC    0x534D46504C4F470AULL  /* "SMFPLOG\n" */
#define SMOOTHFS_PLACEMENT_DIR      ".smoothfs"
#define SMOOTHFS_PLACEMENT_FILE     ".smoothfs/placement.log"
#define SMOOTHFS_REPLAY_HASH_BITS   15
#define SMOOTHFS_REPLAY_HASH_SIZE   (1U << SMOOTHFS_REPLAY_HASH_BITS)
#define SMOOTHFS_PLACEMENT_WB_INTERVAL_MS 1000
#define SMOOTHFS_PLACEMENT_WB_HIGH_WATER 8192

struct smoothfs_replay_record {
	struct list_head link;
	struct hlist_node hash;
	u8 oid[SMOOTHFS_OID_LEN];
	u8 movement_state;
	u8 current_tier;
	u8 intended_tier;
	u8 pin_state;
	u32 gen;
	u64 seq;
	u8 authoritative_tier;
	u8 normalized_state;
	bool normalized;
	u8 fallback_tier;
	char *fallback_rel_path;
	u32 fallback_gen;
	u8 chosen_tier;
	char *chosen_rel_path;
	u32 chosen_gen;
};

struct smoothfs_scan_name {
	struct list_head link;
	char name[NAME_MAX + 1];
};

struct smoothfs_scan_ctx {
	struct dir_context ctx;
	struct list_head names;
	bool oom;
};

static char *smoothfs_tier_root_string(struct smoothfs_tier *tier)
{
	char *buf, *rendered, *out;

	buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buf)
		return NULL;
	rendered = d_path(&tier->lower_path, buf, PATH_MAX);
	if (IS_ERR(rendered)) {
		kfree(buf);
		return NULL;
	}
	out = kstrdup(rendered, GFP_KERNEL);
	kfree(buf);
	return out;
}

static void smoothfs_replay_index_init(struct hlist_head *index)
{
	u32 i;

	for (i = 0; i < SMOOTHFS_REPLAY_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&index[i]);
}

static int smoothfs_resolve_rel_path(struct smoothfs_tier *tier,
				     const char *rel_path, struct path *out)
{
	char *root, *full;
	int err;

	root = smoothfs_tier_root_string(tier);
	if (!root)
		return -ENOMEM;
	if (rel_path && *rel_path)
		full = kasprintf(GFP_KERNEL, "%s/%s", root, rel_path);
	else
		full = kstrdup(root, GFP_KERNEL);
	kfree(root);
	if (!full)
		return -ENOMEM;
	err = kern_path(full, LOOKUP_FOLLOW, out);
	kfree(full);
	return err;
}

static struct smoothfs_replay_record *
smoothfs_replay_find(struct hlist_head *index, const u8 oid[SMOOTHFS_OID_LEN])
{
	struct smoothfs_replay_record *rec;
	u32 key = jhash(oid, SMOOTHFS_OID_LEN, 0);

	hlist_for_each_entry(rec,
			     &index[hash_min(key, SMOOTHFS_REPLAY_HASH_BITS)],
			     hash) {
		if (memcmp(rec->oid, oid, SMOOTHFS_OID_LEN) == 0)
			return rec;
	}
	return NULL;
}

static struct smoothfs_replay_record *
smoothfs_replay_get_or_create(struct list_head *records,
			      struct hlist_head *index,
			      const u8 oid[SMOOTHFS_OID_LEN])
{
	struct smoothfs_replay_record *rec;
	u32 key = jhash(oid, SMOOTHFS_OID_LEN, 0);

	rec = smoothfs_replay_find(index, oid);
	if (rec)
		return rec;

	rec = kzalloc(sizeof(*rec), GFP_KERNEL);
	if (!rec)
		return NULL;
	INIT_LIST_HEAD(&rec->link);
	memcpy(rec->oid, oid, SMOOTHFS_OID_LEN);
	rec->fallback_tier = U8_MAX;
	rec->chosen_tier = U8_MAX;
	rec->authoritative_tier = U8_MAX;
	list_add_tail(&rec->link, records);
	hlist_add_head(&rec->hash,
		       &index[hash_min(key, SMOOTHFS_REPLAY_HASH_BITS)]);
	return rec;
}

static void smoothfs_replay_normalize(struct smoothfs_replay_record *rec)
{
	rec->normalized = false;
	rec->normalized_state = rec->movement_state;
	rec->authoritative_tier = rec->current_tier;

	switch (rec->movement_state) {
	case SMOOTHFS_MS_PLAN_ACCEPTED:
	case SMOOTHFS_MS_DESTINATION_RESERVED:
	case SMOOTHFS_MS_COPY_IN_PROGRESS:
	case SMOOTHFS_MS_COPY_COMPLETE:
	case SMOOTHFS_MS_COPY_VERIFIED:
		rec->normalized = true;
		rec->normalized_state = SMOOTHFS_MS_PLACED;
		rec->authoritative_tier = rec->current_tier;
		break;
	case SMOOTHFS_MS_CUTOVER_IN_PROGRESS:
	case SMOOTHFS_MS_SWITCHED:
	case SMOOTHFS_MS_CLEANUP_IN_PROGRESS:
		rec->normalized = true;
		rec->normalized_state = SMOOTHFS_MS_PLACED;
		rec->authoritative_tier = rec->intended_tier;
		break;
	default:
		break;
	}
}

static bool smoothfs_scan_actor(struct dir_context *ctx, const char *name,
				int namelen, loff_t offset, u64 ino,
				unsigned int d_type)
{
	struct smoothfs_scan_ctx *scan =
		container_of(ctx, struct smoothfs_scan_ctx, ctx);
	struct smoothfs_scan_name *entry;

	if ((namelen == 1 && name[0] == '.') ||
	    (namelen == 2 && name[0] == '.' && name[1] == '.'))
		return true;

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		scan->oom = true;
		return false;
	}
	INIT_LIST_HEAD(&entry->link);
	memcpy(entry->name, name, namelen);
	entry->name[namelen] = '\0';
	list_add_tail(&entry->link, &scan->names);
	return true;
}

static void smoothfs_scan_names_free(struct list_head *names)
{
	struct smoothfs_scan_name *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, names, link) {
		list_del(&entry->link);
		kfree(entry);
	}
}

static int smoothfs_scan_dir_entries(struct path *dir, struct list_head *names)
{
	struct file *f;
	struct smoothfs_scan_ctx scan = {
		.ctx = {
			.actor = smoothfs_scan_actor,
		},
	};
	int err;

	INIT_LIST_HEAD(&scan.names);
	f = dentry_open(dir, O_RDONLY | O_DIRECTORY, current_cred());
	if (IS_ERR(f))
		return PTR_ERR(f);
	err = iterate_dir(f, &scan.ctx);
	fput(f);
	if (err < 0) {
		smoothfs_scan_names_free(&scan.names);
		return err;
	}
	if (scan.oom) {
		smoothfs_scan_names_free(&scan.names);
		return -ENOMEM;
	}
	list_splice_tail_init(&scan.names, names);
	return 0;
}

static int smoothfs_scan_file(struct list_head *records, u8 tier_rank,
			      struct hlist_head *index,
			      const char *rel_path, struct dentry *dentry)
{
	struct smoothfs_replay_record *rec;
	u8 oid[SMOOTHFS_OID_LEN];
	u32 gen = 0;

	if (!S_ISREG(d_inode(dentry)->i_mode))
		return 0;
	if (smoothfs_read_oid_xattr(dentry, oid))
		return 0;
	(void)smoothfs_read_gen_xattr(dentry, &gen);

	rec = smoothfs_replay_get_or_create(records, index, oid);
	if (!rec)
		return -ENOMEM;
	if (rec->seq == 0 && rec->chosen_rel_path == NULL) {
		rec->movement_state = SMOOTHFS_MS_PLACED;
		rec->current_tier = tier_rank;
		rec->intended_tier = tier_rank;
		rec->pin_state = SMOOTHFS_PIN_NONE;
		rec->gen = gen;
		smoothfs_replay_normalize(rec);
	}
	if (!rec->fallback_rel_path) {
		rec->fallback_rel_path = kstrdup(rel_path, GFP_KERNEL);
		if (!rec->fallback_rel_path)
			return -ENOMEM;
		rec->fallback_tier = tier_rank;
		rec->fallback_gen = gen;
	}
	if (tier_rank == rec->authoritative_tier && !rec->chosen_rel_path) {
		rec->chosen_rel_path = kstrdup(rel_path, GFP_KERNEL);
		if (!rec->chosen_rel_path)
			return -ENOMEM;
		rec->chosen_tier = tier_rank;
		rec->chosen_gen = gen;
	}
	return 0;
}

static int smoothfs_scan_tree(struct list_head *records, struct path *dir,
			      struct hlist_head *index, u8 tier_rank,
			      const char *prefix)
{
	LIST_HEAD(names);
	struct smoothfs_scan_name *entry, *tmp;
	int err;

	err = smoothfs_scan_dir_entries(dir, &names);
	if (err)
		return err;

	list_for_each_entry_safe(entry, tmp, &names, link) {
		struct qstr qname = QSTR_INIT(entry->name, strlen(entry->name));
		struct dentry *child;
		struct path child_path;
		char *rel_path;

		if (prefix && *prefix)
			rel_path = kasprintf(GFP_KERNEL, "%s/%s", prefix, entry->name);
		else
			rel_path = kstrdup(entry->name, GFP_KERNEL);
		if (!rel_path) {
			err = -ENOMEM;
			break;
		}

		inode_lock_shared(d_inode(dir->dentry));
		child = smoothfs_compat_lookup(&nop_mnt_idmap, &qname, dir->dentry);
		inode_unlock_shared(d_inode(dir->dentry));
		if (IS_ERR(child)) {
			kfree(rel_path);
			err = PTR_ERR(child);
			break;
		}
		if (d_really_is_negative(child)) {
			dput(child);
			kfree(rel_path);
			continue;
		}
		child_path.mnt = dir->mnt;
		child_path.dentry = child;
		mntget(child_path.mnt);

		if (S_ISDIR(d_inode(child)->i_mode)) {
			if (strcmp(rel_path, SMOOTHFS_PLACEMENT_DIR) != 0)
				err = smoothfs_scan_tree(records, &child_path,
							 index, tier_rank, rel_path);
		} else {
			err = smoothfs_scan_file(records, tier_rank, index,
						 rel_path, child);
		}
		path_put(&child_path);
		kfree(rel_path);
		if (err)
			break;
	}
	smoothfs_scan_names_free(&names);
	return err;
}

static void smoothfs_replay_free_records(struct list_head *records)
{
	struct smoothfs_replay_record *rec, *tmp;

	list_for_each_entry_safe(rec, tmp, records, link) {
		list_del(&rec->link);
		hlist_del_init(&rec->hash);
		kfree(rec->fallback_rel_path);
		kfree(rec->chosen_rel_path);
		kfree(rec);
	}
}

static int smoothfs_replay_load_log(struct smoothfs_sb_info *sbi,
				    struct list_head *records,
				    struct hlist_head *index)
{
	u8 buf[SMOOTHFS_PLACEMENT_REC_SIZE];
	loff_t pos = 0;

	for (;;) {
		struct smoothfs_replay_record *rec;
		ssize_t n;
		u64 magic;
		u8 oid[SMOOTHFS_OID_LEN];
		u64 seq;

		n = kernel_read(sbi->placement_log, buf, sizeof(buf), &pos);
		if (n == 0)
			return 0;
		if (n < 0)
			return n;
		if (n != sizeof(buf))
			return -EIO;

		magic = le64_to_cpu(*(__le64 *)&buf[0]);
		if (magic != SMOOTHFS_PLACEMENT_MAGIC)
			return -EINVAL;
		seq = le64_to_cpu(*(__le64 *)&buf[8]);
		memcpy(oid, &buf[16], SMOOTHFS_OID_LEN);

		rec = smoothfs_replay_get_or_create(records, index, oid);
		if (!rec)
			return -ENOMEM;
		if (seq < rec->seq)
			continue;

		rec->seq = seq;
		rec->movement_state = buf[32];
		rec->current_tier = buf[33];
		rec->intended_tier = buf[34];
		rec->pin_state = buf[35];
		rec->gen = le32_to_cpu(*(__le32 *)&buf[36]);
		smoothfs_replay_normalize(rec);
	}
}

static int smoothfs_replay_instantiate(struct super_block *sb,
				       struct smoothfs_sb_info *sbi,
				       struct list_head *records)
{
	struct smoothfs_replay_record *rec;
	int err;

	list_for_each_entry(rec, records, link) {
		struct path lower;
		struct inode *inode;
		struct smoothfs_inode_info *si;
		u8 tier;
		char *rel_path;
		bool normalized;

		if (!rec->chosen_rel_path && rec->fallback_rel_path) {
			rec->chosen_rel_path = rec->fallback_rel_path;
			rec->fallback_rel_path = NULL;
			rec->chosen_tier = rec->fallback_tier;
			rec->chosen_gen = rec->fallback_gen;
		}
		if (!rec->chosen_rel_path || rec->chosen_tier >= sbi->ntiers)
			continue;

		tier = rec->chosen_tier;
		rel_path = rec->chosen_rel_path;
		normalized = rec->normalized;

		err = smoothfs_resolve_rel_path(&sbi->tiers[tier], rel_path, &lower);
		if (err)
			continue;
		inode = smoothfs_iget(sb, &lower, false, false);
		path_put(&lower);
		if (IS_ERR(inode))
			return PTR_ERR(inode);

		si = SMOOTHFS_I(inode);
		kfree(si->rel_path);
		si->rel_path = kstrdup(rel_path, GFP_KERNEL);
		if (!si->rel_path)
			return -ENOMEM;
		si->current_tier = tier;
		si->intended_tier = normalized ? tier : rec->intended_tier;
		si->movement_state = rec->normalized_state;
		si->pin_state = rec->pin_state;
		si->gen = rec->chosen_gen;
		if (normalized)
			si->transaction_seq = 0;

		if (normalized)
			smoothfs_placement_record(sbi, rec->oid, SMOOTHFS_MS_PLACED,
						  tier, tier, /*sync=*/true);
		/* Keep the replayed inode pinned in-cache so OID- and
		 * rel_path-based recovery survives until a real dentry alias
		 * is created by lookup/open after remount. The matching iput
		 * happens lazily in smoothfs_lookup (when a dentry alias takes
		 * over) or, for any remaining pins at unmount time, in
		 * smoothfs_kill_sb so generic_shutdown_super doesn't see
		 * "Busy inodes". */
		atomic_set(&si->replay_pinned, 1);
	}
	return 0;
}

static int smoothfs_placement_ensure_dir(struct smoothfs_sb_info *sbi)
{
	struct path parent = sbi->tiers[sbi->fastest_tier].lower_path;
	struct qstr name = QSTR_INIT(SMOOTHFS_PLACEMENT_DIR,
				     strlen(SMOOTHFS_PLACEMENT_DIR));
	struct dentry *dir;
	int err = 0;

	inode_lock(d_inode(parent.dentry));
	dir = smoothfs_compat_lookup(&nop_mnt_idmap, &name, parent.dentry);
	if (IS_ERR(dir)) {
		inode_unlock(d_inode(parent.dentry));
		return PTR_ERR(dir);
	}

	if (d_really_is_negative(dir)) {
		struct dentry *new_dir;
		new_dir = smoothfs_compat_mkdir(&nop_mnt_idmap,
						d_inode(parent.dentry),
						dir, 0700);
		if (IS_ERR(new_dir))
			err = PTR_ERR(new_dir);
		else if (new_dir != dir) {
			dput(dir);
			dir = new_dir;
		}
	}
	dput(dir);
	inode_unlock(d_inode(parent.dentry));
	return err;
}

int smoothfs_placement_open(struct smoothfs_sb_info *sbi)
{
	struct path full;
	struct file *f;
	char *path_buf;
	int err;

	err = smoothfs_placement_ensure_dir(sbi);
	if (err)
		return err;

	path_buf = kasprintf(GFP_KERNEL, "%pd4/%s",
			     sbi->tiers[sbi->fastest_tier].lower_path.dentry,
			     SMOOTHFS_PLACEMENT_FILE);
	if (!path_buf)
		return -ENOMEM;

	err = kern_path(path_buf, LOOKUP_FOLLOW, &full);
	if (err == -ENOENT) {
		/* Create it via filp_open with O_CREAT against the lower
		 * mount root. */
		f = file_open_root(&sbi->tiers[sbi->fastest_tier].lower_path,
				   SMOOTHFS_PLACEMENT_FILE,
				   O_RDWR | O_CREAT | O_APPEND, 0600);
	} else if (err) {
		kfree(path_buf);
		return err;
	} else {
		f = dentry_open(&full, O_RDWR | O_APPEND, current_cred());
		path_put(&full);
	}
	kfree(path_buf);

	if (IS_ERR(f))
		return PTR_ERR(f);

	sbi->placement_log = f;
	sbi->placement_seq = i_size_read(file_inode(f)) /
			     SMOOTHFS_PLACEMENT_REC_SIZE;
	return 0;
}

static void smoothfs_placement_wb_worker(struct work_struct *work)
{
	struct smoothfs_sb_info *sbi = container_of(to_delayed_work(work),
						     struct smoothfs_sb_info,
						     placement_wb_work);
	LIST_HEAD(local);
	struct smoothfs_placement_wb_entry *e, *tmp;
	struct file *log;

	spin_lock(&sbi->placement_wb_lock);
	list_splice_init(&sbi->placement_wb_pending, &local);
	sbi->placement_wb_pending_count = 0;
	spin_unlock(&sbi->placement_wb_lock);

	mutex_lock(&sbi->placement_lock);
	log = sbi->placement_log;
	list_for_each_entry_safe(e, tmp, &local, link) {
		loff_t pos = 0;
		ssize_t n;

		if (log) {
			n = kernel_write(log, e->record,
					 SMOOTHFS_PLACEMENT_REC_SIZE, &pos);
			if (n != SMOOTHFS_PLACEMENT_REC_SIZE)
				pr_warn_ratelimited("smoothfs: placement log write failed: %zd\n",
						    n >= 0 ? -EIO : n);
		}
		list_del(&e->link);
		kfree(e);
	}
	mutex_unlock(&sbi->placement_lock);
}

int smoothfs_placement_wb_init(struct smoothfs_sb_info *sbi)
{
	spin_lock_init(&sbi->placement_wb_lock);
	INIT_LIST_HEAD(&sbi->placement_wb_pending);
	sbi->placement_wb_pending_count = 0;
	INIT_DELAYED_WORK(&sbi->placement_wb_work,
			  smoothfs_placement_wb_worker);
	sbi->placement_wb_wq = alloc_workqueue("smoothfs-placement-wb",
					       WQ_UNBOUND | WQ_MEM_RECLAIM,
					       0);
	if (!sbi->placement_wb_wq)
		return -ENOMEM;
	WRITE_ONCE(sbi->placement_wb_ready, true);
	return 0;
}

void smoothfs_placement_wb_kick(struct smoothfs_sb_info *sbi)
{
	if (READ_ONCE(sbi->placement_wb_ready))
		mod_delayed_work(sbi->placement_wb_wq,
				 &sbi->placement_wb_work, 0);
}

void smoothfs_placement_wb_drain(struct smoothfs_sb_info *sbi)
{
	if (!READ_ONCE(sbi->placement_wb_ready))
		return;
	smoothfs_placement_wb_kick(sbi);
	flush_delayed_work(&sbi->placement_wb_work);
}

void smoothfs_placement_wb_destroy(struct smoothfs_sb_info *sbi)
{
	if (!sbi->placement_wb_ready)
		return;
	WRITE_ONCE(sbi->placement_wb_ready, false);
	cancel_delayed_work_sync(&sbi->placement_wb_work);
	smoothfs_placement_wb_worker(&sbi->placement_wb_work.work);
	destroy_workqueue(sbi->placement_wb_wq);
	sbi->placement_wb_wq = NULL;
}

void smoothfs_placement_close(struct smoothfs_sb_info *sbi)
{
	if (!sbi->placement_log)
		return;
	smoothfs_placement_wb_drain(sbi);
	vfs_fsync(sbi->placement_log, 0);
	fput(sbi->placement_log);
	sbi->placement_log = NULL;
}

int smoothfs_placement_record(struct smoothfs_sb_info *sbi,
			      const u8 oid[SMOOTHFS_OID_LEN],
			      u8 movement_state, u8 current_tier,
			      u8 intended_tier, bool sync)
{
	struct smoothfs_placement_wb_entry *e;
	__le64 *magic;
	__le64 *seq;
	__le32 *gen;
	__le64 *ts;
	unsigned int count;
	bool kick_now = sync;

	if (!READ_ONCE(sbi->placement_wb_ready)) {
		pr_warn_ratelimited("smoothfs: placement writeback unavailable; dropping recoverable record\n");
		return 0;
	}

	e = kzalloc(sizeof(*e), GFP_NOFS);
	if (!e) {
		pr_warn_ratelimited("smoothfs: placement writeback allocation failed; dropping recoverable record\n");
		return 0;
	}

	magic = (__le64 *)&e->record[0];
	seq   = (__le64 *)&e->record[8];
	gen   = (__le32 *)&e->record[36];
	ts    = (__le64 *)&e->record[40];
	*magic = cpu_to_le64(SMOOTHFS_PLACEMENT_MAGIC);
	memcpy(&e->record[16], oid, SMOOTHFS_OID_LEN);
	e->record[32] = movement_state;
	e->record[33] = current_tier;
	e->record[34] = intended_tier;
	e->record[35] = SMOOTHFS_PIN_NONE;  /* per-record pin not modelled in Phase 1 */
	*gen   = cpu_to_le32(0);

	spin_lock(&sbi->placement_wb_lock);
	*seq = cpu_to_le64(++sbi->placement_seq);
	*ts = cpu_to_le64(ktime_get_real_ns());
	list_add_tail(&e->link, &sbi->placement_wb_pending);
	count = ++sbi->placement_wb_pending_count;
	if (count >= SMOOTHFS_PLACEMENT_WB_HIGH_WATER)
		kick_now = true;
	spin_unlock(&sbi->placement_wb_lock);

	if (kick_now)
		smoothfs_placement_wb_kick(sbi);
	else
		queue_delayed_work(sbi->placement_wb_wq,
				   &sbi->placement_wb_work,
				   msecs_to_jiffies(SMOOTHFS_PLACEMENT_WB_INTERVAL_MS));
	return 0;
}

int smoothfs_placement_replay(struct super_block *sb,
			      struct smoothfs_sb_info *sbi)
{
	LIST_HEAD(records);
	struct hlist_head *index;
	u8 tier;
	int err;

	if (!sbi->placement_log)
		return -ENODEV;

	index = kvcalloc(SMOOTHFS_REPLAY_HASH_SIZE, sizeof(*index), GFP_KERNEL);
	if (!index)
		return -ENOMEM;
	smoothfs_replay_index_init(index);

	err = smoothfs_replay_load_log(sbi, &records, index);
	if (err)
		goto out;

	for (tier = 0; tier < sbi->ntiers; tier++) {
		err = smoothfs_scan_tree(&records, &sbi->tiers[tier].lower_path,
					 index, tier, "");
		if (err)
			goto out;
	}

	err = smoothfs_replay_instantiate(sb, sbi, &records);

out:
	smoothfs_replay_free_records(&records);
	kvfree(index);
	return err;
}

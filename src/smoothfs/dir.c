// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - directory file_operations.
 *
 * Phase 2: iterate_shared presents a merged directory view across the
 * canonical tier plus any spill tiers. Canonical-tier names win on
 * duplicates; later tiers fill holes only.
 */

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/dcache.h>
#include <linux/slab.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/limits.h>
#include <linux/string.h>

#include "smoothfs.h"

struct smoothfs_dir_entry {
	char         *name;
	unsigned int  namelen;
	u64           ino;
	unsigned int  dtype;
};

struct smoothfs_dir_cache {
	struct smoothfs_dir_entry *entries;
	size_t                    count;
	size_t                    cap;
};

struct smoothfs_collect_ctx {
	struct dir_context          ctx;
	struct smoothfs_dir_cache  *cache;
	int                         err;
};

static void smoothfs_dir_cache_free(struct smoothfs_dir_cache *cache)
{
	size_t i;

	if (!cache)
		return;
	for (i = 0; i < cache->count; i++)
		kfree(cache->entries[i].name);
	kfree(cache->entries);
	kfree(cache);
}

static bool smoothfs_dir_cache_has(struct smoothfs_dir_cache *cache,
				   const char *name, int namelen)
{
	size_t i;

	for (i = 0; i < cache->count; i++) {
		if (cache->entries[i].namelen == namelen &&
		    !memcmp(cache->entries[i].name, name, namelen))
			return true;
	}
	return false;
}

static int smoothfs_dir_cache_add(struct smoothfs_dir_cache *cache,
				  const char *name, int namelen,
				  u64 ino, unsigned int dtype)
{
	struct smoothfs_dir_entry *grown;
	char *dup;

	if ((namelen == 1 && name[0] == '.') ||
	    (namelen == 2 && name[0] == '.' && name[1] == '.'))
		return 0;
	if (smoothfs_dir_cache_has(cache, name, namelen))
		return 0;
	if (cache->count == cache->cap) {
		size_t new_cap = cache->cap ? cache->cap * 2 : 64;

		grown = krealloc(cache->entries,
				 new_cap * sizeof(*cache->entries),
				 GFP_KERNEL);
		if (!grown)
			return -ENOMEM;
		cache->entries = grown;
		cache->cap = new_cap;
	}

	dup = kmalloc(namelen + 1, GFP_KERNEL);
	if (!dup)
		return -ENOMEM;
	memcpy(dup, name, namelen);
	dup[namelen] = '\0';

	cache->entries[cache->count].name = dup;
	cache->entries[cache->count].namelen = namelen;
	cache->entries[cache->count].ino = ino;
	cache->entries[cache->count].dtype = dtype;
	cache->count++;
	return 0;
}

static bool smoothfs_collect_actor(struct dir_context *ctx, const char *name,
				   int namelen, loff_t offset, u64 ino,
				   unsigned int d_type)
{
	struct smoothfs_collect_ctx *collect =
		container_of(ctx, struct smoothfs_collect_ctx, ctx);

	collect->err = smoothfs_dir_cache_add(collect->cache, name, namelen,
					      ino, d_type);
	return collect->err == 0;
}

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

static int smoothfs_collect_one_dir(struct smoothfs_dir_cache *cache,
				    struct path *dir_path)
{
	struct file *dirf;
	struct smoothfs_collect_ctx collect = {
		.ctx = {
			.actor = smoothfs_collect_actor,
		},
		.cache = cache,
	};
	int err;

	dirf = dentry_open(dir_path, O_RDONLY | O_DIRECTORY, current_cred());
	if (IS_ERR(dirf))
		return PTR_ERR(dirf);
	err = iterate_dir(dirf, &collect.ctx);
	fput(dirf);
	if (err < 0)
		return err;
	return collect.err;
}

static struct smoothfs_dir_cache *smoothfs_build_dir_cache(struct file *file)
{
	struct inode *inode = file_inode(file);
	struct smoothfs_inode_info *si = SMOOTHFS_I(inode);
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(inode->i_sb);
	struct smoothfs_dir_cache *cache;
	char *rel_path;
	u8 canonical_tier, tier;
	int err;

	cache = kzalloc(sizeof(*cache), GFP_KERNEL);
	if (!cache)
		return ERR_PTR(-ENOMEM);

	rel_path = smoothfs_rel_path_from_dentry(file->f_path.dentry);
	if (!rel_path) {
		smoothfs_dir_cache_free(cache);
		return ERR_PTR(-ENOMEM);
	}

	canonical_tier = si->current_tier;
	if (canonical_tier >= sbi->ntiers)
		canonical_tier = sbi->fastest_tier;

	err = smoothfs_collect_one_dir(cache, &si->lower_path);
	if (err) {
		kfree(rel_path);
		smoothfs_dir_cache_free(cache);
		return ERR_PTR(err);
	}

	for (tier = 0; tier < sbi->ntiers; tier++) {
		struct path tier_path;

		if (tier == canonical_tier)
			continue;
		if (!smoothfs_metadata_tier_active(sbi, tier)) {
			smoothfs_note_metadata_tier_skip(sbi);
			continue;
		}
		err = smoothfs_resolve_rel_path_on_tier(sbi, tier, rel_path, &tier_path);
		if (err)
			continue;
		if (d_is_dir(tier_path.dentry))
			(void)smoothfs_collect_one_dir(cache, &tier_path);
		path_put(&tier_path);
	}

	kfree(rel_path);
	return cache;
}

static int smoothfs_opendir(struct inode *inode, struct file *file)
{
	return smoothfs_open_lower(file, inode);
}

static int smoothfs_releasedir(struct inode *inode, struct file *file)
{
	struct smoothfs_file_info *fi = file->private_data;

	if (fi && fi->dir_cache) {
		smoothfs_dir_cache_free(fi->dir_cache);
		fi->dir_cache = NULL;
	}
	return smoothfs_release_lower(file);
}

static int smoothfs_iterate_shared(struct file *file, struct dir_context *ctx)
{
	struct smoothfs_file_info *fi = file->private_data;
	struct smoothfs_dir_cache *cache;
	loff_t idx;

	if (!fi)
		return -EBADF;

	if (!dir_emit_dots(file, ctx)) {
		file->f_pos = ctx->pos;
		return 0;
	}

	cache = fi->dir_cache;
	if (!cache) {
		cache = smoothfs_build_dir_cache(file);
		if (IS_ERR(cache))
			return PTR_ERR(cache);
		fi->dir_cache = cache;
	}

	idx = ctx->pos - 2;
	while (idx < cache->count) {
		struct smoothfs_dir_entry *entry = &cache->entries[idx];

		if (!dir_emit(ctx, entry->name, entry->namelen,
			      entry->ino, entry->dtype))
			break;
		ctx->pos++;
		idx++;
	}
	file->f_pos = ctx->pos;
	return 0;
}

static loff_t smoothfs_dir_llseek(struct file *file, loff_t offset, int whence)
{
	struct smoothfs_file_info *fi = file->private_data;
	loff_t ret = generic_file_llseek(file, offset, whence);

	if (ret == 0 && fi && fi->dir_cache) {
		smoothfs_dir_cache_free(fi->dir_cache);
		fi->dir_cache = NULL;
	}
	return ret;
}

const struct file_operations smoothfs_dir_ops = {
	.owner          = THIS_MODULE,
	.open           = smoothfs_opendir,
	.release        = smoothfs_releasedir,
	.iterate_shared = smoothfs_iterate_shared,
	.llseek         = smoothfs_dir_llseek,
	.read           = generic_read_dir,
};

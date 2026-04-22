// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - module init/exit and filesystem registration.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/slab.h>

#include "smoothfs.h"

static_assert(sizeof(struct smoothfs_inode_info) <= 4096);

struct kmem_cache *smoothfs_inode_cachep;

static void smoothfs_init_once(void *foo)
{
	struct smoothfs_inode_info *si = foo;

	inode_init_once(&si->vfs_inode);
}

static int __init smoothfs_init_inodecache(void)
{
	smoothfs_inode_cachep = kmem_cache_create(
		"smoothfs_inode_cache",
		sizeof(struct smoothfs_inode_info),
		0,
		(SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT),
		smoothfs_init_once);
	if (!smoothfs_inode_cachep)
		return -ENOMEM;
	return 0;
}

static void smoothfs_destroy_inodecache(void)
{
	rcu_barrier();
	kmem_cache_destroy(smoothfs_inode_cachep);
}

/*
 * Custom kill_sb that releases any placement_replay-held inode pins
 * before generic_shutdown_super walks evict_inodes. Without this step
 * the pinned-but-never-looked-up inodes from a recovery boot would
 * trip generic_shutdown_super's "Busy inodes after unmount" warning
 * and (worse) leave the sb's s_umount in a half-locked state that
 * wedges subsequent global sync() callers in super_lock.
 *
 * Walking inode_list under inode_lock is safe here: we're after the
 * last umount caller, so no new inodes can be created on this sb.
 */
static void smoothfs_kill_sb(struct super_block *sb)
{
	struct smoothfs_sb_info *sbi = SMOOTHFS_SB(sb);
	struct smoothfs_inode_info *si, *tmp;
	LIST_HEAD(to_release);

	if (sbi) {
		down_write(&sbi->inode_lock);
		list_for_each_entry_safe(si, tmp, &sbi->inode_list, sb_link) {
			if (atomic_xchg(&si->replay_pinned, 0))
				list_move_tail(&si->sb_link, &to_release);
		}
		up_write(&sbi->inode_lock);

		list_for_each_entry_safe(si, tmp, &to_release, sb_link) {
			list_del_init(&si->sb_link);
			iput(&si->vfs_inode);
		}
	}

	kill_anon_super(sb);
}

struct file_system_type smoothfs_fs_type = {
	.owner          = THIS_MODULE,
	.name           = SMOOTHFS_NAME,
	.init_fs_context = smoothfs_init_fs_context,
	.kill_sb        = smoothfs_kill_sb,
	.fs_flags       = FS_USERNS_MOUNT,
};
MODULE_ALIAS_FS(SMOOTHFS_NAME);

static int __init smoothfs_init(void)
{
	int err;

	err = smoothfs_init_inodecache();
	if (err)
		return err;

	err = smoothfs_netlink_init();
	if (err)
		goto out_cache;

	err = register_filesystem(&smoothfs_fs_type);
	if (err)
		goto out_netlink;

	pr_info("smoothfs: loaded\n");
	return 0;

out_netlink:
	smoothfs_netlink_exit();
out_cache:
	smoothfs_destroy_inodecache();
	return err;
}

static void __exit smoothfs_exit(void)
{
	unregister_filesystem(&smoothfs_fs_type);
	smoothfs_netlink_exit();
	smoothfs_destroy_inodecache();
	pr_info("smoothfs: unloaded\n");
}

module_init(smoothfs_init);
module_exit(smoothfs_exit);

MODULE_AUTHOR("RakuenSoftware");
MODULE_DESCRIPTION("Stacked tiering filesystem (Phase 1)");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1.0");

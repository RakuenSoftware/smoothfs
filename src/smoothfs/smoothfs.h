/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * smoothfs - native kernel stacked tiering filesystem (Phase 1 scaffold)
 *
 * Per docs/proposals/pending/smoothfs-stacked-tiering.md and
 * docs/proposals/pending/smoothfs-phase-0-contract.md.
 *
 * Phase 1 scope: passthrough stacked filesystem over a single lower per
 * tier, with stable object_id (UUIDv7) persisted in trusted.smoothfs.oid
 * xattr on the lower file, and a per-pool placement log on the fastest
 * tier. Movement (Phase 2), protocol exports (Phase 4-6), and heat
 * sampling (Phase 2) are NOT implemented; their hooks exist as stubs.
 */

#ifndef _SMOOTHFS_H
#define _SMOOTHFS_H

#include <linux/fs.h>
#include <linux/types.h>
#include <linux/path.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/rwsem.h>
#include <linux/rhashtable.h>
#include <linux/srcu.h>
#include <linux/uuid.h>
#include <linux/xattr.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/version.h>
#include <linux/kobject.h>

#include "uapi_smoothfs.h"
#include "compat.h"

#define SMOOTHFS_NAME           "smoothfs"
#define SMOOTHFS_MAGIC          0x534D4F46  /* 'SMOF' */
#define SMOOTHFS_MAX_TIERS      8
#define SMOOTHFS_OID_XATTR      "trusted.smoothfs.oid"
#define SMOOTHFS_GEN_XATTR      "trusted.smoothfs.gen"
#define SMOOTHFS_FILEID_XATTR   "trusted.smoothfs.fileid"
#define SMOOTHFS_LEASE_XATTR    "trusted.smoothfs.lease"
#define SMOOTHFS_OID_LEN        16  /* UUIDv7, 128 bits */
#define SMOOTHFS_GEN_LEN        4   /* monotonic uint32 */
#define SMOOTHFS_PLACEMENT_REC_SIZE 64

/* Lower-fs capability bits, mirroring Phase 0 contract §0.6. */
#define SMOOTHFS_CAP_XATTR_USER             BIT(0)
#define SMOOTHFS_CAP_XATTR_TRUSTED          BIT(1)
#define SMOOTHFS_CAP_XATTR_SECURITY         BIT(2)
#define SMOOTHFS_CAP_POSIX_ACL              BIT(3)
#define SMOOTHFS_CAP_RENAME_ATOMIC          BIT(4)
#define SMOOTHFS_CAP_HARDLINK               BIT(5)
#define SMOOTHFS_CAP_MMAP_COHERENT          BIT(6)
#define SMOOTHFS_CAP_DIRECT_IO              BIT(7)
#define SMOOTHFS_CAP_FSYNC_DURABLE          BIT(8)
#define SMOOTHFS_CAP_SPARSE_SEEK            BIT(9)
#define SMOOTHFS_CAP_INODE_GENERATION       BIT(10)
#define SMOOTHFS_CAP_QUOTA_USER             BIT(11)
#define SMOOTHFS_CAP_QUOTA_PROJECT          BIT(12)
#define SMOOTHFS_CAP_REFLINK                BIT(13)
#define SMOOTHFS_CAP_COPY_FILE_RANGE        BIT(14)
#define SMOOTHFS_CAP_FSCRYPT                BIT(15)

/* Required-bit set per Phase 0.6. Mount fails if any required bit is clear. */
#define SMOOTHFS_CAPS_REQUIRED \
	(SMOOTHFS_CAP_XATTR_USER     | SMOOTHFS_CAP_XATTR_TRUSTED   | \
	 SMOOTHFS_CAP_XATTR_SECURITY | SMOOTHFS_CAP_POSIX_ACL       | \
	 SMOOTHFS_CAP_RENAME_ATOMIC  | SMOOTHFS_CAP_HARDLINK        | \
	 SMOOTHFS_CAP_MMAP_COHERENT  | SMOOTHFS_CAP_DIRECT_IO       | \
	 SMOOTHFS_CAP_FSYNC_DURABLE  | SMOOTHFS_CAP_SPARSE_SEEK     | \
	 SMOOTHFS_CAP_INODE_GENERATION)

/*
 * Per-tier descriptor inside a smoothfs pool. Each tier is one
 * pre-mounted lower filesystem (XFS-on-LV or ZFS dataset in Phase 1).
 */
struct smoothfs_tier {
	u8           rank;
	u32          caps;
	struct path  lower_path;     /* root of the lower mount */
	const char  *lower_id;       /* tier_targets.id from SQLite, kstrdup'd */
};

/* Movement state, mirroring §0.3 of the Phase 0 contract. */
enum smoothfs_movement_state {
	SMOOTHFS_MS_PLACED               = 0,
	SMOOTHFS_MS_PLAN_ACCEPTED        = 1,
	SMOOTHFS_MS_DESTINATION_RESERVED = 2,
	SMOOTHFS_MS_COPY_IN_PROGRESS     = 3,
	SMOOTHFS_MS_COPY_COMPLETE        = 4,
	SMOOTHFS_MS_COPY_VERIFIED        = 5,
	SMOOTHFS_MS_CUTOVER_IN_PROGRESS  = 6,
	SMOOTHFS_MS_SWITCHED             = 7,
	SMOOTHFS_MS_CLEANUP_IN_PROGRESS  = 8,
	SMOOTHFS_MS_CLEANUP_COMPLETE     = 9,
	SMOOTHFS_MS_FAILED               = 10,
	SMOOTHFS_MS_STALE                = 11,
};

enum smoothfs_pin_state {
	SMOOTHFS_PIN_NONE     = 0,
	SMOOTHFS_PIN_HOT      = 1,
	SMOOTHFS_PIN_COLD     = 2,
	SMOOTHFS_PIN_HARDLINK = 3,
	SMOOTHFS_PIN_LEASE    = 4,
	SMOOTHFS_PIN_LUN      = 5,
};

/*
 * Per-pool runtime state. One per smoothfs mount.
 */
struct smoothfs_sb_info {
	uuid_t              pool_uuid;
	char                pool_name[64];
	u32                 fsid;            /* xxhash32(pool_uuid), per §0.7 */
	u8                  ntiers;
	u8                  fastest_tier;    /* rank 0 always; recorded for clarity */
	struct smoothfs_tier tiers[SMOOTHFS_MAX_TIERS];

	/* Placement log on the fastest tier (per §0.2 metadata-on-SSD). */
	struct file        *placement_log;
	struct mutex        placement_lock;
	u64                 placement_seq;
	struct workqueue_struct *placement_wb_wq;
	struct delayed_work placement_wb_work;
	spinlock_t          placement_wb_lock;
	struct list_head    placement_wb_pending;
	unsigned int        placement_wb_pending_count;
	bool                placement_wb_ready;

	/* OID -> smoothfs_inode map. rhashtable gives O(1) lookups on the
	 * hot path (stat, NFS fh_to_dentry, movement netlink commands).
	 * The list + rwsem remain as the canonical iteration surface for
	 * periodic work (heat drain, pool-wide state sweeps, mount-time
	 * placement-log replay) — insert/remove touches both. */
	struct rhashtable   oid_map;
	bool                oid_map_ready;

	/* (tier_idx, lower_inode_no) -> smoothfs ino_no cache. Lets
	 * smoothfs_iget skip the two vfs_getxattr calls on CREATE and
	 * cold-cache opens when we've seen this lower inode before.
	 * Lower i_ino is stable for the mount lifetime, so the mapping
	 * is durable until the smoothfs inode is evicted. */
	struct rhashtable   lower_ino_map;
	bool                lower_ino_map_ready;

	struct rw_semaphore inode_lock;
	struct list_head    inode_list;

	/* True if any lower filesystem installs a d_revalidate callback.
	 * Set at mount time from the capability probe; read (RCU-safe) by
	 * smoothfs_d_revalidate to fast-return 1 for lowers that don't
	 * revalidate (xfs, ext4, btrfs, zfs — the current Phase 3 set). */
	bool                any_lower_revalidates;

	/* Object_id allocator state (UUIDv7 monotonic counter). */
	atomic64_t          oid_monotonic;

	/* Heat drain work — periodic walk of inode_list, emit deltas via
	 * netlink (Phase 2 §0.5). */
	struct delayed_work heat_drain_work;
	bool                heat_drain_active;

	/* Movement transaction sequence — monotonic per pool. */
	atomic64_t          transaction_seq;

	/* Quiesce gate: when true, MOVE_PLAN/MOVE_CUTOVER refuse with
	 * -EAGAIN. Toggled by SMOOTHFS_CMD_QUIESCE. */
	bool                quiesced;

	/* Cutover drain. Writers wrap vfs_iter_write in
	 * srcu_read_lock(&cutover_srcu); cutover calls synchronize_srcu()
	 * after setting movement_state = CUTOVER_IN_PROGRESS to wait for
	 * all in-flight writes to finish. Cheaper than the per-inode
	 * atomic_t it replaced, at the cost of cutover waiting through
	 * an RCU grace period (~ms) — cutover is never on a benchmark
	 * path, so that cost is acceptable. */
	struct srcu_struct  cutover_srcu;
	bool                cutover_srcu_ready;

	/* OID xattr writeback queue.
	 *
	 * CREATE's synchronous __vfs_setxattr was measured at ~0.9 µs out
	 * of ~5.5 µs total smoothfs_create in the Phase 3 harness, and it
	 * was the top contributor to the ~50 µs CREATE p99. We stash
	 * newly-minted OIDs on this queue instead of writing them through
	 * to the lower's xattr store; a workqueue flushes the queue in
	 * batches. sync_fs and put_super drain synchronously.
	 *
	 * Durability: queued-but-not-flushed OIDs are lost on a crash
	 * between queue and flush. The file itself is still on the lower
	 * (vfs_create commits to the lower's metadata journal normally)
	 * — next mount sees the file without trusted.smoothfs.oid and
	 * mints a fresh OID for it. That changes the file's synthesised
	 * ino_no across the crash but preserves file contents. Documented
	 * as a §0.1 addendum: "OID is durably assigned on first sync_fs
	 * after CREATE; prior to that it may be re-minted across crash."
	 */
	struct workqueue_struct *oid_wb_wq;
	struct delayed_work  oid_wb_work;
	spinlock_t           oid_wb_lock;
	struct list_head     oid_wb_pending;
	unsigned int         oid_wb_pending_count;
	bool                 oid_wb_ready;

	/* Spill observability. Exported via per-pool sysfs files and the
	 * generic-netlink spill event path. */
	atomic64_t          spill_creates_total;
	atomic64_t          spill_creates_failed_all_tiers;
	atomic_t            any_spill_since_mount;
	void               *sysfs_pool;
};

static inline u8 smoothfs_tier_of(struct smoothfs_sb_info *sbi,
				  struct vfsmount *mnt)
{
	u8 i;

	for (i = 0; i < sbi->ntiers; i++)
		if (sbi->tiers[i].lower_path.mnt == mnt)
			return i;
	return SMOOTHFS_MAX_TIERS;
}

/* Default drain interval — overridable per pool via Phase 0 §0.5
 * tunables. Phase 2 hard-codes; later we plumb from tierd. */
#define SMOOTHFS_HEAT_DRAIN_MS  30000

/*
 * Per-inode smoothfs state. Embeds vfs_inode for container_of.
 */
struct smoothfs_inode_info {
	struct inode    vfs_inode;
	u8              oid[SMOOTHFS_OID_LEN];
	u32             gen;
	u8              current_tier;
	u8              intended_tier;       /* == current_tier when placed */
	u8              movement_state;
	u8              pin_state;
	atomic_t        nlink_observed;
	u64             transaction_seq;     /* current in-flight movement, 0 if none */
	u32             cutover_gen;         /* bumped on each successful cutover */

	/* The lower path this inode currently maps to. Re-resolved on
	 * cutover. */
	struct path     lower_path;

	/* Heat counters drained periodically (Phase 2 §0.5). */
	atomic_t        open_count;
	atomic64_t      read_bytes;
	atomic64_t      write_bytes;
	u64             last_access_ns;

	/* Last drained snapshot — drain emits the delta. */
	u32             last_drained_open_count;
	u64             last_drained_read_bytes;
	u64             last_drained_write_bytes;
	u64             last_drain_ns;

	/* Writers stall here when movement_state == CUTOVER_IN_PROGRESS;
	 * the drain itself is done via synchronize_srcu on the sb's
	 * cutover_srcu (see smoothfs_sb_info and smoothfs_write_iter). */
	wait_queue_head_t cutover_wq;
	bool            mappings_quiesced;
	char           *rel_path;            /* namespace-relative cached path */

	/* Non-zero when smoothfs_placement_replay holds a pin (the iget ref
	 * was never released so the replayed inode survives in-cache until
	 * the first lookup/open after remount). Cleared via atomic_xchg in
	 * smoothfs_lookup once a real dentry alias takes over the in-cache
	 * role; any remaining pins are released by smoothfs_kill_sb before
	 * generic_shutdown_super runs evict_inodes (otherwise we hit
	 * "Busy inodes after unmount"). atomic_t because concurrent lookups
	 * could race the handoff and we must iput exactly once. */
	atomic_t        replay_pinned;

	struct list_head sb_link;
	struct rhash_head hash_node;          /* oid_map membership */
};

static __always_inline struct smoothfs_inode_info *SMOOTHFS_I(struct inode *inode)
{
	return container_of(inode, struct smoothfs_inode_info, vfs_inode);
}

static __always_inline struct smoothfs_sb_info *SMOOTHFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static __always_inline struct path *smoothfs_lower_path(struct inode *inode)
{
	return &SMOOTHFS_I(inode)->lower_path;
}

static __always_inline struct dentry *smoothfs_lower_dentry(struct dentry *dentry)
{
	return dentry->d_fsdata;
}

/*
 * d_fsdata owns its own reference on the lower dentry; smoothfs_d_release
 * is the matching dput. Callers pass the lower dentry by pointer without
 * a prior dget — the helper adds the reference itself and releases the
 * previous value (if any). Use NULL to clear.
 */
static inline void smoothfs_set_lower_dentry(struct dentry *dentry,
					     struct dentry *lower)
{
	struct dentry *old = dentry->d_fsdata;

	dentry->d_fsdata = lower ? dget(lower) : NULL;
	if (old)
		dput(old);
}

/* module.c */
extern struct file_system_type smoothfs_fs_type;
extern struct kmem_cache       *smoothfs_inode_cachep;
int  smoothfs_sysfs_init(void);
void smoothfs_sysfs_exit(void);

/* super.c */
extern const struct super_operations  smoothfs_super_ops;
extern const struct rhashtable_params smoothfs_oid_rht_params;
extern const struct rhashtable_params smoothfs_lower_ino_rht_params;
int  smoothfs_init_fs_context(struct fs_context *fc);
struct inode *smoothfs_iget(struct super_block *sb, struct path *lower,
			    bool root, bool fresh);
int  smoothfs_oid_map_init(struct smoothfs_sb_info *sbi);
void smoothfs_oid_map_destroy(struct smoothfs_sb_info *sbi);
int  smoothfs_oid_map_insert(struct smoothfs_sb_info *sbi,
			     struct smoothfs_inode_info *si);
void smoothfs_oid_map_remove(struct smoothfs_sb_info *sbi,
			     struct smoothfs_inode_info *si);
int  smoothfs_sysfs_pool_add(struct smoothfs_sb_info *sbi);
void smoothfs_sysfs_pool_remove(struct smoothfs_sb_info *sbi);
void smoothfs_spill_note_success(struct smoothfs_sb_info *sbi,
				 struct inode *inode,
				 u8 source_tier, u8 dest_tier);
void smoothfs_spill_note_failed_all_tiers(struct smoothfs_sb_info *sbi);

/* (tier_idx, lower_ino) -> smoothfs ino_no cache.
 * 8-byte key: (tier_idx << 56) | (lower_ino & 0x00FFFFFFFFFFFFFF).
 */
struct smoothfs_lower_ino_entry {
	struct rhash_head hnode;
	u64               key;
	u64               ino_no;
};

/* One entry in the OID-xattr writeback queue. Holds a dget+mntget'd
 * path to the lower file plus the OID to persist on the next flush. */
struct smoothfs_oid_wb_entry {
	struct list_head link;
	struct path      lower_path;
	u8               oid[SMOOTHFS_OID_LEN];
};

struct smoothfs_placement_wb_entry {
	struct list_head link;
	u8               record[SMOOTHFS_PLACEMENT_REC_SIZE];
};

int  smoothfs_oid_wb_init(struct smoothfs_sb_info *sbi);
void smoothfs_oid_wb_destroy(struct smoothfs_sb_info *sbi);
int  smoothfs_oid_wb_queue(struct smoothfs_sb_info *sbi,
			   struct path *lower_path,
			   const u8 oid[SMOOTHFS_OID_LEN]);
void smoothfs_oid_wb_drain(struct smoothfs_sb_info *sbi);
int  smoothfs_lower_ino_map_init(struct smoothfs_sb_info *sbi);
void smoothfs_lower_ino_map_destroy(struct smoothfs_sb_info *sbi);
u64  smoothfs_lower_ino_map_get(struct smoothfs_sb_info *sbi, u8 tier_idx,
				unsigned long lower_ino);
int  smoothfs_lower_ino_map_insert(struct smoothfs_sb_info *sbi,
				   u8 tier_idx, unsigned long lower_ino,
				   u64 ino_no);
void smoothfs_lower_ino_map_remove(struct smoothfs_sb_info *sbi,
				   u8 tier_idx, unsigned long lower_ino);

/* inode.c */
extern const struct inode_operations  smoothfs_dir_inode_ops;
extern const struct inode_operations  smoothfs_file_inode_ops;
extern const struct inode_operations  smoothfs_symlink_inode_ops;
extern const struct inode_operations  smoothfs_special_inode_ops;
extern const struct dentry_operations smoothfs_dentry_ops;
void smoothfs_copy_attrs(struct inode *dst, struct inode *src);
struct smoothfs_inode_info *smoothfs_lookup_rel_path(struct smoothfs_sb_info *sbi,
						     const char *rel_path);

/* file.c */
extern const struct file_operations          smoothfs_file_ops;
extern const struct address_space_operations smoothfs_aops;

/* dir.c */
extern const struct file_operations smoothfs_dir_ops;

/* xattr.c */
extern const struct xattr_handler * const smoothfs_xattr_handlers[];
ssize_t smoothfs_listxattr(struct dentry *dentry, char *list, size_t size);

/* export.c — NFS export_operations. Phase 4.0 wires stub ops into
 * sb->s_export_op; real encode_fh / fh_to_dentry land in Phase 4.1.
 * Until then nfsd refuses to export the smoothfs sb (FILEID_INVALID
 * from encode_fh, ESTALE from fh_to_dentry). */
extern const struct export_operations smoothfs_export_ops;

/* acl.c — set under #ifdef CONFIG_FS_POSIX_ACL in callers */
struct posix_acl *smoothfs_get_inode_acl(struct inode *inode, int type, bool rcu);
int smoothfs_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		     struct posix_acl *acl, int type);

/* oid.c */
int  smoothfs_alloc_oid(struct smoothfs_sb_info *sbi, u8 oid[SMOOTHFS_OID_LEN]);
int  smoothfs_read_oid_xattr(struct dentry *lower, u8 oid[SMOOTHFS_OID_LEN]);
int  smoothfs_write_oid_xattr(struct dentry *lower,
			      const u8 oid[SMOOTHFS_OID_LEN]);
int  smoothfs_read_gen_xattr(struct dentry *lower, u32 *gen);
int  smoothfs_write_gen_xattr(struct dentry *lower, u32 gen);
u64  smoothfs_inode_no_from_oid(const u8 oid[SMOOTHFS_OID_LEN]);

/* placement.c */
int  smoothfs_placement_open(struct smoothfs_sb_info *sbi);
void smoothfs_placement_close(struct smoothfs_sb_info *sbi);
int  smoothfs_placement_wb_init(struct smoothfs_sb_info *sbi);
void smoothfs_placement_wb_destroy(struct smoothfs_sb_info *sbi);
void smoothfs_placement_wb_drain(struct smoothfs_sb_info *sbi);
void smoothfs_placement_wb_kick(struct smoothfs_sb_info *sbi);
/*
 * Enqueue one placement record for asynchronous writeback. `sync=true`
 * now means "kick the writeback worker immediately", not "block the
 * caller on lower-fs durability". sync_fs / put_super drain the queue.
 * Record loss is recoverable: replay also scans the lower tiers and
 * rebuilds placement from the copy-on-write state.
 */
int  smoothfs_placement_record(struct smoothfs_sb_info *sbi,
			       const u8 oid[SMOOTHFS_OID_LEN],
			       u8 movement_state, u8 current_tier,
			       u8 intended_tier, bool sync);
int  smoothfs_placement_replay(struct super_block *sb,
			       struct smoothfs_sb_info *sbi);

/* netlink.c */
int  smoothfs_netlink_init(void);
void smoothfs_netlink_exit(void);
int  smoothfs_netlink_emit_mount_ready(struct smoothfs_sb_info *sbi);
int  smoothfs_netlink_emit_tier_fault(struct smoothfs_sb_info *sbi, u8 tier);
int  smoothfs_netlink_emit_move_state(struct smoothfs_sb_info *sbi,
				      const u8 oid[SMOOTHFS_OID_LEN],
				      u8 new_state, u64 transaction_seq);
int  smoothfs_netlink_emit_heat_samples(struct smoothfs_sb_info *sbi,
					const void *blob, size_t len,
					unsigned int n_records);
int  smoothfs_netlink_emit_spill(struct smoothfs_sb_info *sbi,
				 const u8 oid[SMOOTHFS_OID_LEN],
				 u8 source_tier, u8 dest_tier,
				 u64 size_bytes);

/* sb registry (sb_id <-> sbi) — netlink command handlers look up the
 * target pool from the request payload. */
int  smoothfs_sb_register(struct smoothfs_sb_info *sbi);
void smoothfs_sb_unregister(struct smoothfs_sb_info *sbi);
struct smoothfs_sb_info *smoothfs_sb_find(const uuid_t *pool_uuid);

/* heat.c */
int  smoothfs_heat_init(struct smoothfs_sb_info *sbi);
void smoothfs_heat_destroy(struct smoothfs_sb_info *sbi);
void smoothfs_heat_kick_drain(struct smoothfs_sb_info *sbi);

/* movement.c — kernel-side state machine. tierd drives transitions;
 * the kernel enforces concurrency and atomicity. */
struct smoothfs_inode_info *smoothfs_lookup_oid(struct smoothfs_sb_info *sbi,
						const u8 oid[SMOOTHFS_OID_LEN]);
int  smoothfs_movement_plan(struct smoothfs_sb_info *sbi,
			    const u8 oid[SMOOTHFS_OID_LEN],
			    u8 dest_tier, u64 transaction_seq, bool force);
int  smoothfs_movement_cutover(struct smoothfs_sb_info *sbi,
			       const u8 oid[SMOOTHFS_OID_LEN],
			       u64 transaction_seq);
int  smoothfs_movement_abort(struct smoothfs_sb_info *sbi,
			     const u8 oid[SMOOTHFS_OID_LEN],
			     u64 transaction_seq, const char *reason);
int  smoothfs_revoke_mappings(struct smoothfs_sb_info *sbi,
			      const u8 oid[SMOOTHFS_OID_LEN]);
void smoothfs_clear_pool_mapping_quiesce(struct smoothfs_sb_info *sbi);

/* lower.c */
struct smoothfs_file_info {
	struct file       *lower_file;
	u32                lower_gen;          /* cutover_gen at lower_file open */
	fmode_t            open_flags;
	const struct cred *open_cred;
	struct mutex       reissue_lock;
	void              *dir_cache;          /* dir.c-owned merged readdir state */
};

int  smoothfs_open_lower(struct file *file, struct inode *inode);
int  smoothfs_release_lower(struct file *file);
struct file *smoothfs_lower_file(struct file *file);

/* probe.c (capabilities) */
int  smoothfs_probe_capabilities(struct smoothfs_tier *tier);
bool smoothfs_lower_has_revalidate(const struct smoothfs_tier *tier);

#endif /* _SMOOTHFS_H */

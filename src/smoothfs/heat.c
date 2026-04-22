// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - heat sample drain (Phase 2 §0.5).
 *
 * Per-pool delayed_work runs every SMOOTHFS_HEAT_DRAIN_MS, walks the
 * sb's inode list, computes (current - last_drained) deltas, and
 * emits up to 256 heat_sample_records per netlink multicast message.
 * Inodes with no activity since their last drain are skipped.
 */

#include <linux/fs.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#include "smoothfs.h"

#define SMOOTHFS_HEAT_BATCH_MAX  256

static void smoothfs_heat_drain_work_fn(struct work_struct *work);

int smoothfs_heat_init(struct smoothfs_sb_info *sbi)
{
	INIT_DELAYED_WORK(&sbi->heat_drain_work, smoothfs_heat_drain_work_fn);
	sbi->heat_drain_active = true;
	queue_delayed_work(system_wq, &sbi->heat_drain_work,
			   msecs_to_jiffies(SMOOTHFS_HEAT_DRAIN_MS));
	return 0;
}

void smoothfs_heat_destroy(struct smoothfs_sb_info *sbi)
{
	sbi->heat_drain_active = false;
	cancel_delayed_work_sync(&sbi->heat_drain_work);
}

void smoothfs_heat_kick_drain(struct smoothfs_sb_info *sbi)
{
	if (sbi->heat_drain_active)
		mod_delayed_work(system_wq, &sbi->heat_drain_work, 0);
}

static int smoothfs_heat_collect(struct smoothfs_sb_info *sbi,
				 struct smoothfs_heat_sample_record *batch,
				 int max)
{
	struct smoothfs_inode_info *si;
	u64 now = ktime_get_real_ns();
	int n = 0;

	down_read(&sbi->inode_lock);
	list_for_each_entry(si, &sbi->inode_list, sb_link) {
		u32 cur_open;
		u64 cur_read, cur_write;
		struct smoothfs_heat_sample_record *r;

		if (n >= max)
			break;

		cur_open  = (u32)atomic_read(&si->open_count);
		cur_read  = atomic64_read(&si->read_bytes);
		cur_write = atomic64_read(&si->write_bytes);

		if (cur_open == si->last_drained_open_count &&
		    cur_read == si->last_drained_read_bytes &&
		    cur_write == si->last_drained_write_bytes)
			continue;

		r = &batch[n++];
		memcpy(r->oid, si->oid, SMOOTHFS_OID_LEN);
		r->open_count_delta  = cur_open  - si->last_drained_open_count;
		r->reserved          = 0;
		r->read_bytes_delta  = cur_read  - si->last_drained_read_bytes;
		r->write_bytes_delta = cur_write - si->last_drained_write_bytes;
		r->last_access_ns    = si->last_access_ns;
		r->sample_window_ns  = now - si->last_drain_ns;

		si->last_drained_open_count  = cur_open;
		si->last_drained_read_bytes  = cur_read;
		si->last_drained_write_bytes = cur_write;
		si->last_drain_ns            = now;
	}
	up_read(&sbi->inode_lock);
	return n;
}

static void smoothfs_heat_drain_work_fn(struct work_struct *work)
{
	struct delayed_work *dw = to_delayed_work(work);
	struct smoothfs_sb_info *sbi = container_of(dw, struct smoothfs_sb_info,
						    heat_drain_work);
	struct smoothfs_heat_sample_record *batch;
	int n;

	if (!sbi->heat_drain_active)
		return;

	batch = kmalloc_array(SMOOTHFS_HEAT_BATCH_MAX, sizeof(*batch),
			      GFP_KERNEL);
	if (!batch)
		goto resched;

	n = smoothfs_heat_collect(sbi, batch, SMOOTHFS_HEAT_BATCH_MAX);
	if (n > 0)
		smoothfs_netlink_emit_heat_samples(sbi, batch,
						   n * sizeof(*batch), n);
	kfree(batch);

resched:
	if (sbi->heat_drain_active)
		queue_delayed_work(system_wq, &sbi->heat_drain_work,
				   msecs_to_jiffies(SMOOTHFS_HEAT_DRAIN_MS));
}

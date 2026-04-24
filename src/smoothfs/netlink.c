// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - generic netlink control channel.
 *
 * Phase 2: real multicast emit (group "events"), real handlers for
 * MOVE_PLAN / MOVE_CUTOVER / RECONCILE / INSPECT / QUIESCE /
 * TIER_DOWN, plus sb registry so handlers can find the target pool
 * by UUID. REGISTER_POOL and POLICY_PUSH still ack ENOSYS — Phase 2
 * doesn't need them yet (mount syntax carries tier paths).
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netlink.h>
#include <linux/uuid.h>
#include <net/genetlink.h>

#include "smoothfs.h"

static struct genl_family smoothfs_genl_family;

/* ---------------- sb registry ---------------- */

static LIST_HEAD(smoothfs_sb_list);
static DEFINE_MUTEX(smoothfs_sb_lock);

struct smoothfs_sb_entry {
	struct list_head        link;
	struct smoothfs_sb_info *sbi;
};

int smoothfs_sb_register(struct smoothfs_sb_info *sbi)
{
	struct smoothfs_sb_entry *e = kzalloc(sizeof(*e), GFP_KERNEL);

	if (!e)
		return -ENOMEM;
	e->sbi = sbi;
	mutex_lock(&smoothfs_sb_lock);
	list_add(&e->link, &smoothfs_sb_list);
	mutex_unlock(&smoothfs_sb_lock);
	return 0;
}

void smoothfs_sb_unregister(struct smoothfs_sb_info *sbi)
{
	struct smoothfs_sb_entry *e, *tmp;

	mutex_lock(&smoothfs_sb_lock);
	list_for_each_entry_safe(e, tmp, &smoothfs_sb_list, link) {
		if (e->sbi == sbi) {
			list_del(&e->link);
			kfree(e);
			break;
		}
	}
	mutex_unlock(&smoothfs_sb_lock);
}

struct smoothfs_sb_info *smoothfs_sb_find(const uuid_t *pool_uuid)
{
	struct smoothfs_sb_entry *e;
	struct smoothfs_sb_info *out = NULL;

	mutex_lock(&smoothfs_sb_lock);
	list_for_each_entry(e, &smoothfs_sb_list, link) {
		if (uuid_equal(&e->sbi->pool_uuid, pool_uuid)) {
			out = e->sbi;
			break;
		}
	}
	mutex_unlock(&smoothfs_sb_lock);
	return out;
}

/* ---------------- attr policy ---------------- */

static const struct nla_policy smoothfs_genl_policy[SMOOTHFS_ATTR_MAX + 1] = {
	[SMOOTHFS_ATTR_POOL_UUID]        = { .type = NLA_BINARY, .len = 16 },
	[SMOOTHFS_ATTR_POOL_NAME]        = { .type = NLA_NUL_STRING, .len = 63 },
	[SMOOTHFS_ATTR_FSID]             = { .type = NLA_U32 },
	[SMOOTHFS_ATTR_TIER_RANK]        = { .type = NLA_U8 },
	[SMOOTHFS_ATTR_TIER_CAPS]        = { .type = NLA_U32 },
	[SMOOTHFS_ATTR_TIER_PATH]        = { .type = NLA_NUL_STRING, .len = PATH_MAX - 1 },
	[SMOOTHFS_ATTR_TIER_ID]          = { .type = NLA_NUL_STRING, .len = 64 },
	[SMOOTHFS_ATTR_OBJECT_ID]        = { .type = NLA_BINARY, .len = SMOOTHFS_OID_LEN },
	[SMOOTHFS_ATTR_GENERATION]       = { .type = NLA_U32 },
	[SMOOTHFS_ATTR_MOVEMENT_STATE]   = { .type = NLA_U8 },
	[SMOOTHFS_ATTR_CURRENT_TIER]     = { .type = NLA_U8 },
	[SMOOTHFS_ATTR_INTENDED_TIER]    = { .type = NLA_U8 },
	[SMOOTHFS_ATTR_TRANSACTION_SEQ]  = { .type = NLA_U64 },
	[SMOOTHFS_ATTR_PIN_STATE]        = { .type = NLA_U8 },
	[SMOOTHFS_ATTR_HEAT_SAMPLE_BLOB] = { .type = NLA_BINARY },
	[SMOOTHFS_ATTR_CHECKPOINT_SEQ]   = { .type = NLA_U64 },
	[SMOOTHFS_ATTR_RECONCILE_REASON] = { .type = NLA_NUL_STRING, .len = 255 },
	[SMOOTHFS_ATTR_TIERS]            = { .type = NLA_NESTED },
	[SMOOTHFS_ATTR_REL_PATH]         = { .type = NLA_NUL_STRING, .len = PATH_MAX - 1 },
	[SMOOTHFS_ATTR_FORCE]            = { .type = NLA_U8 },
	[SMOOTHFS_ATTR_SIZE_BYTES]       = { .type = NLA_U64 },
	[SMOOTHFS_ATTR_ANY_SPILL_SINCE_MOUNT] = { .type = NLA_U8 },
};

static char *smoothfs_path_string(const struct path *path)
{
	char *buf, *rendered, *out;

	buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buf)
		return NULL;
	rendered = d_path(path, buf, PATH_MAX);
	if (IS_ERR(rendered)) {
		kfree(buf);
		return NULL;
	}
	out = kstrdup(rendered, GFP_KERNEL);
	kfree(buf);
	return out;
}

/* dentry_path_raw expects a working buffer; helper computes the
 * pool-root-relative path for the inode's first alias and returns
 * a kmalloc'd string the caller frees. NULL on error. */
static char *smoothfs_inode_rel_path(struct inode *inode)
{
	struct dentry *d;
	char *buf, *path, *out;

	d = d_find_alias(inode);
	if (!d) {
		struct smoothfs_inode_info *si = SMOOTHFS_I(inode);

		return si->rel_path ? kstrdup(si->rel_path, GFP_KERNEL) : NULL;
	}
	buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buf) {
		dput(d);
		return NULL;
	}
	path = dentry_path_raw(d, buf, PATH_MAX);
	if (IS_ERR(path)) {
		kfree(buf);
		dput(d);
		return NULL;
	}
	/* Strip the leading "/" so the worker can join with the lower
	 * dir directly. The pool root itself becomes the empty string. */
	if (*path == '/')
		path++;
	out = kstrdup(path, GFP_KERNEL);
	kfree(buf);
	dput(d);
	return out;
}

/* ---------------- helpers ---------------- */

static struct smoothfs_sb_info *resolve_pool(struct genl_info *info)
{
	uuid_t uuid;

	if (!info->attrs[SMOOTHFS_ATTR_POOL_UUID])
		return ERR_PTR(-EINVAL);
	if (nla_len(info->attrs[SMOOTHFS_ATTR_POOL_UUID]) != sizeof(uuid))
		return ERR_PTR(-EINVAL);
	memcpy(uuid.b, nla_data(info->attrs[SMOOTHFS_ATTR_POOL_UUID]),
	       sizeof(uuid));
	{
		struct smoothfs_sb_info *sbi = smoothfs_sb_find(&uuid);

		return sbi ? sbi : ERR_PTR(-ENODEV);
	}
}

/* ---------------- inbound handlers ---------------- */

static int doit_register_pool(struct sk_buff *skb, struct genl_info *info)
{
	(void)skb;
	(void)info;
	/* Mounts carry tier paths in mount options; tierd-side
	 * tier_target IDs are tracked in SQLite and applied at planning
	 * time. We accept REGISTER_POOL as a no-op. */
	return 0;
}

static int doit_move_plan(struct sk_buff *skb, struct genl_info *info)
{
	struct smoothfs_sb_info *sbi;
	const u8 *oid;
	u8 dest_tier;
	u64 seq;
	bool force = false;
	int err;

	(void)skb;
	sbi = resolve_pool(info);
	if (IS_ERR(sbi))
		return PTR_ERR(sbi);
	if (!info->attrs[SMOOTHFS_ATTR_OBJECT_ID] ||
	    !info->attrs[SMOOTHFS_ATTR_INTENDED_TIER] ||
	    !info->attrs[SMOOTHFS_ATTR_TRANSACTION_SEQ])
		return -EINVAL;

	oid       = nla_data(info->attrs[SMOOTHFS_ATTR_OBJECT_ID]);
	dest_tier = nla_get_u8(info->attrs[SMOOTHFS_ATTR_INTENDED_TIER]);
	seq       = nla_get_u64(info->attrs[SMOOTHFS_ATTR_TRANSACTION_SEQ]);
	if (info->attrs[SMOOTHFS_ATTR_FORCE])
		force = nla_get_u8(info->attrs[SMOOTHFS_ATTR_FORCE]) != 0;

	return smoothfs_movement_plan(sbi, oid, dest_tier, seq, force);
}

static int doit_move_cutover(struct sk_buff *skb, struct genl_info *info)
{
	struct smoothfs_sb_info *sbi;
	const u8 *oid;
	u64 seq;
	int err;

	(void)skb;
	sbi = resolve_pool(info);
	if (IS_ERR(sbi))
		return PTR_ERR(sbi);
	if (!info->attrs[SMOOTHFS_ATTR_OBJECT_ID] ||
	    !info->attrs[SMOOTHFS_ATTR_TRANSACTION_SEQ])
		return -EINVAL;

	oid = nla_data(info->attrs[SMOOTHFS_ATTR_OBJECT_ID]);
	seq = nla_get_u64(info->attrs[SMOOTHFS_ATTR_TRANSACTION_SEQ]);

	return smoothfs_movement_cutover(sbi, oid, seq);
}

static int doit_inspect(struct sk_buff *skb, struct genl_info *info)
{
	struct smoothfs_sb_info *sbi;
	struct smoothfs_inode_info *si;
	const u8 *oid;
	struct sk_buff *rsp;
	void *hdr;

	(void)skb;
	sbi = resolve_pool(info);
	if (IS_ERR(sbi))
		return PTR_ERR(sbi);
	if (!info->attrs[SMOOTHFS_ATTR_OBJECT_ID])
		return -EINVAL;

	oid = nla_data(info->attrs[SMOOTHFS_ATTR_OBJECT_ID]);
	si = smoothfs_lookup_oid(sbi, oid);
	if (!si)
		return -ENOENT;

	rsp = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!rsp)
		return -ENOMEM;
	hdr = genlmsg_put_reply(rsp, info, &smoothfs_genl_family, 0,
				SMOOTHFS_CMD_INSPECT);
	if (!hdr) {
		nlmsg_free(rsp);
		return -EMSGSIZE;
	}
	if (nla_put(rsp, SMOOTHFS_ATTR_OBJECT_ID, SMOOTHFS_OID_LEN, si->oid) ||
	    nla_put_u8(rsp, SMOOTHFS_ATTR_CURRENT_TIER, si->current_tier) ||
	    nla_put_u8(rsp, SMOOTHFS_ATTR_INTENDED_TIER, si->intended_tier) ||
	    nla_put_u8(rsp, SMOOTHFS_ATTR_MOVEMENT_STATE, si->movement_state) ||
	    nla_put_u8(rsp, SMOOTHFS_ATTR_PIN_STATE, si->pin_state) ||
	    nla_put_u32(rsp, SMOOTHFS_ATTR_GENERATION, si->cutover_gen) ||
	    nla_put_u64_64bit(rsp, SMOOTHFS_ATTR_TRANSACTION_SEQ,
			      si->transaction_seq, 0)) {
		genlmsg_cancel(rsp, hdr);
		nlmsg_free(rsp);
		return -EMSGSIZE;
	}
	{
		char *rel = smoothfs_inode_rel_path(&si->vfs_inode);
		int err2 = 0;

		if (rel) {
			err2 = nla_put_string(rsp, SMOOTHFS_ATTR_REL_PATH, rel);
			kfree(rel);
		}
		if (err2) {
			genlmsg_cancel(rsp, hdr);
			nlmsg_free(rsp);
			return -EMSGSIZE;
		}
	}
	{
		char *tier_path = smoothfs_path_string(&sbi->tiers[si->current_tier].lower_path);
		int err2 = 0;

		if (tier_path) {
			err2 = nla_put_string(rsp, SMOOTHFS_ATTR_TIER_PATH, tier_path);
			kfree(tier_path);
		}
		if (err2) {
			genlmsg_cancel(rsp, hdr);
			nlmsg_free(rsp);
			return -EMSGSIZE;
		}
	}
	genlmsg_end(rsp, hdr);
	return genlmsg_reply(rsp, info);
}

static int doit_quiesce(struct sk_buff *skb, struct genl_info *info)
{
	struct smoothfs_sb_info *sbi;

	(void)skb;
	sbi = resolve_pool(info);
	if (IS_ERR(sbi))
		return PTR_ERR(sbi);
	WRITE_ONCE(sbi->quiesced, true);
	return 0;
}

static int doit_reconcile(struct sk_buff *skb, struct genl_info *info)
{
	struct smoothfs_sb_info *sbi;

	(void)skb;
	sbi = resolve_pool(info);
	if (IS_ERR(sbi))
		return PTR_ERR(sbi);
	WRITE_ONCE(sbi->quiesced, false);
	smoothfs_clear_pool_mapping_quiesce(sbi);
	smoothfs_heat_kick_drain(sbi);
	return 0;
}

static int doit_tier_down(struct sk_buff *skb, struct genl_info *info)
{
	(void)skb;
	(void)info;
	/* Mark a tier as quarantined; needs lower-fs unmount handling. */
	return 0;
}

static int doit_revoke_mappings(struct sk_buff *skb, struct genl_info *info)
{
	struct smoothfs_sb_info *sbi;
	const u8 *oid;

	(void)skb;
	sbi = resolve_pool(info);
	if (IS_ERR(sbi))
		return PTR_ERR(sbi);
	if (!info->attrs[SMOOTHFS_ATTR_OBJECT_ID])
		return -EINVAL;
	oid = nla_data(info->attrs[SMOOTHFS_ATTR_OBJECT_ID]);
	return smoothfs_revoke_mappings(sbi, oid);
}

static int doit_unimpl(struct sk_buff *skb, struct genl_info *info)
{
	(void)skb;
	pr_debug("smoothfs: netlink cmd %u not implemented in Phase 2\n",
		 info->genlhdr->cmd);
	return -ENOSYS;
}

/* ---------------- family + multicast ---------------- */

static const struct genl_multicast_group smoothfs_genl_mcgrps[] = {
	{ .name = "events" },
};

static const struct genl_small_ops smoothfs_genl_ops[] = {
	{ .cmd = SMOOTHFS_CMD_REGISTER_POOL, .flags = GENL_ADMIN_PERM,
	  .doit = doit_register_pool },
	{ .cmd = SMOOTHFS_CMD_POLICY_PUSH,   .flags = GENL_ADMIN_PERM,
	  .doit = doit_unimpl },
	{ .cmd = SMOOTHFS_CMD_MOVE_PLAN,     .flags = GENL_ADMIN_PERM,
	  .doit = doit_move_plan },
	{ .cmd = SMOOTHFS_CMD_TIER_DOWN,     .flags = GENL_ADMIN_PERM,
	  .doit = doit_tier_down },
	{ .cmd = SMOOTHFS_CMD_RECONCILE,     .flags = GENL_ADMIN_PERM,
	  .doit = doit_reconcile },
	{ .cmd = SMOOTHFS_CMD_QUIESCE,       .flags = GENL_ADMIN_PERM,
	  .doit = doit_quiesce },
	{ .cmd = SMOOTHFS_CMD_INSPECT,       .doit = doit_inspect },
	{ .cmd = SMOOTHFS_CMD_REPROBE,       .flags = GENL_ADMIN_PERM,
	  .doit = doit_unimpl },
	{ .cmd = SMOOTHFS_CMD_MOVE_CUTOVER,  .flags = GENL_ADMIN_PERM,
	  .doit = doit_move_cutover },
	{ .cmd = SMOOTHFS_CMD_REVOKE_MAPPINGS, .flags = GENL_ADMIN_PERM,
	  .doit = doit_revoke_mappings },
};

static struct genl_family smoothfs_genl_family __ro_after_init = {
	.name        = SMOOTHFS_GENL_NAME,
	.version     = SMOOTHFS_GENL_VERSION,
	.maxattr     = SMOOTHFS_ATTR_MAX,
	.policy      = smoothfs_genl_policy,
	.netnsok     = false,
	.module      = THIS_MODULE,
	.small_ops   = smoothfs_genl_ops,
	.n_small_ops = ARRAY_SIZE(smoothfs_genl_ops),
	.mcgrps      = smoothfs_genl_mcgrps,
	.n_mcgrps    = ARRAY_SIZE(smoothfs_genl_mcgrps),
};

int smoothfs_netlink_init(void)
{
	return genl_register_family(&smoothfs_genl_family);
}

void smoothfs_netlink_exit(void)
{
	genl_unregister_family(&smoothfs_genl_family);
}

/* ---------------- multicast emit ---------------- */

static int multicast_send(struct sk_buff *skb)
{
	int err = genlmsg_multicast(&smoothfs_genl_family, skb, 0, 0,
				    GFP_KERNEL);
	/* ESRCH = no listeners; perfectly normal. Other errors are
	 * fatal-looking; log for debug. */
	if (err == -ESRCH)
		err = 0;
	return err;
}

static int emit_pool_event(struct smoothfs_sb_info *sbi, u8 cmd,
			   u8 fault_tier, bool include_fault_tier)
{
	struct sk_buff *skb;
	void *hdr;
	struct nlattr *tiers = NULL;
	u8 i;

	skb = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	hdr = genlmsg_put(skb, 0, 0, &smoothfs_genl_family, 0, cmd);
	if (!hdr)
		goto err;
	if (nla_put(skb, SMOOTHFS_ATTR_POOL_UUID, sizeof(sbi->pool_uuid.b),
		    sbi->pool_uuid.b) ||
	    nla_put_string(skb, SMOOTHFS_ATTR_POOL_NAME, sbi->pool_name) ||
	    nla_put_u32(skb, SMOOTHFS_ATTR_FSID, sbi->fsid) ||
	    nla_put_u8(skb, SMOOTHFS_ATTR_ANY_SPILL_SINCE_MOUNT,
		       atomic_read(&sbi->any_spill_since_mount) ? 1 : 0))
		goto cancel;
	if (cmd == SMOOTHFS_EVENT_MOUNT_READY) {
		tiers = nla_nest_start(skb, SMOOTHFS_ATTR_TIERS);
		if (!tiers)
			goto cancel;
		for (i = 0; i < sbi->ntiers; i++) {
			struct nlattr *entry;
			char *tier_path;

			entry = nla_nest_start(skb, i + 1);
			if (!entry)
				goto cancel;
			tier_path = smoothfs_path_string(&sbi->tiers[i].lower_path);
			if (!tier_path ||
			    nla_put_u8(skb, SMOOTHFS_ATTR_TIER_RANK, sbi->tiers[i].rank) ||
			    nla_put_u32(skb, SMOOTHFS_ATTR_TIER_CAPS, sbi->tiers[i].caps) ||
			    nla_put_string(skb, SMOOTHFS_ATTR_TIER_PATH, tier_path)) {
				kfree(tier_path);
				goto cancel;
			}
			kfree(tier_path);
			nla_nest_end(skb, entry);
		}
		nla_nest_end(skb, tiers);
	}
	if (include_fault_tier &&
	    nla_put_u8(skb, SMOOTHFS_ATTR_TIER_RANK, fault_tier))
		goto cancel;
	genlmsg_end(skb, hdr);
	return multicast_send(skb);

cancel:
	genlmsg_cancel(skb, hdr);
err:
	nlmsg_free(skb);
	return -EMSGSIZE;
}

int smoothfs_netlink_emit_mount_ready(struct smoothfs_sb_info *sbi)
{
	return emit_pool_event(sbi, SMOOTHFS_EVENT_MOUNT_READY, 0, false);
}

int smoothfs_netlink_emit_tier_fault(struct smoothfs_sb_info *sbi, u8 tier)
{
	return emit_pool_event(sbi, SMOOTHFS_EVENT_TIER_FAULT, tier, true);
}

int smoothfs_netlink_emit_move_state(struct smoothfs_sb_info *sbi,
				     const u8 oid[SMOOTHFS_OID_LEN],
				     u8 new_state, u64 transaction_seq)
{
	struct sk_buff *skb;
	void *hdr;

	skb = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	hdr = genlmsg_put(skb, 0, 0, &smoothfs_genl_family, 0,
			  SMOOTHFS_EVENT_MOVE_STATE);
	if (!hdr)
		goto err;
	if (nla_put(skb, SMOOTHFS_ATTR_POOL_UUID, sizeof(sbi->pool_uuid.b),
		    sbi->pool_uuid.b) ||
	    nla_put(skb, SMOOTHFS_ATTR_OBJECT_ID, SMOOTHFS_OID_LEN, oid) ||
	    nla_put_u8(skb, SMOOTHFS_ATTR_MOVEMENT_STATE, new_state) ||
	    nla_put_u64_64bit(skb, SMOOTHFS_ATTR_TRANSACTION_SEQ,
			      transaction_seq, 0))
		goto cancel;
	genlmsg_end(skb, hdr);
	return multicast_send(skb);

cancel:
	genlmsg_cancel(skb, hdr);
err:
	nlmsg_free(skb);
	return -EMSGSIZE;
}

int smoothfs_netlink_emit_heat_samples(struct smoothfs_sb_info *sbi,
				       const void *blob, size_t len,
				       unsigned int n_records)
{
	struct sk_buff *skb;
	void *hdr;
	size_t need;

	(void)n_records;
	if (!len)
		return 0;

	need = NLMSG_GOODSIZE + len;
	if (need > 64 * 1024)
		need = 64 * 1024;
	skb = genlmsg_new(need, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	hdr = genlmsg_put(skb, 0, 0, &smoothfs_genl_family, 0,
			  SMOOTHFS_EVENT_HEAT_SAMPLE);
	if (!hdr)
		goto err;
	if (nla_put(skb, SMOOTHFS_ATTR_POOL_UUID, sizeof(sbi->pool_uuid.b),
		    sbi->pool_uuid.b) ||
	    nla_put(skb, SMOOTHFS_ATTR_HEAT_SAMPLE_BLOB, len, blob))
		goto cancel;
	genlmsg_end(skb, hdr);
	return multicast_send(skb);

cancel:
	genlmsg_cancel(skb, hdr);
err:
	nlmsg_free(skb);
	return -EMSGSIZE;
}

int smoothfs_netlink_emit_spill(struct smoothfs_sb_info *sbi,
				const u8 oid[SMOOTHFS_OID_LEN],
				u8 source_tier, u8 dest_tier,
				u64 size_bytes)
{
	struct sk_buff *skb;
	void *hdr;

	skb = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	hdr = genlmsg_put(skb, 0, 0, &smoothfs_genl_family, 0,
			  SMOOTHFS_EVENT_SPILL);
	if (!hdr)
		goto err;
	if (nla_put(skb, SMOOTHFS_ATTR_POOL_UUID, sizeof(sbi->pool_uuid.b),
		    sbi->pool_uuid.b) ||
	    nla_put_u8(skb, SMOOTHFS_ATTR_ANY_SPILL_SINCE_MOUNT, 1) ||
	    nla_put(skb, SMOOTHFS_ATTR_OBJECT_ID, SMOOTHFS_OID_LEN, oid) ||
	    nla_put_u8(skb, SMOOTHFS_ATTR_CURRENT_TIER, source_tier) ||
	    nla_put_u8(skb, SMOOTHFS_ATTR_INTENDED_TIER, dest_tier) ||
	    nla_put_u64_64bit(skb, SMOOTHFS_ATTR_SIZE_BYTES, size_bytes, 0))
		goto cancel;
	genlmsg_end(skb, hdr);
	return multicast_send(skb);

cancel:
	genlmsg_cancel(skb, hdr);
err:
	nlmsg_free(skb);
	return -EMSGSIZE;
}

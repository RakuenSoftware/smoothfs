/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * smoothfs uapi - shared types between kernel module and userspace tierd.
 *
 * Per smoothfs-stacked-tiering.md §Control Plane and
 * smoothfs-phase-0-contract.md §0.5 (heat sampling) / §0.7 (protocol).
 */

#ifndef _UAPI_LINUX_SMOOTHFS_H
#define _UAPI_LINUX_SMOOTHFS_H

#include <linux/types.h>

#define SMOOTHFS_GENL_NAME    "smoothfs"
#define SMOOTHFS_GENL_VERSION 1

/*
 * On-wire NFS file-handle types, both outside Linux's standard
 * `enum fid_type` range (1–6, plus FILEID_INVALID = 0xff).
 *
 * FILEID_SMOOTHFS_OID ('S', 24 bytes, Phase 0 §0.7):
 *   fsid (4) | object_id (16) | gen (4)
 *
 * FILEID_SMOOTHFS_OID_CONNECTABLE ('T', 40 bytes, Phase 4.5):
 *   fsid (4) | object_id (16) | gen (4) | parent_object_id (16)
 *
 *   The connectable variant is emitted whenever nfsd asks for a
 *   parent-bearing handle (LOOKUPP-class ops, silly-rename cleanup).
 *   File resolution uses only the object_id — parent_object_id is
 *   read by fh_to_parent.
 */
#define FILEID_SMOOTHFS_OID               0x53
#define FILEID_SMOOTHFS_OID_CONNECTABLE   0x54

/* Generic netlink commands. Mirrors §Control Plane in the parent
 * proposal. Phase 1 implements REGISTER_POOL, MOUNT_READY, RECONCILE,
 * TIER_FAULT, INSPECT. The remainder are reserved enum values so the
 * userspace API doesn't shift when later phases land them. */
enum {
	SMOOTHFS_CMD_UNSPEC               = 0,
	SMOOTHFS_CMD_REGISTER_POOL        = 1,
	SMOOTHFS_CMD_POLICY_PUSH          = 2,
	SMOOTHFS_CMD_MOVE_PLAN            = 3,
	SMOOTHFS_CMD_TIER_DOWN            = 4,
	SMOOTHFS_CMD_RECONCILE            = 5,
	SMOOTHFS_CMD_QUIESCE              = 6,
	SMOOTHFS_CMD_INSPECT              = 7,
	SMOOTHFS_CMD_REPROBE              = 8,
	SMOOTHFS_EVENT_MOUNT_READY        = 9,
	SMOOTHFS_EVENT_HEAT_SAMPLE        = 10,
	SMOOTHFS_EVENT_MOVE_STATE         = 11,
	SMOOTHFS_EVENT_TIER_FAULT         = 12,
	SMOOTHFS_CMD_MOVE_CUTOVER         = 13,
	SMOOTHFS_CMD_REVOKE_MAPPINGS      = 14,
	SMOOTHFS_EVENT_SPILL              = 15,
	__SMOOTHFS_CMD_MAX,
};
#define SMOOTHFS_CMD_MAX (__SMOOTHFS_CMD_MAX - 1)

/* Generic netlink attributes. */
enum {
	SMOOTHFS_ATTR_UNSPEC              = 0,
	SMOOTHFS_ATTR_POOL_UUID           = 1,  /* binary, 16 bytes */
	SMOOTHFS_ATTR_POOL_NAME           = 2,  /* nul-terminated string */
	SMOOTHFS_ATTR_FSID                = 3,  /* u32 */
	SMOOTHFS_ATTR_TIER_RANK           = 4,  /* u8 */
	SMOOTHFS_ATTR_TIER_CAPS           = 5,  /* u32 bitmap */
	SMOOTHFS_ATTR_TIER_PATH           = 6,  /* string */
	SMOOTHFS_ATTR_TIER_ID             = 7,  /* string (tier_targets.id) */
	SMOOTHFS_ATTR_OBJECT_ID           = 8,  /* binary, 16 bytes */
	SMOOTHFS_ATTR_GENERATION          = 9,  /* u32 */
	SMOOTHFS_ATTR_MOVEMENT_STATE      = 10, /* u8 enum */
	SMOOTHFS_ATTR_CURRENT_TIER        = 11, /* u8 */
	SMOOTHFS_ATTR_INTENDED_TIER       = 12, /* u8 */
	SMOOTHFS_ATTR_TRANSACTION_SEQ     = 13, /* u64 */
	SMOOTHFS_ATTR_PIN_STATE           = 14, /* u8 enum */
	SMOOTHFS_ATTR_HEAT_SAMPLE_BLOB    = 15, /* binary, packed records */
	SMOOTHFS_ATTR_CHECKPOINT_SEQ      = 16, /* u64 */
	SMOOTHFS_ATTR_RECONCILE_REASON    = 17, /* string */
	SMOOTHFS_ATTR_TIERS               = 18, /* nested */
	SMOOTHFS_ATTR_REL_PATH            = 19, /* string, namespace-relative */
	SMOOTHFS_ATTR_FORCE               = 20, /* u8; 1 = bypass pin_state on MOVE_PLAN — Phase 5.3 */
	SMOOTHFS_ATTR_SIZE_BYTES          = 21, /* u64 */
	SMOOTHFS_ATTR_ANY_SPILL_SINCE_MOUNT = 22, /* u8 boolean */
	SMOOTHFS_ATTR_WRITE_SEQ           = 23, /* u64; data-change sequence for movement cutover */
	SMOOTHFS_ATTR_RANGE_STAGED        = 24, /* u8 boolean; object has staged range overlay */
	__SMOOTHFS_ATTR_MAX,
};
#define SMOOTHFS_ATTR_MAX (__SMOOTHFS_ATTR_MAX - 1)

/* On-wire heat sample record, packed into SMOOTHFS_ATTR_HEAT_SAMPLE_BLOB.
 * 56 bytes per record per Phase 0 §0.5. Repeated up to 256 per message. */
struct smoothfs_heat_sample_record {
	__u8  oid[16];
	__u32 open_count_delta;
	__u32 reserved;             /* alignment */
	__u64 read_bytes_delta;
	__u64 write_bytes_delta;
	__u64 last_access_ns;
	__u64 sample_window_ns;
};

#endif /* _UAPI_LINUX_SMOOTHFS_H */

// SPDX-License-Identifier: GPL-2.0-only
/*
 * smoothfs - object_id allocation and xattr-backed identity.
 *
 * Object IDs are 128-bit UUIDv7 (RFC 9562 §5.7) per Phase 0 §0.1:
 *   - 48 bits: Unix milliseconds (big-endian)
 *   - 4  bits: version (0b0111)
 *   - 12 bits: sub-millisecond monotonic counter (big-endian)
 *   - 2  bits: variant (0b10)
 *   - 62 bits: cryptographic random
 *
 * The exposed inode number is xxhash64(oid) | (1 << 63) per §0.1.
 */

#include <linux/fs.h>
#include <linux/random.h>
#include <linux/timekeeping.h>
#include <linux/xattr.h>
#include <linux/dcache.h>
#include <linux/xxhash.h>
#include <linux/atomic.h>

#include "smoothfs.h"

int smoothfs_alloc_oid(struct smoothfs_sb_info *sbi, u8 oid[SMOOTHFS_OID_LEN])
{
	u64 ms;
	u16 ctr;
	u64 rand;

	ms  = ktime_get_real_ns() / NSEC_PER_MSEC;
	ctr = (u16)(atomic64_inc_return(&sbi->oid_monotonic) & 0x0FFF);

	/* 48-bit unix_ms */
	oid[0] = (ms >> 40) & 0xff;
	oid[1] = (ms >> 32) & 0xff;
	oid[2] = (ms >> 24) & 0xff;
	oid[3] = (ms >> 16) & 0xff;
	oid[4] = (ms >>  8) & 0xff;
	oid[5] = (ms >>  0) & 0xff;
	/* 4-bit version (0x7) | 12-bit counter */
	oid[6] = 0x70 | ((ctr >> 8) & 0x0f);
	oid[7] = ctr & 0xff;

	/*
	 * 64 bits of random tail, variant bits forced to 0b10. get_random_u64
	 * reads from a per-CPU ChaCha20 stream in ~5 ns — noticeably cheaper
	 * than get_random_bytes(buf, 8) which walks the generic CSPRNG path
	 * and contends on a shared lock under bursty CREATE.
	 */
	rand = get_random_u64();
	rand = (rand & ~(0xC0ULL << 56)) | (0x80ULL << 56);
	memcpy(&oid[8], &rand, 8);

	return 0;
}

/*
 * The trusted.smoothfs.* xattrs are smoothfs-internal metadata, never
 * surfaced to userspace through the smoothfs namespace. Use the
 * __vfs_*xattr variants to skip security_inode_{get,set}xattr on this
 * hot path — the security hook would only check userspace-visible
 * xattrs anyway, and on CREATE this shaves the LSM layer off every
 * new-file code path.
 */
int smoothfs_read_oid_xattr(struct dentry *lower, u8 oid[SMOOTHFS_OID_LEN])
{
	int ret;

	ret = __vfs_getxattr(lower, d_inode(lower), SMOOTHFS_OID_XATTR,
			     oid, SMOOTHFS_OID_LEN);
	if (ret < 0)
		return ret;
	if (ret != SMOOTHFS_OID_LEN)
		return -EIO;
	return 0;
}

int smoothfs_write_oid_xattr(struct dentry *lower,
			     const u8 oid[SMOOTHFS_OID_LEN])
{
	return __vfs_setxattr(&nop_mnt_idmap, lower, d_inode(lower),
			      SMOOTHFS_OID_XATTR, oid, SMOOTHFS_OID_LEN,
			      XATTR_CREATE);
}

int smoothfs_read_gen_xattr(struct dentry *lower, u32 *gen)
{
	__le32 buf;
	int ret;

	ret = __vfs_getxattr(lower, d_inode(lower), SMOOTHFS_GEN_XATTR,
			     &buf, sizeof(buf));
	if (ret == -ENODATA) {
		*gen = 0;
		return 0;
	}
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;
	*gen = le32_to_cpu(buf);
	return 0;
}

int smoothfs_write_gen_xattr(struct dentry *lower, u32 gen)
{
	__le32 buf = cpu_to_le32(gen);

	return __vfs_setxattr(&nop_mnt_idmap, lower, d_inode(lower),
			      SMOOTHFS_GEN_XATTR, &buf, sizeof(buf), 0);
}

u64 smoothfs_inode_no_from_oid(const u8 oid[SMOOTHFS_OID_LEN])
{
	u64 h = xxh64(oid, SMOOTHFS_OID_LEN, 0);
	return h | (1ULL << 63);
}

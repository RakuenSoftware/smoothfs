// SPDX-License-Identifier: GPL-2.0-only
/*
 * Phase 5.0 check for smoothfs's SMB-shaped control surface:
 *
 *   trusted.smoothfs.fileid   read-only u64 ino | u32 gen = 12 bytes
 *   trusted.smoothfs.lease    1-byte toggle; sets pin_state to PIN_LEASE
 *                             on "1", clears back to PIN_NONE on "0"
 *                             or removexattr. Writes do not persist on
 *                             the lower.
 *
 * This runs entirely in userspace against a smoothfs mount. No Samba
 * required; the test only verifies the kernel-side contract so a
 * future Samba VFS module has a solid foundation.
 *
 * Build: cc -O2 -Wall -o smb_identity_pin smb_identity_pin.c
 * Run as root:
 *   ./smb_identity_pin <path-under-smoothfs>
 *     e.g. ./smb_identity_pin /mnt/pool/some-file
 *
 * Exit: 0 on all assertions passing.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#define FILEID_XATTR  "trusted.smoothfs.fileid"
#define LEASE_XATTR   "trusted.smoothfs.lease"

static int check(const char *what, int cond)
{
	if (cond) {
		printf("  ok    %s\n", what);
		return 0;
	}
	printf("  FAIL  %s (errno=%d %s)\n", what, errno, strerror(errno));
	return 1;
}

int main(int argc, char **argv)
{
	struct stat st;
	unsigned char fid[32];
	unsigned char lease;
	uint64_t ino_from_fid;
	uint32_t gen_from_fid;
	ssize_t n;
	int fails = 0;
	const char *path;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <path>\n", argv[0]);
		return 2;
	}
	path = argv[1];

	if (stat(path, &st) < 0) {
		perror("stat");
		return 1;
	}

	/* --- trusted.smoothfs.fileid -------------------------------- */
	n = getxattr(path, FILEID_XATTR, fid, sizeof(fid));
	fails += check("fileid get returns 12 bytes", n == 12);
	if (n == 12) {
		memcpy(&ino_from_fid, fid, sizeof(ino_from_fid));
		memcpy(&gen_from_fid, fid + 8, sizeof(gen_from_fid));
		fails += check("fileid inode matches stat", ino_from_fid == st.st_ino);
		/* gen is always 0 in Phase 4; log but don't fail */
		printf("  info  fileid gen=%u (Phase 4 hard-wires 0)\n", gen_from_fid);
	}
	fails += check("fileid set rejected with EPERM",
		       setxattr(path, FILEID_XATTR, "x", 1, 0) < 0 && errno == EPERM);

	/* --- trusted.smoothfs.lease toggle -------------------------- */
	n = getxattr(path, LEASE_XATTR, &lease, sizeof(lease));
	fails += check("lease get returns 1 byte initially", n == 1);
	fails += check("lease starts cleared (pin_state != LEASE)", n == 1 && lease == 0);

	{
		unsigned char one = 1;
		fails += check("lease set 1 succeeds",
			       setxattr(path, LEASE_XATTR, &one, 1, 0) == 0);
	}
	n = getxattr(path, LEASE_XATTR, &lease, sizeof(lease));
	fails += check("lease reads back 1 after set", n == 1 && lease == 1);

	/* Idempotent set */
	{
		unsigned char one = 1;
		fails += check("lease set 1 while already set is idempotent",
			       setxattr(path, LEASE_XATTR, &one, 1, 0) == 0);
	}

	{
		unsigned char zero = 0;
		fails += check("lease set 0 clears",
			       setxattr(path, LEASE_XATTR, &zero, 1, 0) == 0);
	}
	n = getxattr(path, LEASE_XATTR, &lease, sizeof(lease));
	fails += check("lease reads back 0 after clear", n == 1 && lease == 0);

	/* removexattr should also clear from LEASE (or be a no-op if 0) */
	{
		unsigned char one = 1;
		setxattr(path, LEASE_XATTR, &one, 1, 0);
	}
	fails += check("removexattr(lease) succeeds",
		       removexattr(path, LEASE_XATTR) == 0);
	n = getxattr(path, LEASE_XATTR, &lease, sizeof(lease));
	fails += check("lease 0 after removexattr", n == 1 && lease == 0);

	/* Value length validation */
	{
		unsigned char two[2] = { 1, 1 };
		fails += check("lease set with size>1 rejected (EINVAL)",
			       setxattr(path, LEASE_XATTR, two, 2, 0) < 0 &&
			       errno == EINVAL);
	}
	{
		unsigned char bogus = 2;
		fails += check("lease set with value=2 rejected (EINVAL)",
			       setxattr(path, LEASE_XATTR, &bogus, 1, 0) < 0 &&
			       errno == EINVAL);
	}

	if (fails)
		printf("FAIL: %d assertions\n", fails);
	else
		printf("PASS: smoothfs smb identity + lease-pin surface\n");
	return fails ? 1 : 0;
}

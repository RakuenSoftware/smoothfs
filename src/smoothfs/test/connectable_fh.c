// SPDX-License-Identifier: GPL-2.0-only
/*
 * Phase 4.5 round-trip check for connectable file handles.
 *
 *   name_to_handle_at(AT_HANDLE_CONNECTABLE) -> open_by_handle_at
 *
 * Exercises smoothfs_encode_fh with parent != NULL (FILEID_SMOOTHFS_OID_CONNECTABLE,
 * 40-byte body) and smoothfs_fh_to_dentry's decode of the connectable
 * form. Prints the handle hex so smoke tests can eyeball that the
 * 16 trailing bytes match the parent's 24-byte non-connectable OID
 * from a parallel name_to_handle_at on the parent path.
 *
 * Build: cc -O2 -Wall -o connectable_fh connectable_fh.c
 * Run:   ./connectable_fh <pool-mount-root> <file-under-that-root>
 *   e.g. ./connectable_fh /mnt/pool /mnt/pool/a/b/c/file.txt
 *
 * Exit: 0 on round-trip success, nonzero on any step's failure.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef AT_HANDLE_CONNECTABLE
#define AT_HANDLE_CONNECTABLE 0x002
#endif

#define FILEID_SMOOTHFS_OID             0x53
#define FILEID_SMOOTHFS_OID_CONNECTABLE 0x54

struct big_fh {
	struct file_handle h;
	unsigned char body[128];
};

static void hex(const char *label, const struct big_fh *fh)
{
	printf("%-18s type=0x%04x len=%u  ", label,
	       fh->h.handle_type, fh->h.handle_bytes);
	for (unsigned i = 0; i < fh->h.handle_bytes; i++)
		printf("%02x", fh->h.f_handle[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	struct big_fh nf, cf;
	int mid, mnt_fd, fd;
	ssize_t n;
	char buf[128];

	if (argc != 3) {
		fprintf(stderr, "usage: %s <mount-root> <target>\n", argv[0]);
		return 2;
	}

	mnt_fd = open(argv[1], O_RDONLY | O_DIRECTORY);
	if (mnt_fd < 0) {
		perror("open mount");
		return 1;
	}

	memset(&nf, 0, sizeof(nf));
	nf.h.handle_bytes = sizeof(nf.body);
	if (name_to_handle_at(AT_FDCWD, argv[2], &nf.h, &mid, 0) < 0) {
		perror("name_to_handle_at (normal)");
		return 1;
	}
	hex("NORMAL", &nf);
	if ((nf.h.handle_type & 0xffff) != FILEID_SMOOTHFS_OID) {
		fprintf(stderr, "unexpected normal type 0x%x\n", nf.h.handle_type);
		return 1;
	}
	if (nf.h.handle_bytes != 24) {
		fprintf(stderr, "unexpected normal length %u\n", nf.h.handle_bytes);
		return 1;
	}

	memset(&cf, 0, sizeof(cf));
	cf.h.handle_bytes = sizeof(cf.body);
	if (name_to_handle_at(AT_FDCWD, argv[2], &cf.h, &mid,
			      AT_HANDLE_CONNECTABLE) < 0) {
		perror("name_to_handle_at (connectable)");
		return 1;
	}
	hex("CONNECTABLE", &cf);
	if ((cf.h.handle_type & 0xffff) != FILEID_SMOOTHFS_OID_CONNECTABLE) {
		fprintf(stderr, "unexpected connectable type 0x%x\n",
			cf.h.handle_type);
		return 1;
	}
	if (cf.h.handle_bytes != 40) {
		fprintf(stderr, "unexpected connectable length %u\n",
			cf.h.handle_bytes);
		return 1;
	}
	if (memcmp(nf.h.f_handle, cf.h.f_handle, 24) != 0) {
		fprintf(stderr, "first 24 bytes of connectable don't match normal\n");
		return 1;
	}

	fd = open_by_handle_at(mnt_fd, &cf.h, O_RDONLY);
	if (fd < 0) {
		perror("open_by_handle_at");
		return 1;
	}
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 0) {
		perror("read");
		return 1;
	}
	buf[n] = 0;
	printf("open_by_handle_at(CONNECTABLE) read %zd bytes: %s",
	       n, n > 0 ? buf : "(empty)\n");

	printf("PASS: round-trip through FILEID_SMOOTHFS_OID_CONNECTABLE\n");
	return 0;
}

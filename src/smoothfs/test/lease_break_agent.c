// SPDX-License-Identifier: GPL-2.0-only
/*
 * lease_break_agent — Phase 5.3 reference userspace listener.
 *
 * Subscribes to fanotify(FAN_MODIFY) on a smoothfs mount and, for
 * every event, clears trusted.smoothfs.lease on the affected file.
 * The kernel only emits FS_MODIFY during a forced cutover (Phase 5.3
 * §Kernel-side: movement.c :: smoothfs_movement_cutover), so in
 * steady-state operation this agent is silent. When an admin issues
 *     tierd-cli smoothfs promote --force --pool ... --oid ... --to N --seq M
 * on a PIN_LEASE-held file, the kernel:
 *   1. allows the plan (force bypasses PIN_LEASE only)
 *   2. runs the normal MOVE_CUTOVER path
 *   3. clears pin_state back to PIN_NONE (so subsequent moves don't
 *      need --force)
 *   4. fires fsnotify(FS_MODIFY) on the smoothfs inode
 *
 * This agent is the Phase 5.3 reference that the completed Samba VFS
 * module (see docs/proposals/completed/smoothfs-samba-vfs-module.md)
 * replaces: the VFS module plugs the same logic into SMB_VFS_SET_LEASE
 * and the share-mode break path so the CIFS client sees a clean
 * oplock/lease break before the new tier's bytes become visible.
 *
 * Build:   cc -O2 -Wall -o lease_break_agent lease_break_agent.c
 * Run:     ./lease_break_agent <smoothfs-mount>  [-v]
 * Stop:    SIGTERM / SIGINT; cleanup is in a signal handler.
 *
 * Exit: 0 on clean shutdown, 2 on argv error, 1 on fanotify/xattr
 *       setup failure.
 *
 * Requires: CAP_SYS_ADMIN (fanotify with FAN_CLASS_NOTIF on a mount).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#define LEASE_XATTR "trusted.smoothfs.lease"

static volatile sig_atomic_t stop;
static int verbose;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void vlog(const char *fmt, ...)
{
	if (!verbose)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* Resolve a fanotify event fd to its path under /proc/self/fd so we
 * can name-based setxattr on it. (The kernel also accepts fsetxattr
 * on the fd itself for user.* / security.*, but trusted.* must go
 * through the VFS xattr handler which gates it on CAP_SYS_ADMIN; the
 * path-based form exercises exactly the surface Samba will use.) */
static int fd_to_path(int fd, char *out, size_t out_len)
{
	char link[64];
	ssize_t n;

	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	n = readlink(link, out, out_len - 1);
	if (n < 0)
		return -1;
	out[n] = 0;
	return 0;
}

static void handle_event(const struct fanotify_event_metadata *ev)
{
	char path[PATH_MAX];
	struct stat st;

	if (ev->fd < 0)
		return;
	if (fd_to_path(ev->fd, path, sizeof(path)) < 0) {
		perror("readlink fanotify fd");
		close(ev->fd);
		return;
	}
	/* Only clear lease on regular files. Directory FS_MODIFY events
	 * (entry creation/deletion) are not covered by the Phase 5.3
	 * contract. */
	if (fstat(ev->fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		close(ev->fd);
		return;
	}
	if (removexattr(path, LEASE_XATTR) < 0) {
		if (errno == ENODATA) {
			vlog("lease_break_agent: %s already unleased\n", path);
		} else {
			fprintf(stderr,
				"lease_break_agent: removexattr(%s): %s\n",
				path, strerror(errno));
		}
	} else {
		fprintf(stderr,
			"lease_break_agent: lease cleared on %s (fsnotify FS_MODIFY)\n",
			path);
	}
	close(ev->fd);
}

int main(int argc, char **argv)
{
	const char *mount;
	int fan_fd;
	struct sigaction sa = { 0 };

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: %s <smoothfs-mount> [-v]\n", argv[0]);
		return 2;
	}
	mount = argv[1];
	if (argc == 3 && strcmp(argv[2], "-v") == 0)
		verbose = 1;

	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	fan_fd = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC,
			       O_RDONLY | O_LARGEFILE);
	if (fan_fd < 0) {
		perror("fanotify_init");
		return 1;
	}
	if (fanotify_mark(fan_fd,
			  FAN_MARK_ADD | FAN_MARK_MOUNT,
			  FAN_MODIFY,
			  AT_FDCWD, mount) < 0) {
		perror("fanotify_mark");
		close(fan_fd);
		return 1;
	}
	fprintf(stderr, "lease_break_agent: watching %s for FAN_MODIFY\n",
		mount);

	while (!stop) {
		struct pollfd p = { .fd = fan_fd, .events = POLLIN };
		int n = poll(&p, 1, 500);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}
		if (n == 0)
			continue;

		char buf[4096];
		ssize_t got = read(fan_fd, buf, sizeof(buf));
		if (got < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			perror("read fanotify");
			break;
		}
		struct fanotify_event_metadata *ev =
			(struct fanotify_event_metadata *)buf;
		while (FAN_EVENT_OK(ev, got)) {
			handle_event(ev);
			ev = FAN_EVENT_NEXT(ev, got);
		}
	}

	close(fan_fd);
	fprintf(stderr, "lease_break_agent: exiting\n");
	return 0;
}

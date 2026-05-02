#!/bin/bash
# Lower-filesystem helpers for smoothfs runtime harnesses.
#
# Source this from a harness script to abstract the mkfs / capability
# checks for the lower tier filesystem. Per the kernel-test-matrix
# coverage spec, smoothfs is supported on top of XFS / ext4 / btrfs /
# zfs lower filesystems with different coverage requirements; harnesses
# that touch lookup, movement, metadata replay, write staging, or
# protocol export should be runnable against each supported lower.
#
# Drive the lower fs via the SMOOTHFS_LOWER_FS environment variable:
#
#   SMOOTHFS_LOWER_FS=xfs    (default)
#   SMOOTHFS_LOWER_FS=ext4
#   SMOOTHFS_LOWER_FS=btrfs
#
# zfs lowers are intentionally left out of this helper because they
# require a `zpool create` flow on a block device, not a `mkfs` on a
# loopback file. The zfs path will land alongside an OpenZFS-bearing
# appliance image.
#
# Usage from a harness:
#
#   . "$(dirname "$0")/lower_fs_lib.sh"
#   ...
#   truncate -s 1G "$ROOT/fast.img" "$ROOT/slow.img"
#   mkfs_lower "$ROOT/fast.img"
#   mkfs_lower "$ROOT/slow.img"
#
# Capability queries for tests that exercise reflink / O_DIRECT etc:
#
#   if lower_supports_reflink; then ...
#

LOWER_FS=${SMOOTHFS_LOWER_FS:-xfs}

mkfs_lower() {
	local img=$1
	case "$LOWER_FS" in
	xfs)
		mkfs.xfs -q -f "$img"
		;;
	ext4)
		mkfs.ext4 -q -F "$img"
		;;
	btrfs)
		mkfs.btrfs -q -f "$img"
		;;
	*)
		echo "lower_fs_lib: unsupported SMOOTHFS_LOWER_FS=$LOWER_FS" >&2
		echo "  supported: xfs, ext4, btrfs" >&2
		return 1
		;;
	esac
}

# Capability flags for tests that conditionally skip on lowers without
# the relevant feature. xfs and btrfs do reflink (FICLONE / FICLONERANGE
# / copy_file_range with shared extents); ext4 does not.
lower_supports_reflink() {
	case "$LOWER_FS" in
	xfs|btrfs) return 0 ;;
	*) return 1 ;;
	esac
}

# Print the current lower fs name (useful for log messages).
lower_fs_name() {
	echo "$LOWER_FS"
}

# Skip the calling harness if the current lower fs is not in the
# supported set. Prints a SKIP line to stdout and exits 0 (run-runtime-
# harnesses treats exit 0 as success). Use at the top of any harness
# that assumes lower-fs-specific semantics:
#
#   require_lower_fs xfs ext4
#
# Reasons a harness might exclude a lower:
#   - btrfs falls back to buffered I/O for misaligned O_DIRECT instead
#     of returning EINVAL (so odirect's misalign-rejection assertion
#     doesn't apply on btrfs)
#   - protocol harnesses (smb*, iscsi*, cthon04) are XFS-primary per
#     the kernel-test-matrix doc; ext4/btrfs aren't validated for the
#     full protocol surface
#   - reflink/remap harnesses need the underlying fs to support shared
#     extents (xfs and btrfs do, ext4 does not)
require_lower_fs() {
	local f
	for f in "$@"; do
		if [ "$LOWER_FS" = "$f" ]; then
			return 0
		fi
	done
	echo "$(basename "${0:-harness}"): SKIP — lower fs $LOWER_FS not in supported set ($*)"
	exit 0
}

#!/bin/bash
# Shared helpers for smoothfs tier-spill shell harnesses.

set -u

. "$(dirname "$0")/lower_fs_lib.sh"

SPILL_ROOT=${SPILL_ROOT:-/tmp/smoothfs-tier-spill}
SPILL_UUID=${SPILL_UUID:-00000000-0000-0000-0000-00000000f001}
SPILL_FAST_MB=${SPILL_FAST_MB:-512}
SPILL_SLOW_MB=${SPILL_SLOW_MB:-512}
SPILL_FILL_MB=${SPILL_FILL_MB:-500}

spill_rc=0

spill_assert() {
	if "$@"; then
		echo "  ok    $*"
	else
		echo "  FAIL  $*"
		spill_rc=1
	fi
}

spill_cleanup() {
	umount -l "$SPILL_ROOT/server" 2>/dev/null || true
	umount -l "$SPILL_ROOT/fast" 2>/dev/null || true
	umount -l "$SPILL_ROOT/slow" 2>/dev/null || true
	rm -rf "$SPILL_ROOT"
}

spill_setup_pool() {
	local pool_name=${1:-spill}
	local uuid=${2:-$SPILL_UUID}

	rm -rf "$SPILL_ROOT"
	mkdir -p "$SPILL_ROOT"/{fast,slow,server}
	truncate -s "${SPILL_FAST_MB}M" "$SPILL_ROOT/fast.img"
	truncate -s "${SPILL_SLOW_MB}M" "$SPILL_ROOT/slow.img"
	mkfs_lower "$SPILL_ROOT/fast.img"
	mkfs_lower "$SPILL_ROOT/slow.img"
	mount -o loop "$SPILL_ROOT/fast.img" "$SPILL_ROOT/fast"
	mount -o loop "$SPILL_ROOT/slow.img" "$SPILL_ROOT/slow"
	mount -t smoothfs -o "pool=${pool_name},uuid=${uuid},tiers=$SPILL_ROOT/fast:$SPILL_ROOT/slow" \
		none "$SPILL_ROOT/server"
}

spill_fill_fast_tier() {
	local fill_mb=${1:-$SPILL_FILL_MB}

	dd if=/dev/zero of="$SPILL_ROOT/fast/.spill-fill.bin" bs=1M count="$fill_mb" \
		conv=fsync status=none
}

spill_make_payload() {
	local path=$1
	local mb=${2:-16}

	dd if=/dev/urandom of="$path" bs=1M count="$mb" status=none
}

spill_sha() {
	sha256sum "$1" | awk '{print $1}'
}

spill_finish() {
	local label=$1

	echo
	if [ $spill_rc -eq 0 ]; then
		echo "${label}: PASS"
	else
		echo "${label}: FAIL"
	fi
	exit $spill_rc
}

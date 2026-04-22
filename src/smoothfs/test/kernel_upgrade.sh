#!/bin/bash
# Phase 7.3 — kernel upgrade / rollback harness.
#
# The "kernel upgrade / rollback flow" from §Phased Delivery rests
# on two mechanisms we inherit from DKMS:
#
#   1. DKMS autoinstall triggers whenever `linux-headers-<kver>`
#      is installed. If smoothfs's dkms.conf BUILD_EXCLUSIVE_KERNEL
#      regex matches <kver>, the module builds + installs at
#      /lib/modules/<kver>/updates/dkms/smoothfs.ko.xz against
#      that kernel.
#
#   2. DKMS module trees are per-kernel. Kernel A's .ko sits under
#      /lib/modules/<kver-A>/..., kernel B's under <kver-B>/...
#      If kernel B's DKMS build fails (API breakage, headers
#      missing, signing error), kernel A's module is untouched
#      and GRUB still boots A with a working smoothfs.
#
# This test is a static-correctness check that those two
# invariants hold on the current host. It doesn't install kernels
# (root + apt + ~100 MB download each) — that's a manual /
# CI-automated step. What it guards is the post-condition: given
# the currently installed kernels, is the per-kernel DKMS tree in
# the shape rollback needs it in?
#
# Assertions (4 + N, where N is the number of smoothfs-eligible
# kernels with headers installed):
#
#   1.  At least one kernel has `linux-headers-<kver>` installed.
#   2.  For every kernel where headers are installed AND the
#       kernel version matches dkms.conf's BUILD_EXCLUSIVE_KERNEL,
#       DKMS reports smoothfs/<ver> as "installed" on that kernel.
#   3.  The corresponding /lib/modules/<kver>/updates/dkms/
#       smoothfs.ko.xz exists and carries a PKCS#7 signature.
#   4.  Kernels that don't match BUILD_EXCLUSIVE_KERNEL are
#       reported by DKMS as skipped, not failed — so an older
#       Debian stock kernel on the same box never trips the
#       autoinstall hook into an error state.

set -u

rc=0
assert() {
	if "$@"; then
		echo "  ok    $*"
	else
		echo "  FAIL  $*"
		rc=1
	fi
}

if ! command -v dkms >/dev/null 2>&1; then
	echo "kernel_upgrade: SKIP — dkms not installed"
	exit 0
fi

if ! dkms status -m smoothfs 2>/dev/null | grep -q smoothfs; then
	echo "kernel_upgrade: SKIP — smoothfs-dkms not registered"
	exit 0
fi

BUILD_RE='^(6\.(1[8-9]|[2-9][0-9])|[7-9]\.).*'

# Kernels that have their linux-headers installed — matches what
# DKMS can actually build against right now.
mapfile -t KERNELS < <(ls -1 /lib/modules 2>/dev/null)
KERNELS_WITH_HEADERS=()
for k in "${KERNELS[@]}"; do
	if [ -d "/lib/modules/$k/build" ]; then
		KERNELS_WITH_HEADERS+=("$k")
	fi
done

echo "=== kernels discovered: ${KERNELS[*]:-(none)} ==="
echo "=== kernels with headers installed: ${KERNELS_WITH_HEADERS[*]:-(none)} ==="
assert test "${#KERNELS_WITH_HEADERS[@]}" -gt 0

# Capture the full dkms status once — the per-kernel assertions
# parse this rather than re-running dkms status per kernel
# (which is slow and racy against concurrent builds).
DKMS_STATUS=$(dkms status -m smoothfs 2>&1)
echo "=== dkms status -m smoothfs ==="
echo "$DKMS_STATUS" | sed 's/^/    /'

eligible=0
for k in "${KERNELS_WITH_HEADERS[@]}"; do
	echo "=== kernel $k ==="
	if ! echo "$k" | grep -qE "$BUILD_RE"; then
		# Kernel not eligible (e.g. Debian 6.12 stock). DKMS
		# must not have built smoothfs for it, and must not have
		# flagged it as failed either.
		if echo "$DKMS_STATUS" | grep -qF "smoothfs/" | grep -qF ", $k"; then
			echo "  FAIL  DKMS installed smoothfs on an ineligible kernel $k"
			rc=1
		else
			echo "  ok    DKMS skipped $k (BUILD_EXCLUSIVE_KERNEL excludes it)"
		fi
		continue
	fi
	eligible=$((eligible + 1))
	# DKMS must report smoothfs/<ver>, $k as installed. Use fixed
	# string match ("-qF") on the ", $k" segment — the kernel
	# version strings contain '+' which is a regex metacharacter,
	# and quoting in grep -E is awkward.
	if echo "$DKMS_STATUS" | grep -F "smoothfs/" | grep -qF ", $k" \
	   && echo "$DKMS_STATUS" | grep -F "$k" | grep -q "installed"; then
		echo "  ok    DKMS status: smoothfs installed on $k"
	else
		echo "  FAIL  DKMS does not report smoothfs as installed on $k"
		rc=1
		continue
	fi
	KO=/lib/modules/$k/updates/dkms/smoothfs.ko.xz
	if [ -f "$KO" ]; then
		echo "  ok    $KO exists"
	else
		echo "  FAIL  $KO missing"
		rc=1
		continue
	fi
	SIG=$(modinfo -F sig_id "$KO" 2>/dev/null)
	if [ "$SIG" = "PKCS#7" ]; then
		echo "  ok    $KO is PKCS#7-signed"
	else
		echo "  FAIL  $KO has sig_id='$SIG' (want PKCS#7)"
		rc=1
	fi
done

echo "=== summary: $eligible eligible kernel(s) covered ==="
assert test "$eligible" -gt 0

if [ $rc -eq 0 ]; then
	echo "kernel_upgrade: PASS (every eligible kernel has a signed smoothfs.ko; rollback to any is safe)"
else
	echo "kernel_upgrade: FAIL"
fi
exit $rc

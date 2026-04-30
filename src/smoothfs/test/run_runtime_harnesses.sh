#!/bin/bash
# Run privileged smoothfs runtime harnesses in a predictable order.
#
# Default suite:
#   core      smoothfs-only loopback/runtime tests
#
# Optional suites:
#   protocol  NFS/SMB/iSCSI tests that need external services/packages
#   ops       DKMS/kernel-upgrade/module-signing operational checks
#   all       core + protocol + ops
#
# Overrides:
#   SMOOTHFS_RUNTIME_SUITE=core|protocol|ops|all
#   SMOOTHFS_RUNTIME_TESTS="script-a.sh script-b.sh"
#   SMOOTHFS_RUNTIME_DRY_RUN=1
#   SMOOTHFS_RUNTIME_FAIL_FAST=1

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SUITE=${SMOOTHFS_RUNTIME_SUITE:-core}
DRY_RUN=${SMOOTHFS_RUNTIME_DRY_RUN:-0}
FAIL_FAST=${SMOOTHFS_RUNTIME_FAIL_FAST:-0}

core_tests=(
	netlink_receive_close.sh
	tier_spill_basic_create.sh
	tier_spill_nested_parent.sh
	tier_spill_union_readdir.sh
	tier_spill_unlink_finds_right_tier.sh
	tier_spill_rename_xdev.sh
	tier_spill_crash_replay.sh
	metadata_tier_activity_gate.sh
	write_staging_truncate.sh
	write_staging_range.sh
	write_staging_range_crash_replay.sh
	write_staging_drain_mask.sh
	movement_nested_cutover.sh
	movement_open_fd_cutover.sh
	write_staging_direct_cutover.sh
	odirect.sh
)

protocol_tests=(
	cthon04.sh
	smb_roundtrip.sh
	smb_vfs_module.sh
	smbtorture.sh
	smbtorture_xfs_baseline.sh
	iscsi_pin.sh
	iscsi_roundtrip.sh
	iscsi_restart.sh
)

ops_tests=(
	kernel_upgrade.sh
	module_signing.sh
)

manifest_exclusions=(
	run_runtime_harnesses.sh
	tier_spill_lib.sh
)

tests_for_suite() {
	case "$1" in
	core)
		printf '%s\n' "${core_tests[@]}"
		;;
	protocol)
		printf '%s\n' "${protocol_tests[@]}"
		;;
	ops)
		printf '%s\n' "${ops_tests[@]}"
		;;
	all)
		printf '%s\n' "${core_tests[@]}" "${protocol_tests[@]}" "${ops_tests[@]}"
		;;
	*)
		echo "unknown SMOOTHFS_RUNTIME_SUITE=$1" >&2
		return 2
		;;
	esac
}

resolve_test() {
	case "$1" in
	/*|*/*)
		printf '%s\n' "$1"
		;;
	*)
		printf '%s/%s\n' "$SCRIPT_DIR" "$1"
		;;
	esac
}

is_manifest_exclusion() {
	local candidate=$1
	local exclusion

	for exclusion in "${manifest_exclusions[@]}"; do
		if [ "$candidate" = "$exclusion" ]; then
			return 0
		fi
	done
	return 1
}

in_manifest() {
	local candidate=$1
	local manifest_entry

	for manifest_entry in "${core_tests[@]}" "${protocol_tests[@]}" "${ops_tests[@]}"; do
		if [ "$candidate" = "$manifest_entry" ]; then
			return 0
		fi
	done
	return 1
}

validate_manifest() {
	local test_name
	local test_path
	local seen="
"
	local script_path
	local script_name

	for test_name in "${core_tests[@]}" "${protocol_tests[@]}" "${ops_tests[@]}"; do
		test_path=$(resolve_test "$test_name")
		if [ ! -f "$test_path" ]; then
			echo "  FAIL  missing harness: $test_path" >&2
			return 1
		fi
		case "$seen" in
		*"
$test_name
"*)
			echo "  FAIL  duplicate harness manifest entry: $test_name" >&2
			return 1
			;;
		esac
		seen="${seen}${test_name}
"
	done

	while IFS= read -r script_path; do
		script_name=$(basename "$script_path")
		if is_manifest_exclusion "$script_name"; then
			continue
		fi
		if ! in_manifest "$script_name"; then
			echo "  FAIL  unlisted runtime harness: $script_path" >&2
			return 1
		fi
	done < <(find "$SCRIPT_DIR" -maxdepth 1 -type f -name '*.sh' -print | sort)
}

if [ -n "${SMOOTHFS_RUNTIME_TESTS:-}" ]; then
	# shellcheck disable=SC2206 # Intentionally split simple script names.
	tests=($SMOOTHFS_RUNTIME_TESTS)
else
	case "$SUITE" in
	core|protocol|ops|all) ;;
	*)
		echo "unknown SMOOTHFS_RUNTIME_SUITE=$SUITE" >&2
		exit 2
		;;
	esac
	mapfile -t tests < <(tests_for_suite "$SUITE")
fi

if [ -z "${SMOOTHFS_RUNTIME_TESTS:-}" ]; then
	if ! validate_manifest; then
		exit 1
	fi
fi

echo "=== smoothfs runtime harnesses: suite=$SUITE ==="
for test_name in "${tests[@]}"; do
	test_path=$(resolve_test "$test_name")
	if [ ! -f "$test_path" ]; then
		echo "  FAIL  missing harness: $test_path" >&2
		exit 1
	fi
	echo "  $test_path"
done

if [ "$DRY_RUN" = "1" ]; then
	echo "runtime harness dry-run: PASS"
	exit 0
fi

if [ "$(id -u)" -ne 0 ]; then
	echo "runtime harnesses require root privileges for mounts, modules, xattrs, and protocol services" >&2
	echo "use SMOOTHFS_RUNTIME_DRY_RUN=1 for manifest validation" >&2
	exit 1
fi

# Tests like tier_spill_crash_replay deliberately rmmod smoothfs as part of
# their replay flow. Without this helper the next test sees an unloaded module
# and every subsequent test in the suite cascade-fails with "smoothfs kernel
# module not loaded". Re-insmod from the build tree before each test, unless
# the operator opts out by setting SMOOTHFS_RUNTIME_MODULE_PATH= (empty).
MODULE_PATH=${SMOOTHFS_RUNTIME_MODULE_PATH-$SCRIPT_DIR/../smoothfs.ko}

ensure_smoothfs_loaded() {
	if [ -z "$MODULE_PATH" ]; then
		return 0
	fi
	if lsmod | awk '{print $1}' | grep -qx smoothfs; then
		return 0
	fi
	if [ ! -f "$MODULE_PATH" ]; then
		echo "  WARN  smoothfs not loaded and module file $MODULE_PATH not found; skipping reload" >&2
		return 0
	fi
	if ! insmod "$MODULE_PATH"; then
		echo "  WARN  insmod $MODULE_PATH failed; harness will see unloaded module" >&2
		return 1
	fi
}

rc=0
for test_name in "${tests[@]}"; do
	test_path=$(resolve_test "$test_name")
	ensure_smoothfs_loaded
	echo
	echo "=== RUN $(basename "$test_path") ==="
	if bash "$test_path"; then
		echo "=== PASS $(basename "$test_path") ==="
	else
		status=$?
		echo "=== FAIL $(basename "$test_path") status=$status ==="
		rc=1
		if [ "$FAIL_FAST" = "1" ]; then
			exit "$rc"
		fi
	fi
done

echo
if [ "$rc" -eq 0 ]; then
	echo "runtime harnesses: PASS"
else
	echo "runtime harnesses: FAIL"
fi
exit "$rc"

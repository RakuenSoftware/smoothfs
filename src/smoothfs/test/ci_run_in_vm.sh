#!/bin/bash
# Runs INSIDE the virtme-ng guest (clean kernel, no production smoothfs
# loaded). Loads the build-under-test and runs the runtime harness suite.
# Writes the suite's exit code to vm_result.txt in the shared working dir
# so the host job can read it even if the VM exit code is not propagated.
set -uxo pipefail

cd "${SMOOTHFS_CI_WORKDIR:-$PWD}"

rc=1
{
	insmod src/smoothfs/smoothfs.ko || { echo "insmod failed"; exit 1; }
	lsmod | grep -q '^smoothfs' || { echo "smoothfs not loaded"; exit 1; }
	SMOOTHFS_RUNTIME_SUITE="${SMOOTHFS_RUNTIME_SUITE:-core}" \
	SMOOTHFS_RUNTIME_TESTS="${SMOOTHFS_RUNTIME_TESTS:-}" \
	SMOOTHFS_RUNTIME_FAIL_FAST="${SMOOTHFS_RUNTIME_FAIL_FAST:-0}" \
	SMOOTHFS_LOWER_FS="${SMOOTHFS_LOWER_FS:-xfs}" \
		bash src/smoothfs/test/run_runtime_harnesses.sh
	rc=$?
}
echo "$rc" > vm_result.txt
exit "$rc"

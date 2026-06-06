#!/bin/bash
# Runs INSIDE the virtme-ng guest (clean kernel, no production smoothfs
# loaded). Loads the build-under-test and runs the runtime harness suite.
# Its exit code is propagated back to the host as vng's exit code (via the
# virtme.ret port), so the host job fails iff the suite fails.
#
# virtme routes the guest's stdout to the host but its stderr to /dev/null,
# so fold stderr into stdout to capture the full harness output.
set -uo pipefail
exec 2>&1

cd "${SMOOTHFS_CI_WORKDIR:-$PWD}" || { echo "guest: cd failed"; exit 1; }

# Go toolchain for harnesses that `go run` a helper. Installed on the host
# at /usr/local/go (shared via virtiofs) with caches warmed in this 9p dir.
export PATH="/usr/local/go/bin:${PATH}"
export GOCACHE="${PWD}/.gocache" GOMODCACHE="${PWD}/.gomodcache" \
       GOFLAGS=-mod=mod GOTOOLCHAIN=local HOME="${HOME:-/tmp}"

echo "== guest: loading smoothfs.ko =="
insmod src/smoothfs/smoothfs.ko || { echo "guest: insmod failed"; exit 1; }
lsmod | grep -q '^smoothfs' || { echo "guest: smoothfs not loaded"; exit 1; }

echo "== guest: running '${SMOOTHFS_RUNTIME_SUITE:-core}' suite =="
SMOOTHFS_RUNTIME_SUITE="${SMOOTHFS_RUNTIME_SUITE:-core}" \
SMOOTHFS_RUNTIME_TESTS="${SMOOTHFS_RUNTIME_TESTS:-}" \
SMOOTHFS_RUNTIME_FAIL_FAST="${SMOOTHFS_RUNTIME_FAIL_FAST:-0}" \
SMOOTHFS_LOWER_FS="${SMOOTHFS_LOWER_FS:-xfs}" \
	bash src/smoothfs/test/run_runtime_harnesses.sh
rc=$?

echo "== guest: suite exit ${rc} =="
exit "${rc}"

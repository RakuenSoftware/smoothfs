#!/bin/bash
# Idempotent setup for a smoothfs privileged runtime runner on Debian 13.
#
# Configures a fresh smoothfs-runtime-<arch> self-hosted runner so it
# can pass the core / protocol / ops / cthon04 suites that the
# .github/workflows/runtime-harnesses.yml workflow drives. Run once
# on the runner host as root (sudo bash runner-setup.sh). Re-running
# is safe: every step is conditional on the missing-state it fixes.
#
# What this script does NOT do:
#   - Install or upgrade the running kernel. The smoothfs DKMS floor
#     is 6.18; on Debian 13 that's only available from
#     trixie-backports. If `uname -r` is older, run
#       apt-get install -y -t trixie-backports linux-image-<arch> linux-headers-<arch>
#     and reboot before re-running this script.
#   - Register the GitHub Actions runner. Use the standard runner
#     config flow (config.sh / svc.sh) and add this script's
#     `runner-path-fix` step afterwards. Labels expected by the
#     workflow: self-hosted, linux, smoothfs-runtime-<arch>.
#   - Provision the VM / hardware. The workflow assumes a Debian 13
#     trixie host with a 6.18+ kernel, passwordless sudo for the
#     runner user, and /dev/loop-control accessible.

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "runner-setup.sh: must run as root (use sudo)" >&2
    exit 1
fi

ARCH=$(dpkg --print-architecture)
RUNNER_USER=${RUNNER_USER:-smoothfs}
RUNNER_HOME=$(getent passwd "$RUNNER_USER" | awk -F: '{print $6}')
if [ -z "$RUNNER_HOME" ]; then
    echo "runner-setup.sh: user '$RUNNER_USER' not found; set RUNNER_USER=<name>" >&2
    exit 1
fi

# ---------- 1. apt sources: enable deb-src for build-dep samba ----------
echo "=== enabling deb-src on trixie + trixie-security ==="
if ! grep -q "^Types: deb deb-src" /etc/apt/sources.list.d/debian.sources; then
    sed -i 's|^Types: deb$|Types: deb deb-src|' /etc/apt/sources.list.d/debian.sources
fi
apt-get update -q

# ---------- 2. backports headers (kernel itself is operator-managed) ----------
echo "=== installing matching headers from trixie-backports ==="
apt-get install -y -t trixie-backports \
    "linux-headers-$ARCH" \
    libelf-dev

# ---------- 3. build essentials + smoothfs build deps ----------
echo "=== installing build essentials ==="
DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential dkms git make gcc bc bison flex \
    libssl-dev xfsprogs e2fsprogs btrfs-progs \
    sudo curl jq ca-certificates devscripts equivs strace \
    golang-go

# ---------- 4. protocol + ops suite deps ----------
echo "=== installing protocol + ops deps ==="
DEBIAN_FRONTEND=noninteractive apt-get install -y \
    samba samba-testsuite smbclient \
    nfs-kernel-server nfs-common \
    open-iscsi targetcli-fb \
    mokutil time groff

# ---------- 5. cthon04 NFS test suite deps via samba build-dep ----------
# The smoothfs-samba-vfs deb's build path needs a full Samba in-tree
# rebuild against `apt-get source samba`; pull its build deps here.
# trixie-backports has libngtcp2-dev > 1.12 which the trixie samba
# build wants (vs trixie-security's 1.11.x).
echo "=== installing samba build-deps for smoothfs-samba-vfs ==="
DEBIAN_FRONTEND=noninteractive apt-get install -y -t trixie-backports \
    libngtcp2-dev libngtcp2-crypto-gnutls-dev
DEBIAN_FRONTEND=noninteractive apt-get build-dep -y samba

# ---------- 6. iSCSI services ----------
echo "=== enabling iSCSI services ==="
systemctl enable --now iscsid 2>/dev/null || true
systemctl enable --now open-iscsi 2>/dev/null || true

# ---------- 7. cthon04 NFS test suite source + build ----------
# The cthon04 harness expects /opt/cthon04 with built basic/test*
# binaries. The leil-io fork has the modern-glibc compile fixes; build
# with permissive CFLAGS to bypass the 2004-era K&R declarations.
# tools/dirdmp build is allowed to fail — basic/general/special don't
# use it.
echo "=== cthon04 NFS test suite ==="
if [ ! -x /opt/cthon04/basic/test1 ]; then
    rm -rf /opt/cthon04
    git clone --depth=1 https://github.com/leil-io/cthon04 /opt/cthon04
    chown -R "$RUNNER_USER:$RUNNER_USER" /opt/cthon04
    make -C /opt/cthon04 \
        CFLAGS="-O2 -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-int" \
        || echo "  (tools/ failed to build — basic/general/special suites do not need it)"
fi

# ---------- 8. fix actions-runner PATH ----------
# The default PATH the GitHub Actions runner exports does NOT include
# /sbin or /usr/sbin, where mkfs.xfs / insmod / lsmod / modinfo /
# modprobe live on Debian. The runtime workflow's "Check privileged
# runner prerequisites" step fails with "command not found" without
# this. Fix the runner's .path file (read on each job) and reload.
RUNNER_DIR=$RUNNER_HOME/actions-runner
if [ -f "$RUNNER_DIR/.path" ]; then
    if ! grep -q '/usr/sbin' "$RUNNER_DIR/.path"; then
        echo "=== fixing actions-runner .path to include /sbin ==="
        echo "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/games" > "$RUNNER_DIR/.path"
        chown "$RUNNER_USER:$RUNNER_USER" "$RUNNER_DIR/.path"
        systemctl restart 'actions.runner.*' 2>/dev/null || true
    fi
else
    echo "  (no actions-runner .path at $RUNNER_DIR; skip — register the runner first)"
fi

# ---------- 9. passwordless sudo for the runner user ----------
SUDOERS_FILE=/etc/sudoers.d/smoothfs-runner
if [ ! -f "$SUDOERS_FILE" ]; then
    echo "=== granting passwordless sudo to $RUNNER_USER ==="
    echo "$RUNNER_USER ALL=(ALL) NOPASSWD:ALL" > "$SUDOERS_FILE"
    chmod 0440 "$SUDOERS_FILE"
fi

echo
echo "=== runner setup complete ==="
echo
echo "Verify with:"
echo "  uname -r                                # must be 6.18+"
echo "  sudo modprobe smoothfs && lsmod | grep smoothfs"
echo "  /opt/cthon04/basic/test1 -h             # cthon04 binary built"
echo "  ls /etc/sudoers.d/smoothfs-runner       # passwordless sudo"
echo
echo "If the GitHub Actions runner service is registered, the"
echo "runner-path-fix above already restarted it. Otherwise register"
echo "it now per:"
echo "  https://github.com/RakuenSoftware/smoothfs/settings/actions/runners/new"
echo "and ensure it carries the labels: self-hosted, linux,"
echo "smoothfs-runtime-$ARCH"

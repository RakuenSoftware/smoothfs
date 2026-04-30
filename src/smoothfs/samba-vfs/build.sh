#!/bin/bash
# Build the smoothfs Samba VFS module against a matching Samba
# source tree and install it into the system's Samba modules
# directory.
#
# The VFS ABI changes between Samba minor versions, so the source
# tree MUST match the installed smbd's version. The script reports
# the mismatch and exits if they differ — don't try to patch it.
#
# Usage:
#   bash build.sh
#     expects Samba source at /tmp/samba-<installed-version>+dfsg.
#     apt-get source samba=<version> lays it down there.
#
# Output:
#   /usr/lib/<multiarch>/samba/vfs/smoothfs.so
#
# Run as root (install step writes under /usr/lib).

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
MULTIARCH=${DEB_HOST_MULTIARCH:-}
if [ -z "$MULTIARCH" ]; then
    MULTIARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || true)
fi
if [ -z "$MULTIARCH" ]; then
    echo "unable to determine Debian multiarch triplet" >&2
    exit 1
fi
LIBDIR=/usr/lib/${MULTIARCH}

INSTALLED_FULL=$(dpkg-query -W -f='${Version}\n' samba 2>/dev/null | sed 's/^2://')
INSTALLED_VER=$(echo "$INSTALLED_FULL" | sed 's/-.*//')
if [ -z "$INSTALLED_VER" ]; then
    echo "samba not installed — apt-get install samba samba-testsuite first" >&2
    exit 1
fi
SAMBA_SRC=/tmp/samba-${INSTALLED_VER}
if [ ! -d "$SAMBA_SRC" ]; then
    echo "missing Samba source at $SAMBA_SRC" >&2
    echo "Fix: cd /tmp && apt-get source samba=2:${INSTALLED_FULL}" >&2
    exit 1
fi
# Vendor suffix is load-bearing. The installed libsmbd-base carries
# private-symbol versions like SAMBA_4.22.8_DEBIAN_4.22.8_DFSG_0_DEB13U1_PRIVATE_SAMBA;
# if our build omits the matching --vendor-suffix flag, smbd refuses
# to load the module with 'version SAMBA_4.22.8_PRIVATE_SAMBA not found'.
VENDOR_SUFFIX="Debian-${INSTALLED_FULL}"

echo "=== copying vfs_smoothfs.c into Samba source tree ==="
cp "$HERE/vfs_smoothfs.c" "$SAMBA_SRC/source3/modules/vfs_smoothfs.c"

echo "=== registering module in source3/modules/wscript_build ==="
WSCRIPT_MODULES="$SAMBA_SRC/source3/modules/wscript_build"
if ! grep -q "vfs_smoothfs" "$WSCRIPT_MODULES"; then
    cat >> "$WSCRIPT_MODULES" <<'EOF'

# smoothfs VFS module — Phase 5.8+ of the stacked-tiering proposal.
# Transparent passthrough today (Phase 5.8.1); lease/fileid/fanotify
# overrides land in later sub-phases.
bld.SAMBA3_MODULE('vfs_smoothfs',
                 subsystem='vfs',
                 source='vfs_smoothfs.c',
                 deps='samba-util',
                 init_function='',
                 internal_module=bld.SAMBA3_IS_STATIC_MODULE('vfs_smoothfs'),
                 enabled=bld.SAMBA3_IS_ENABLED_MODULE('vfs_smoothfs'))
EOF
fi

# The module also has to be in source3/wscript's default_shared_modules
# list (or passed via --with-shared-modules at configure time) before
# waf will build it. Patching the wscript is the least-surprising way
# to make `--with-shared-modules` on re-config idempotent.
WSCRIPT_S3="$SAMBA_SRC/source3/wscript"
if ! grep -q "vfs_smoothfs" "$WSCRIPT_S3"; then
    # Insert right after the line that extends with vfs_catia etc.
    python3 - "$WSCRIPT_S3" <<'PY'
import sys
path = sys.argv[1]
with open(path) as f:
    src = f.read()
marker = "'vfs_preopen', 'vfs_catia',"
assert marker in src, f"marker not found in {path}"
new = src.replace(
    marker,
    marker + "\n                                      'vfs_smoothfs',",
    1,
)
with open(path, "w") as f:
    f.write(new)
PY
fi

echo "=== configuring Samba (always — so the default-modules edit takes effect) ==="
cd "$SAMBA_SRC"
rm -rf bin
PYTHONHASHSEED=1 ./buildtools/bin/waf configure \
    --vendor-suffix="${VENDOR_SUFFIX}" \
    --enable-fhs --prefix=/usr \
    --libdir="${LIBDIR}" \
    --with-modulesdir="${LIBDIR}/samba" \
    --without-ad-dc --without-ldap --without-ads --without-gpgme \
    --without-winbind --without-systemd --disable-cups \
    --without-pam --without-acl-support --without-quota \
    --without-automount --without-utmp --without-fam \
    >/tmp/smoothfs-vfs-configure.log 2>&1

echo "=== building vfs_smoothfs ==="
PYTHONHASHSEED=1 ./buildtools/bin/waf build --targets=vfs_smoothfs \
    >/tmp/smoothfs-vfs-build.log 2>&1
if [ ! -f bin/default/source3/modules/libvfs_module_smoothfs.so ]; then
    echo "build failed — see /tmp/smoothfs-vfs-build.log (last 30 lines):" >&2
    tail -30 /tmp/smoothfs-vfs-build.log >&2
    exit 1
fi

BUILT_SO="$SAMBA_SRC/bin/default/source3/modules/libvfs_module_smoothfs.so"
if [ -n "${SMOOTHFS_VFS_OUTPUT:-}" ]; then
    # Package-build mode (Phase 7.1 debian/rules): emit the .so at a
    # caller-specified path instead of installing to the system tree,
    # so dpkg-buildpackage can stage it under debian/<pkg>/usr/lib/.
    install -D -m 0644 "$BUILT_SO" "$SMOOTHFS_VFS_OUTPUT"
    echo "=== emitted $SMOOTHFS_VFS_OUTPUT ==="
    ls -la "$SMOOTHFS_VFS_OUTPUT"
    exit 0
fi

DEST=${LIBDIR}/samba/vfs/smoothfs.so
install -D -m 0644 "$BUILT_SO" "$DEST"
echo "=== installed $DEST ==="
ls -la "$DEST"

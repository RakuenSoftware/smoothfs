#!/bin/bash
# Build smoothfs's Debian packages in a Debian 13 container (run inside
# debian:trixie, as root, from the repository root):
#
#   packaging/build-debs.sh dkms <out-dir>       smoothfs-dkms (arch all)
#   packaging/build-debs.sh samba-vfs <out-dir>  smoothfs-samba-vfs (this arch)
#
# smoothfs-samba-vfs links against Samba's vendor-suffixed private ABI, so it
# is built against the exact Samba that Debian 13 ships now (security updates
# included) and depends on exactly that version. Its version names both:
# <smoothfs>+samba<samba version>-1.
set -euo pipefail

what=${1:?dkms|samba-vfs} out=${2:?out-dir}
root=$(pwd)
version=$(cat version.txt)
mkdir -p "$out"
export DEBIAN_FRONTEND=noninteractive

case $what in
dkms)
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends debhelper dh-dkms dpkg-dev >/dev/null
    packaging/debian-changelog.sh src/smoothfs/debian smoothfs-dkms "${version}-1"
    (cd src/smoothfs && dpkg-buildpackage -us -uc -b)
    deb=$(ls src/smoothfs-dkms_*_all.deb)
    # Sanity: the source tree is where DKMS will look, and the maintainer
    # scripts run DKMS.
    dpkg-deb -c "$deb" | grep -q "usr/src/smoothfs-${version}/dkms.conf"
    dpkg-deb -c "$deb" | grep -q "usr/src/smoothfs-${version}/range_staging.c"
    dpkg-deb -e "$deb" "$out/control-check"
    grep -q dkms "$out/control-check/postinst"
    rm -rf "$out/control-check"
    mv "$deb" "$out/"
    ;;
samba-vfs)
    # Samba's source, as Debian 13 ships it now.
    sed -n 's/^Types: deb$/Types: deb deb-src/; p' /etc/apt/sources.list.d/debian.sources > /tmp/debian.sources
    mv /tmp/debian.sources /etc/apt/sources.list.d/debian.sources
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends samba debhelper dpkg-dev python3 >/dev/null
    apt-get build-dep -y -qq samba >/dev/null
    samba_full=$(dpkg-query -W -f='${Version}' samba)          # e.g. 2:4.22.8+dfsg-0+deb13u1
    samba_nover=${samba_full#*:}
    # Unpacks to /tmp/samba-<upstream version>, where build.sh looks.
    (cd /tmp && apt-get source -qq "samba=${samba_full}" >/dev/null)
    [ -d "/tmp/samba-${samba_nover%%-*}" ] || { echo "no Samba source at /tmp/samba-${samba_nover%%-*}" >&2; ls /tmp >&2; exit 1; }
    pkg_version="${version}+samba$(echo "$samba_nover" | tr -- '-' '.')-1"
    packaging/debian-changelog.sh src/smoothfs/samba-vfs/debian smoothfs-samba-vfs "$pkg_version"
    (cd src/smoothfs/samba-vfs && dpkg-buildpackage -us -uc -b)
    deb=$(ls src/smoothfs/smoothfs-samba-vfs_*.deb | head -1)
    dpkg-deb -f "$deb" Depends | grep -q "samba (= ${samba_full})"
    mv "$deb" "$out/"
    ;;
*) echo "unknown package set: $what" >&2; exit 2 ;;
esac
ls -l "$out"

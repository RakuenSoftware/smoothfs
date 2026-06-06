#!/bin/bash
# Fetch and install a >=6.18 mainline kernel (image + modules + headers)
# so a gh-runner worker — which shares the host's <6.18-incompatible
# kernel and has the production smoothfs loaded — can boot a clean guest
# kernel under virtme-ng/QEMU and load a build-under-test there.
#
# Prints the installed kernel version (uname-style) to stdout on the last
# line as "GUEST_KVER=<ver>"; logs go to stderr.
set -euo pipefail

VER="${SMOOTHFS_GUEST_KERNEL:-v6.18}"
ARCH=amd64
BASE="https://kernel.ubuntu.com/mainline/${VER}/${ARCH}"

log() { echo "[fetch-kernel] $*" >&2; }

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
cd "$workdir"

log "listing $BASE"
index="$(curl -fsSL "$BASE/")"

# Pick the generic flavour debs. There is one *_all.deb for the common
# headers plus per-flavour image/modules/headers. We want "generic".
pick() {
	# $1 = grep pattern for the deb filename
	echo "$index" | grep -oE 'href="[^"]+\.deb"' | sed -E 's/href="([^"]+)"/\1/' \
		| grep -E "$1" | grep -v -- '-dbg' | head -1
}

img="$(pick 'linux-image-unsigned-[0-9].*-generic_.*_'"$ARCH"'\.deb')"
mod="$(pick 'linux-modules-[0-9].*-generic_.*_'"$ARCH"'\.deb')"
hdr_all="$(pick 'linux-headers-[0-9][^_]*_.*_all\.deb')"
hdr_gen="$(pick 'linux-headers-[0-9].*-generic_.*_'"$ARCH"'\.deb')"

for v in img mod hdr_all hdr_gen; do
	if [ -z "${!v}" ]; then
		log "ERROR: could not find $v deb in $BASE"; exit 1
	fi
done

for f in "$hdr_all" "$hdr_gen" "$mod" "$img"; do
	log "downloading $f"
	curl -fsSL -o "$(basename "$f")" "$BASE/$f"
done

log "installing debs"
sudo dpkg -i ./*.deb >&2 2>&1 || sudo apt-get -f install -y >&2

# The installed kernel version = the modules directory just created.
kver="$(basename "$img" | sed -E 's/^linux-image-unsigned-([0-9][^_]*)-generic_.*/\1-generic/')"
if [ ! -d "/lib/modules/$kver" ]; then
	# fall back to newest /lib/modules entry
	kver="$(ls -1 /lib/modules | sort -V | tail -1)"
fi
test -e "/boot/vmlinuz-$kver"
test -d "/lib/modules/$kver/build"
log "installed guest kernel $kver"
echo "GUEST_KVER=$kver"

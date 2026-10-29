#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Write a debian/changelog whose version is the one being packaged.
#
#   packaging/debian-changelog.sh <debian-dir> <package> <version>
#
# release-please bumps version.txt and dkms.conf, not Debian changelogs, so
# the packaging steps write the changelog entry from the release version
# right before dpkg-buildpackage.
set -eu
dir=$1 pkg=$2 version=$3
cat > "$dir/changelog" <<EOF
$pkg ($version) trixie; urgency=medium

  * Release $version.

 -- RakuenSoftware <support@smoothnas.com>  $(date -R)
EOF

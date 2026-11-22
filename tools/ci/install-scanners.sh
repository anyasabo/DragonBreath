#!/bin/sh
# Install Grype for tools/sbom/sbom.sh scan in CI. The devspace installs the same version
# (dotfiles devspaces/prusa.sh). To update, change the version and the SHA-256 from
# grype_VERSION_checksums.txt of the release, here and in the devspace script.
set -eu
grype_ver=0.122.1
grype_sha=f1ba7e2917ee1395618de20a98da0aa7c0fb1788927797651047c50502a37538
dest=${1:-/usr/local/bin}
tmp=$(mktemp -d)
curl -fsSL -o "$tmp/grype.tar.gz" "https://github.com/anchore/grype/releases/download/v$grype_ver/grype_${grype_ver}_linux_amd64.tar.gz"
echo "$grype_sha  $tmp/grype.tar.gz" | sha256sum -c --quiet
tar -xzf "$tmp/grype.tar.gz" -C "$tmp" grype
install -m 0755 "$tmp/grype" "$dest/grype"
rm -rf "$tmp"
"$dest/grype" version | sed -n 's/^Version: */grype /p'

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Fetches a Linux release from kernel.org, verifies its SHA-256 against the
# published checksum list, and installs its UAPI headers for building dnuma.
#
#   scripts/kernel-headers.sh [version] [work-dir]
#
# Defaults: version 7.3.5, work dir /var/tmp/dnuma-kernel. The headers end up
# in <repo>/.deps/linux-<version>-headers/include; configure with
#   cmake -DDNUMA_KERNEL_HEADERS=<repo>/.deps/linux-<version>-headers/include
# Nothing on the running system is modified.
set -euo pipefail
ver="${1:-7.3.5}"
work="${2:-/var/tmp/dnuma-kernel}"
root="$(cd "$(dirname "$0")/.." && pwd)"
major="${ver%%.*}"
base="https://cdn.kernel.org/pub/linux/kernel/v${major}.x"
mkdir -p "$work" && cd "$work"
tarball="linux-$ver.tar.xz"
[ -f "$tarball" ] || { curl -fsS -o "$tarball.part" "$base/$tarball" && mv "$tarball.part" "$tarball"; }
sum=$(curl -fsS "$base/sha256sums.asc" | awk -v f="$tarball" '$2==f {print $1}')
[ -n "$sum" ] || { echo "no published checksum for $tarball" >&2; exit 1; }
echo "$sum  $tarball" | sha256sum -c -
[ -d "linux-$ver" ] || tar -xf "$tarball"
make -s -C "linux-$ver" headers_install ARCH=x86 INSTALL_HDR_PATH="$work/headers-$ver"
mkdir -p "$root/.deps"
rsync -a --delete "$work/headers-$ver/" "$root/.deps/linux-$ver-headers/"
echo "installed: $root/.deps/linux-$ver-headers/include"
echo "note: the checksum list was fetched over HTTPS; its PGP signature was not verified."

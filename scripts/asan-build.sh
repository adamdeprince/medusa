#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Configures and builds the AddressSanitizer tree (build-asan/).
#   scripts/asan-build.sh [ninja targets...]     (default: everything)
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
hdr="$root/.deps/linux-7.3.5-headers/include"
cmake -S "$root" -B "$root/build-asan" -G Ninja -DDNUMA_SANITIZE=address \
  $([ -d "$hdr" ] && echo "-DDNUMA_KERNEL_HEADERS=$hdr") >/dev/null || exit 1
out=$(cmake --build "$root/build-asan" -- "$@" 2>&1); rc=$?
[ $rc -eq 0 ] || echo "$out" | grep -E "error|FAILED" | head -20
exit $rc

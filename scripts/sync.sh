#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# For developing on one machine and building on another: copies the working
# tree to $DNUMA_BUILD_HOST:$DNUMA_BUILD_PATH and builds it there.
#   scripts/sync.sh [ninja targets...]
# Prints only compiler errors and warnings, then "build exit: <status>".
set -euo pipefail
. "$(dirname "$0")/env.sh"
dnuma_need DNUMA_BUILD_HOST DNUMA_BUILD_PATH
remote="$DNUMA_BUILD_HOST"; path="$DNUMA_BUILD_PATH"
rsync -a --delete --exclude .deps --exclude build --exclude build-asan --exclude .git "$DNUMA_ROOT/" "$remote:$path/"
# Newer kernel UAPI headers than the build host's own are used if they have
# been unpacked there (scripts/kernel-headers.sh).
ssh -o BatchMode=yes "$remote" "cd ~/'$path' && \
  hdr=\$PWD/.deps/linux-7.3.5-headers/include; \
  cmake -S . -B build -G Ninja \$([ -d \"\$hdr\" ] && echo -DDNUMA_KERNEL_HEADERS=\$hdr) >/dev/null && \
  cmake --build build -- -k 0 $* 2>&1 | grep -E 'error|FAILED: |warning: |undefined reference|ninja: (build stopped|no work)|^\[[0-9]+/[0-9]+\] Linking' | grep -v '^/usr/bin/c++' | cut -c1-260 | head -${DNUMA_BUILD_LINES:-60}; echo \"build exit: \${PIPESTATUS[0]}\""

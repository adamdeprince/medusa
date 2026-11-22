#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Boots a kernel built with CONFIG_DNUMA_VIEW in a virtual machine and runs
# the view tests in it (kernel/test/view-test-init.sh as init, plus the
# master's client test, tests/unit/test_kernel_view). Nothing is installed
# and the host's kernel is not involved.
#   kernel/test/run.sh <bzImage> [initramfs-to-reuse]
# Needs busybox (static), top and ps on the machine that builds the
# initramfs, and qemu-system-x86_64 where the VM runs. Set DNUMA_QEMU to run
# QEMU some other way, for example in a container:
#   DNUMA_QEMU="docker run --rm --device /dev/kvm -v $PWD:$PWD:ro some-qemu-image qemu-system-x86_64"
# Exit status 0 only if every check passed.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
kernel="$(readlink -f "${1:?usage: $0 <bzImage> [initramfs]}")"
initrd="${2:-}"
if [ -z "$initrd" ]; then
  initrd="$(mktemp --suffix=.cpio.gz)"; trap 'rm -f "$initrd"' EXIT
  bash "$here/mkinitrd.sh" "$initrd" >/dev/null
fi
initrd="$(readlink -f "$initrd")"
accel="-cpu max"; [ -w /dev/kvm ] && accel="-enable-kvm -cpu host"
log="$(mktemp)"
timeout 600 ${DNUMA_QEMU:-qemu-system-x86_64} $accel -smp 4 -m 3072 -nographic -no-reboot \
  -kernel "$kernel" -initrd "$initrd" -append "console=ttyS0 panic=-1 quiet dnuma.test_native_pager=${DNUMA_TEST_NATIVE_PAGER:-0}" 2>&1 | tr -d '\r' > "$log" || true
sed -n '/KTEST begin/,$p' "$log"
grep -q "KTEST end: 0 failure" "$log" && ! grep -qE "BUG:|WARNING:|KASAN|Oops|sleeping function|suspicious RCU|circular locking" "$log"
rc=$?; rm -f "$log"; exit $rc

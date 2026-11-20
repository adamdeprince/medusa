#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Boots another kernel once, without touching the boot loader: the kernel is
# loaded with kexec, and the next ordinary reboot or power cycle returns to
# whatever the boot loader starts by default. Useful for trying a kernel
# built with CONFIG_DNUMA_VIEW on the master.
#
#   sudo scripts/kexec-kernel.sh <kernel image> <initramfs>          load only
#   sudo scripts/kexec-kernel.sh <kernel image> <initramfs> --now    load, then shut down cleanly into it
#   sudo scripts/kexec-kernel.sh --unload
#
# The new kernel gets the running kernel's command line (without BOOT_IMAGE).
# Its modules must already be in /lib/modules/<release>. After "load only",
# `systemctl kexec` performs the switch.
#
# Uses kexec(8) if installed; otherwise calls kexec_file_load(2) directly,
# which this script knows how to do on x86-64 only.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 1; }
if [ "${1:-}" = --unload ]; then
  if command -v kexec >/dev/null; then kexec -u
  else python3 -c 'import ctypes,os,sys; l=ctypes.CDLL(None,use_errno=True); r=l.syscall(320,-1,-1,0,None,1); sys.exit(0 if r==0 else os.strerror(ctypes.get_errno()))'; fi
  echo "unloaded"; exit 0
fi
kernel="${1:?usage: $0 <kernel image> <initramfs> [--now]}"; initrd="${2:?usage: $0 <kernel image> <initramfs> [--now]}"
[ -r "$kernel" ] && [ -r "$initrd" ] || { echo "cannot read $kernel or $initrd" >&2; exit 1; }
cmdline="$(tr ' ' '\n' < /proc/cmdline | grep -v '^BOOT_IMAGE=' | tr '\n' ' ' | sed 's/ *$//')"
if command -v kexec >/dev/null; then
  kexec -s -l "$kernel" --initrd="$initrd" --command-line="$cmdline"
elif [ "$(uname -m)" = x86_64 ]; then
  python3 - "$kernel" "$initrd" "$cmdline" <<'PY'
import ctypes, os, sys
kernel, initrd, cmdline = sys.argv[1], sys.argv[2], sys.argv[3].encode() + b"\0"
libc = ctypes.CDLL(None, use_errno=True)
kfd, ifd = os.open(kernel, os.O_RDONLY), os.open(initrd, os.O_RDONLY)
SYS_kexec_file_load = 320  # x86-64
if libc.syscall(SYS_kexec_file_load, kfd, ifd, ctypes.c_ulong(len(cmdline)), ctypes.c_char_p(cmdline), ctypes.c_ulong(0)) != 0:
    sys.exit("kexec_file_load: " + os.strerror(ctypes.get_errno()))
PY
else
  echo "kexec(8) is not installed and this script cannot call kexec_file_load on $(uname -m)" >&2; exit 1
fi
[ "$(cat /sys/kernel/kexec_loaded)" = 1 ] || { echo "the kernel did not accept the image" >&2; exit 1; }
echo "loaded $kernel"
echo "command line: $cmdline"
if [ "${3:-}" = --now ]; then
  echo "switching to it now (clean shutdown of all services first)"
  systemctl kexec
else
  echo "run 'systemctl kexec' to switch to it, or '$0 --unload' to forget it"
fi

#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Builds a small initramfs around the test script: static busybox, plus the
# host's own top and ps with their libraries.
set -e
out=$1; here=$(cd "$(dirname "$0")" && pwd)
d=$(mktemp -d); mkdir -p $d/bin $d/proc $d/sys $d/dev $d/tmp $d/etc $d/usr/share/terminfo/d
cp /usr/bin/busybox $d/bin/busybox
for a in sh mount cat grep awk sed sleep ls dd md5sum wc printf kill touch uname dmesg sync poweroff tr seq head; do ln -s busybox $d/bin/$a; done
cp /usr/bin/top /usr/bin/ps $d/bin/
client=${DNUMA_BUILD:-$here/../../build}/tests/test_kernel_view
cp $client $d/bin/
for b in /usr/bin/top /usr/bin/ps $client; do
  ldd $b | awk '$3 ~ /^\// {print $3} $1 ~ /^\/lib/ {print $1}' | while read l; do mkdir -p $d$(dirname $l); cp -L $l $d$l; done
done
cp /usr/share/terminfo/d/dumb $d/usr/share/terminfo/d/ 2>/dev/null || cp /lib/terminfo/d/dumb $d/usr/share/terminfo/d/
echo "root:x:0:0:root:/:/bin/sh" > $d/etc/passwd
cp $here/view-test-init.sh $d/init; chmod +x $d/init
(cd $d && find . | cpio -o -H newc 2>/dev/null | gzip -1) > $out
rm -rf $d; ls -la $out

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Like `cluster.sh start`, but runs AddressSanitizer builds of both daemons
# (build-asan/). ASan reports land in the daemon log files.
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
"$root/scripts/asan-build.sh" dnuma-master dnuma-agent || exit 1
"$root/scripts/cluster.sh" stop; "$root/scripts/cluster.sh" clean-logs
key="$root/.deps/cluster.key"
. "$root/scripts/env.sh"
dnuma_need DNUMA_SLAVES
addr="${DNUMA_SLAVE_ADDR:-$(set -- $DNUMA_SLAVES; echo "${1#*=}")}"
sudo -n mkdir -p /run/dnuma
sudo -n sh -c "ASAN_OPTIONS=detect_leaks=0 nohup setsid $root/build-asan/dnuma-master --key-file $key --log-file /tmp/dnuma-master.log ${DNUMA_MASTER_ARGS:-} >/dev/null 2>>/tmp/dnuma-master.log &"
ssh -o BatchMode=yes "$addr" "sudo -n sh -c 'ASAN_OPTIONS=detect_leaks=0 nohup setsid $root/build-asan/dnuma-agent --key-file $key --log-file /tmp/dnuma-agent.log ${DNUMA_AGENT_ARGS:-} >/dev/null 2>>/tmp/dnuma-agent.log &'"
for i in $(seq 100); do [ -S /run/dnuma/master.sock ] && (exec 3<>/dev/tcp/$addr/${DNUMA_AGENT_PORT:-7710}) 2>/dev/null && break; sleep 0.1; done
sudo -n chmod 666 /run/dnuma/master.sock

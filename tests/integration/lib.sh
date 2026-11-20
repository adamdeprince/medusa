# SPDX-License-Identifier: GPL-2.0-only
# Shared helpers for the integration and failure tests. Source this file.
#
# The tests run on the master host and reach one slave over ssh. Which slave
# is site configuration (environment or .dnuma-env, see
# scripts/dnuma-env.example):
#   DNUMA_SLAVES            name=address pairs; the first one is the default slave
#   DNUMA_SLAVE             administrative name of the slave under test
#   DNUMA_SLAVE_ADDR        its address for ssh and management
#   DNUMA_SLAVE_RDMA_ADDR   its RDMA address, if different
#   DNUMA_UNREACHABLE_ADDR  an address nothing answers on
#   DNUMA_ASAN=1            run AddressSanitizer builds of the daemons
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$ROOT/scripts/env.sh"
BIN="$ROOT/build"
CTL="$BIN/dnumactl"
RUN="$BIN/dnuma-run"
T="$BIN/tests"
dnuma_need DNUMA_SLAVES
_first="$(set -- $DNUMA_SLAVES; echo "$1")"
SLAVE="${DNUMA_SLAVE:-${_first%%=*}}"
SLAVE_ADDR="${DNUMA_SLAVE_ADDR:-${_first#*=}}"
SLAVE_RDMA_ADDR="${DNUMA_SLAVE_RDMA_ADDR:-$SLAVE_ADDR}"
# 192.0.2.0/24 is reserved for documentation and never assigned to a host.
UNREACHABLE_ADDR="${DNUMA_UNREACHABLE_ADDR:-192.0.2.1}"
AGENT_PORT="${DNUMA_AGENT_PORT:-7710}"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8"
MLOG=/tmp/dnuma-master.log
ALOG=/tmp/dnuma-agent.log
OUT="${TMPDIR:-/tmp}/dnuma-test.$$"
mkdir -p "$OUT"
FAILS=0
CHECKS=0

say() { printf '%s\n' "$*"; }
ok() { CHECKS=$((CHECKS + 1)); say "  ok    $*"; }
bad() { CHECKS=$((CHECKS + 1)); FAILS=$((FAILS + 1)); say "  FAIL  $*"; }
# check "<description>" <command...>   passes when the command succeeds
check() { local d="$1"; shift; if "$@" >/dev/null 2>&1; then ok "$d"; else bad "$d"; fi; }
# expect "<description>" "<actual>" "<expected>"
expect() { if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1: got '$2', expected '$3'"; fi; }
contains() { case "$2" in *"$3"*) ok "$1";; *) bad "$1: '$3' not found";; esac; }

slave() { $SSH "$SLAVE_ADDR" "$@"; }
# NUMA nodes (kernel numbers) the slave's agent offers, one per line: those
# named by --nodes in DNUMA_AGENT_ARGS, otherwise all the slave has.
slave_offered_nodes() {
  local list
  list=$(set -- ${DNUMA_AGENT_ARGS:-}; while [ $# -gt 1 ]; do [ "$1" = --nodes ] && { echo "$2"; break; }; shift; done)
  if [ -n "$list" ]; then echo "$list" | tr ',' '\n' | while IFS=- read -r a b; do seq "$a" "${b:-$a}"; done
  else slave "ls -d /sys/devices/system/node/node[0-9]* | sed 's/.*node//'"; fi
}
# Number of CPUs in those nodes.
slave_offered_cpus() {
  local total=0 node
  for node in $(slave_offered_nodes); do
    total=$((total + $(slave "cat /sys/devices/system/node/node$node/cpulist" | tr ',' '\n' | awk -F- '{s += ($2 == "" ? 1 : $2 - $1 + 1)} END {print s + 0}')))
  done
  echo $total
}

start_cluster() { # [master args...]
  if [ "${DNUMA_ASAN:-0}" = 1 ]; then
    DNUMA_MASTER_ARGS="$* ${DNUMA_MASTER_ARGS:-}" "$ROOT/scripts/asan-cluster.sh" || return 1
  else
    "$ROOT/scripts/cluster.sh" stop >/dev/null 2>&1
    "$ROOT/scripts/cluster.sh" clean-logs
    DNUMA_MASTER_ARGS="$* ${DNUMA_MASTER_ARGS:-}" "$ROOT/scripts/cluster.sh" start || return 1
  fi
}
stop_cluster() { "$ROOT/scripts/cluster.sh" stop >/dev/null 2>&1; }
start_agent() { # restart only the slave's agent
  local bin="$BIN"; [ "${DNUMA_ASAN:-0}" = 1 ] && bin="$ROOT/build-asan"
  slave "sudo -n sh -c 'ASAN_OPTIONS=detect_leaks=0 nohup setsid $bin/dnuma-agent --key-file $ROOT/.deps/cluster.key --log-file $ALOG ${DNUMA_AGENT_ARGS:-} >/dev/null 2>>$ALOG &'"
  for i in $(seq 100); do (exec 3<>/dev/tcp/$SLAVE_ADDR/$AGENT_PORT) 2>/dev/null && return 0; sleep 0.1; done
  return 1
}
attach() { $CTL slave connect "$SLAVE" --address "$SLAVE_ADDR"; }
slave_state() { $CTL slave list | awk -v n="$SLAVE" '$1==n {print $2}'; }
slave_inc() { $CTL slave list | awk -v n="$SLAVE" '$1==n {print $NF}'; }
# wait_state <state> <seconds>
wait_state() { for i in $(seq $(( $2 * 10 ))); do [ "$(slave_state)" = "$1" ] && return 0; sleep 0.1; done; return 1; }
# wait_line <file> <pattern> <seconds>
wait_line() { for i in $(seq $(( $3 * 10 ))); do grep -q -E "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
first_slave_node() { $CTL topology | awk -v n="$SLAVE" '$3==n && $NF=="ACTIVE" {print $1; exit}'; }
# Fails the test if either daemon logged a sanitizer report.
no_sanitizer_reports() {
  local n; n=$(grep -c "AddressSanitizer" $MLOG 2>/dev/null); n=${n:-0}
  local m; m=$(slave "grep -c AddressSanitizer $ALOG 2>/dev/null"); m=${m:-0}
  [ "$n" = 0 ] && [ "$m" = 0 ]
}
finish_test() {
  [ "${DNUMA_ASAN:-0}" = 1 ] && check "no sanitizer reports in either daemon" no_sanitizer_reports
  rm -rf "$OUT"
  if [ $FAILS -eq 0 ]; then say "PASS $(basename "$0") ($CHECKS checks)"; exit 0; fi
  say "FAIL $(basename "$0") ($FAILS of $CHECKS checks failed)"; exit 1
}
# wait_line_remote <pattern> <seconds>: waits for a line in the slave's agent log.
wait_line_remote() { for i in $(seq $(( $2 * 2 ))); do slave "grep -q -E '$1' $ALOG" && return 0; sleep 0.5; done; return 1; }

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Starts/stops the dnuma daemons for development and testing.
#   scripts/cluster.sh start|stop|status|logs [master-args...]
# Run it on the master host. An agent is started over ssh on every slave
# listed in DNUMA_SLAVES; the build directory must be visible there under the
# same path (a shared file system, or a copy).
# Configuration (environment or .dnuma-env, see scripts/dnuma-env.example):
#   DNUMA_SLAVES       space-separated name=address pairs; the address is used
#                      for ssh and as the agent's management address
#   DNUMA_MASTER_ARGS, DNUMA_AGENT_ARGS   extra daemon arguments
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
. "$root/scripts/env.sh"
dnuma_need DNUMA_SLAVES
bin="$root/build"
key="$root/.deps/cluster.key"
slaves="$DNUMA_SLAVES"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8"
mlog=/tmp/dnuma-master.log
alog=/tmp/dnuma-agent.log

ensure_key() {
  [ -f "$key" ] || { (umask 077; head -c 48 /dev/urandom | base64 -w0 > "$key"); }
}

case "${1:-}" in
start)
  shift
  ensure_key
  sudo -n mkdir -p /run/dnuma
  sudo -n sh -c "nohup setsid $bin/dnuma-master --key-file $key --log-file $mlog ${DNUMA_MASTER_ARGS:-} $* >/dev/null 2>>$mlog &"
  for s in $slaves; do
    $SSH "${s#*=}" "sudo -n sh -c 'nohup setsid $bin/dnuma-agent --key-file $key --log-file $alog ${DNUMA_AGENT_ARGS:-} >/dev/null 2>>$alog &'"
  done
  for i in $(seq 50); do [ -S /run/dnuma/master.sock ] && break; sleep 0.1; done
  [ -S /run/dnuma/master.sock ] || { echo "master did not start:"; tail -5 $mlog; exit 1; }
  sudo -n chmod 666 /run/dnuma/master.sock
  # Wait until every agent accepts management connections.
  for s in $slaves; do
    ok=0
    for i in $(seq 100); do (exec 3<>/dev/tcp/${s#*=}/${DNUMA_AGENT_PORT:-7710}) 2>/dev/null && { ok=1; break; }; sleep 0.1; done
    [ $ok = 1 ] || { echo "agent on ${s%%=*} did not start:"; $SSH "${s#*=}" "tail -5 $alog"; exit 1; }
  done
  ;;
stop)
  sudo -n pkill -TERM -x dnuma-master 2>/dev/null
  for s in $slaves; do $SSH "${s#*=}" "sudo -n pkill -TERM -x dnuma-agent 2>/dev/null"; done
  for i in $(seq 50); do pgrep -x dnuma-master >/dev/null || break; sleep 0.1; done
  sudo -n pkill -KILL -x dnuma-master 2>/dev/null
  for s in $slaves; do $SSH "${s#*=}" "sleep 0.3; sudo -n pkill -KILL -x dnuma-agent 2>/dev/null"; done
  true
  ;;
status)
  pgrep -a -x dnuma-master || echo "master: not running"
  for s in $slaves; do $SSH "${s#*=}" "pgrep -a -x dnuma-agent || echo '${s%%=*} agent: not running'"; done
  ;;
logs)
  echo "== master =="; tail -${2:-30} $mlog
  for s in $slaves; do echo "== agent ${s%%=*} =="; $SSH "${s#*=}" "tail -${2:-30} $alog"; done
  ;;
clean-logs)
  sudo -n rm -f $mlog; for s in $slaves; do $SSH "${s#*=}" "sudo -n rm -f $alog"; done
  ;;
*)
  echo "usage: $0 start|stop|status|logs [n]|clean-logs"; exit 2;;
esac

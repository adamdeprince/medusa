#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Unexpected failure: the agent process is killed (this also models a slave
# reboot: the agent and everything it hosts vanish and a new agent starts
# later). Dependent processes die, unrelated ones do not, and the master
# does not claim the slave is safe before it can know that.
. "$(dirname "$0")/../integration/lib.sh"
start_cluster || { say "cannot start cluster"; exit 1; }
attach >/dev/null || { say "attach failed"; exit 1; }
node=$(first_slave_node)

timeout 120 $RUN $T/t_worker 0 8 8 1 > "$OUT/local.out" 2>&1 &
lpid=$!
timeout 120 $RUN $T/t_worker "$node" 16 60 2 > "$OUT/dep.out" 2>&1 &
dpid=$!
check "dependent application started on the slave" wait_line "$OUT/dep.out" "worker0-on $SLAVE" 20
check "unrelated application started" wait_line "$OUT/local.out" "worker0-on $(hostname)" 20

slave "sudo -n pkill -KILL -x dnuma-agent"
t0=$(date +%s.%N)
wait $dpid; rc=$?
t1=$(date +%s.%N)
expect "dependent application was killed (128+9)" "$rc" "137"
say "  (killed $(awk -v a="$t0" -v b="$t1" 'BEGIN {printf "%.2f", b-a}') s after the agent died)"
st=$(slave_state)
check "master does not report the slave as cleanly gone yet (state $st)" test "$st" = "FENCE_INCOMPLETE" -o "$st" = "FENCING"
contains "status says the remote side is unproven" "$($CTL slave status "$SLAVE")" "not proven stopped"
out=$($CTL slave connect ghost2 --address "$UNREACHABLE_ADDR" 2>&1)
expect "master still serves other requests meanwhile" "$($CTL slave list | grep -c MASTER)" "1"
# Images must not outlive their agent.
expect "no image survived on the slave" "$(slave "pgrep -c -x t_worker || true")" "0"
check "state becomes DISCONNECTED once the lease has run out" wait_state DISCONNECTED 12
contains "proof recorded is lease expiry" "$($CTL slave status "$SLAVE")" "lease-expired"

check "unrelated application still running" kill -0 $lpid
wait $lpid; rc=$?
expect "unrelated application exit code" "$rc" "0"
check "unrelated application data intact" grep -q "^RESULT PASS" "$OUT/local.out"

# "Reboot": a new agent process starts with the same persistent identity.
inc_old=$(slave_inc)
check "agent restarts" start_agent
attach > "$OUT/attach.out"
expect "slave reattaches after the restart" "$(slave_state)" "ACTIVE"
check "newer incarnation" test "$(slave_inc)" -gt "$inc_old"
expect "same logical node numbers (identity survived the restart)" "$(first_slave_node)" "$node"
timeout 30 $RUN "$BIN/examples/dnuma-hello" "$node" > /dev/null 2>&1
expect "restarted slave runs new work" "$?" "0"
stop_cluster
finish_test

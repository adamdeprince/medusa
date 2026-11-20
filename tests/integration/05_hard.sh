#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Hard disconnect: processes that depend on the slave are killed, everything
# else survives, stale ownership is gone, and the slave comes back only as a
# new incarnation.
. "$(dirname "$0")/lib.sh"
start_cluster || { say "cannot start cluster"; exit 1; }
attach >/dev/null || { say "attach failed"; exit 1; }
node=$(first_slave_node)

timeout 120 $RUN $T/t_worker 0 16 9 2 > "$OUT/local.out" 2>&1 &
lpid=$!
timeout 120 $RUN $T/t_worker "$node" 32 60 2 > "$OUT/dep.out" 2>&1 &
dpid=$!
check "unrelated application started (master only)" wait_line "$OUT/local.out" "worker0-on $(hostname)" 20
check "dependent application started on the slave" wait_line "$OUT/dep.out" "worker0-on $SLAVE" 20
sleep 1
dep_native=$(awk '/^STARTED/ {print $3}' "$OUT/dep.out")
local_native=$(awk '/^STARTED/ {print $3}' "$OUT/local.out")

preview=$($CTL slave disconnect "$SLAVE" --hard --dry-run); rc=$?
say "$preview" | sed 's/^/      /'
expect "dry run exit code" "$rc" "0"
contains "dry run lists the dependent process" "$preview" "PID $dep_native"
expect "dry run does not list the unrelated process" "$(echo "$preview" | grep -c "PID $local_native ")" "0"
contains "dry run counts one process" "$preview" "Processes to terminate: 1"
expect "dry run changed nothing" "$(slave_state)" "ACTIVE"
check "dependent application still alive after dry run" kill -0 $dpid

out=$($CTL slave disconnect "$SLAVE" --hard < /dev/null 2>&1); rc=$?
expect "hard disconnect without --yes is refused non-interactively" "$rc" "2"
expect "refusal changed nothing" "$(slave_state)" "ACTIVE"

inc_old=$(slave_inc)
$CTL slave disconnect "$SLAVE" --hard --yes > "$OUT/hard.out" 2>&1; rc=$?
sed 's/^/      /' "$OUT/hard.out"
expect "hard disconnect exit code" "$rc" "0"
check "report lists the killed process" grep -q "PID $dep_native" "$OUT/hard.out"
check "fencing confirmed by the agent" grep -q "Fencing: CONFIRMED" "$OUT/hard.out"
expect "slave state" "$(slave_state)" "DISCONNECTED"

wait $dpid; rc=$?
expect "dependent application died from SIGKILL (128+9)" "$rc" "137"
expect "its image on the slave is gone" "$(slave "pgrep -c -x t_worker || true")" "0"
expect "its ownership metadata is gone" "$($CTL process list | sed '/Recent forced/,$d' | grep -c " $dep_native ")" "0"
expect "nothing is recorded as resident on the slave" "$($CTL memory status | awk -v n="$SLAVE" '$1==n {print $4 $5}')" "0B"
contains "termination is recorded" "$($CTL process list)" "Recent forced terminations"

check "unrelated application is still running" kill -0 $lpid
wait $lpid; rc=$?
tail -1 "$OUT/local.out" | sed 's/^/      /'
expect "unrelated application exit code" "$rc" "0"
check "unrelated application finished with intact data" grep -q "^RESULT PASS" "$OUT/local.out"

expect "repeating the hard disconnect is a no-op" "$($CTL slave disconnect "$SLAVE" --hard --yes | grep -c 'already DISCONNECTED')" "1"
attach >/dev/null
expect "slave reattaches" "$(slave_state)" "ACTIVE"
check "with a newer incarnation" test "$(slave_inc)" -gt "$inc_old"
timeout 30 $RUN "$BIN/examples/dnuma-hello" "$node" > "$OUT/hello.out" 2>&1
expect "the reattached slave runs new work" "$?" "0"
stop_cluster
finish_test

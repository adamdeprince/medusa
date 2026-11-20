#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Graceful disconnect: while an application has threads and memory on the
# slave, drain it. Execution must move off the slave, every modified page
# must be evacuated, and the application must keep running and finish with
# correct data.
. "$(dirname "$0")/lib.sh"
start_cluster || { say "cannot start cluster"; exit 1; }
attach >/dev/null || { say "attach failed"; exit 1; }
node=$(first_slave_node)

timeout 120 $RUN $T/t_worker "$node" 64 14 2 > "$OUT/w.out" 2>&1 &
wpid=$!
check "application started on the slave" wait_line "$OUT/w.out" "worker0-on $SLAVE" 20
sleep 1.5
before=$($CTL memory status --slave "$SLAVE")
say "$before" | sed -n '1,2p;/PID/,$p' | sed 's/^/      /'
resident_before=$($CTL slave status "$SLAVE" | awk '/^Usage:/ {gsub(",","",$2); print $2}')
check "memory is resident on the slave before the drain ($resident_before pages)" test "${resident_before:-0}" -gt 1000

$CTL slave disconnect "$SLAVE" --drain --timeout 60s > "$OUT/drain.out" 2>&1; rc=$?
sed 's/^/      /' "$OUT/drain.out" | tail -14
expect "drain exit code" "$rc" "0"
check "progress was reported while draining" grep -q "Pages remaining" "$OUT/drain.out"
check "drain reports DETACHED" grep -q "DETACHED after graceful drain" "$OUT/drain.out"
expect "slave state after drain" "$(slave_state)" "DISCONNECTED"
contains "detach was clean" "$($CTL slave status "$SLAVE")" "clean-detach"
expect "no page left on the slave" "$($CTL memory status | awk -v n="$SLAVE" '$1==n {print $4 $5}')" "0B"
expect "no application image left on the slave" "$(slave "pgrep -c -x t_worker || true")" "0"

# New work must not be placed on the drained slave, and must not be
# silently run somewhere else instead.
timeout 20 $RUN "$BIN/examples/dnuma-hello" "$node" > "$OUT/hello.out" 2>&1; rc=$?
check "placement on the detached slave is refused, not substituted" test "$rc" -ne 0
contains "refusal explains why" "$(cat "$OUT/hello.out")" "not attached"

# The application is still alive and still making progress, now on the master.
check "application survived the drain" kill -0 $wpid
t_drain=$(grep -c TICK "$OUT/w.out")
sleep 2
check "application keeps making progress after the drain" test "$(grep -c TICK "$OUT/w.out")" -gt "$t_drain"
wait $wpid; rc=$?
tail -3 "$OUT/w.out" | sed 's/^/      /'
expect "application exit code" "$rc" "0"
check "application data is intact (every word equals its pass count)" grep -q "^RESULT PASS" "$OUT/w.out"
final=$(grep "^FINAL" "$OUT/w.out")
contains "execution finished on the master" "$final" "last-host $(hostname)"
contains "data ended up on the master" "$final" "data-owner $(hostname)"
check "execution contexts were migrated, not restarted" test "$(echo "$final" | awk '{print $5}')" -ge 2
check "passes continued after migration" test "$(grep TICK "$OUT/w.out" | grep -c "worker0-on $(hostname)")" -ge 2

# Reattach: fresh incarnation, same node numbers.
inc_old=$(slave_inc)
attach > /dev/null
expect "slave reattaches" "$(slave_state)" "ACTIVE"
check "reattach uses a newer incarnation" test "$(slave_inc)" -gt "$inc_old"
expect "logical node numbers are stable across reattach" "$(first_slave_node)" "$node"
stop_cluster
finish_test

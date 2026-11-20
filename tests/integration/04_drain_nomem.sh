#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# A drain that cannot fit the slave's data anywhere else must fail safely:
# precise reason, slave left attached, nothing killed, nothing lost - and it
# must never turn into a hard disconnect.
. "$(dirname "$0")/lib.sh"
start_cluster --capacity 16M || { say "cannot start cluster"; exit 1; }
attach >/dev/null || { say "attach failed"; exit 1; }
node=$(first_slave_node)

timeout 120 $RUN $T/t_worker "$node" 64 9 2 > "$OUT/w.out" 2>&1 &
wpid=$!
check "application started on the slave" wait_line "$OUT/w.out" "worker0-on $SLAVE" 20
sleep 1.5
$CTL slave disconnect "$SLAVE" --drain --timeout 30s > "$OUT/drain.out" 2>&1; rc=$?
sed 's/^/      /' "$OUT/drain.out" | tail -6
expect "drain exit code (failure)" "$rc" "1"
check "reason is insufficient memory" grep -q "insufficient memory elsewhere" "$OUT/drain.out"
check "report says nothing was terminated or discarded" grep -q "No process was terminated and no page was discarded" "$OUT/drain.out"
expect "slave is still attached" "$(slave_state)" "ACTIVE"
expect "nothing was force-terminated" "$($CTL metrics | grep -c forced_process_terminations)" "0"
check "application is still running" kill -0 $wpid
wait $wpid; rc=$?
tail -2 "$OUT/w.out" | sed 's/^/      /'
expect "application exit code" "$rc" "0"
check "application data is intact" grep -q "^RESULT PASS" "$OUT/w.out"
contains "application stayed on the slave" "$(grep '^FINAL' "$OUT/w.out")" "last-host $SLAVE"

# A drain that times out waiting for a context that never reaches a
# migration point is reported with the exact blocker, and also leaves the
# slave attached.
timeout 60 $RUN $T/t_worker "$node" 4 8 1 nomigrate > "$OUT/w2.out" 2>&1 &
w2=$!
check "second application (no migration points) started" wait_line "$OUT/w2.out" "worker0-on $SLAVE" 20
$CTL slave disconnect "$SLAVE" --drain --timeout 2s > "$OUT/drain2.out" 2>&1; rc=$?
tail -4 "$OUT/drain2.out" | sed 's/^/      /'
expect "timed-out drain exit code" "$rc" "1"
check "blocker names the stuck execution context" grep -q "execution context(s) have not reached a migration point" "$OUT/drain2.out"
expect "slave is still attached after the timeout" "$(slave_state)" "ACTIVE"
wait $w2; rc=$?
expect "that application also finished normally" "$rc" "0"
check "its data is intact" grep -q "^RESULT PASS" "$OUT/w2.out"
expect "still nothing force-terminated" "$($CTL metrics | grep -c forced_process_terminations)" "0"
stop_cluster
finish_test

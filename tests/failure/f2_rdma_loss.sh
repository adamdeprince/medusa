#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# RDMA connection loss with the management channel still up, and a
# disconnect that lands in the middle of page-ownership transfers.
. "$(dirname "$0")/../integration/lib.sh"
start_cluster || { say "cannot start cluster"; exit 1; }
attach >/dev/null || { say "attach failed"; exit 1; }
node=$(first_slave_node)

# --- 1. the slave's RDMA link dies under a dependent application ---
timeout 120 $RUN $T/t_worker "$node" 16 60 2 > "$OUT/dep.out" 2>&1 &
dpid=$!
check "dependent application started on the slave" wait_line "$OUT/dep.out" "worker0-on $SLAVE" 20
$CTL fault "$SLAVE" drop-rdma > "$OUT/fault.out" 2>&1
wait $dpid; rc=$?
expect "dependent application was killed (128+9)" "$rc" "137"
check "slave ends up DISCONNECTED" wait_state DISCONNECTED 12
contains "failure cause recorded" "$($CTL slave status "$SLAVE")" "RDMA link lost"
contains "agent confirmed the fence over the management channel" "$($CTL slave status "$SLAVE")" "agent-confirmed"
expect "no image left on the slave" "$(slave "pgrep -c -x t_worker || true")" "0"
attach >/dev/null
expect "slave reattaches" "$(slave_state)" "ACTIVE"

# --- 2. hard disconnect while pages are in transit to the slave ---
# The process does not depend on the slave (no thread there, no page there
# yet), so it must survive and keep every byte.
$CTL fault "$SLAVE" pull-delay 4000 > /dev/null
timeout 60 $RUN $T/t_migrate_abort "$node" 4 > "$OUT/mig.out" 2>&1 &
mpid=$!
check "migration started" wait_line "$OUT/mig.out" "^STARTED" 20
sleep 1
inflight=$($CTL memory status | awk -v n="$SLAVE" '$1==n {print $10 $11}')
check "transfers are in flight to the slave ($inflight)" test "$inflight" != "0B"
$CTL slave disconnect "$SLAVE" --hard --yes > "$OUT/hard.out" 2>&1
sed 's/^/      /' "$OUT/hard.out" | sed -n '1,8p'
check "the in-transit process is not on the kill list" grep -q "Processes terminated: 0" "$OUT/hard.out"
wait $mpid; rc=$?
sed 's/^/      /' "$OUT/mig.out"
expect "process survived the interrupted transfer" "$rc" "0"
check "all of its data is intact" grep -q "^RESULT PASS" "$OUT/mig.out"
check "the migration call reported the failure" grep -q "^MIGRATE rc=-" "$OUT/mig.out"
contains "ownership stayed on the master" "$(grep '^FINAL' "$OUT/mig.out")" "owner=$(hostname)"
check "aborted transfers were counted" test "$($CTL metrics | awk '$1=="page_transfer_failures" {gsub(",","",$2); print $2}')" -gt 0

attach >/dev/null
expect "slave reattaches again" "$(slave_state)" "ACTIVE"
stop_cluster
finish_test

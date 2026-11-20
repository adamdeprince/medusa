#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Network partition: the slave goes silent on every channel without anyone
# being told (no TCP reset, no RDMA disconnect). The master must detect it by
# lease expiry, must not treat the unconfirmed slave as safe, and the cut-off
# incarnation must be unable to use the RDMA credentials it still holds.
. "$(dirname "$0")/../integration/lib.sh"
start_cluster || { say "cannot start cluster"; exit 1; }
attach >/dev/null || { say "attach failed"; exit 1; }
node=$(first_slave_node)

timeout 120 $RUN $T/t_worker "$node" 16 60 2 > "$OUT/dep.out" 2>&1 &
dpid=$!
check "dependent application started on the slave" wait_line "$OUT/dep.out" "worker0-on $SLAVE" 20
inc_old=$(slave_inc)
$CTL fault "$SLAVE" partition 11000 > "$OUT/fault.out" 2>&1
t0=$(date +%s)
wait $dpid; rc=$?
expect "dependent application was killed (128+9)" "$rc" "137"
say "  (failure detected $(( $(date +%s) - t0 )) s after the partition began; lease is 5 s)"
contains "detected through lease expiry" "$($CTL slave status "$SLAVE")" "lease expired"
st=$(slave_state)
check "slave is reported as not yet safe (state $st)" test "$st" = "FENCE_INCOMPLETE" -o "$st" = "FENCING"
check "master only declares it DISCONNECTED after the lease bound" wait_state DISCONNECTED 12
contains "proof is lease expiry, not an acknowledgement" "$($CTL slave status "$SLAVE")" "lease-expired"

# On the slave, the agent's own lease expiry must have destroyed the images
# even though no message from the master could reach it.
check "the isolated agent fenced itself" slave "grep -q 'lease expired without renewal' $ALOG"
expect "no image survived on the isolated slave" "$(slave "pgrep -c -x t_worker || true")" "0"

# The stale incarnation now tries to read master memory with the queue pair
# and rkey it was given before the partition.
# (The HCA retries a request to a vanished queue pair 7 times with the
# path's acknowledgement timeout before giving up: about half a minute here.)
check "stale-credential probe completed on the slave" wait_line_remote "STALE-ACCESS-PROBE read:" 90
probe=$(slave "grep 'STALE-ACCESS-PROBE' $ALOG")
say "$probe" | sed 's/^.*STALE/      STALE/'
contains "stale RDMA access was denied" "$probe" "DENIED"
expect "stale RDMA access never succeeded" "$(echo "$probe" | grep -c SUCCEEDED)" "0"

sleep 1
attach > /dev/null
expect "slave reattaches after the partition heals" "$(slave_state)" "ACTIVE"
check "as a newer incarnation" test "$(slave_inc)" -gt "$inc_old"
timeout 30 $RUN "$BIN/examples/dnuma-hello" "$node" > /dev/null 2>&1
expect "and runs new work" "$?" "0"
stop_cluster
finish_test

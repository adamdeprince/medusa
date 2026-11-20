#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The end-to-end demonstration (section 18 of the project brief), run on the
# master host. Every step prints what the system itself reports.
#
#   examples/demo.sh            # this host = master; the slave comes from
#                               # DNUMA_SLAVES (see scripts/dnuma-env.example)
. "$(dirname "$0")/../tests/integration/lib.sh"
H=$(hostname)
step() { printf '\n\033[1m=== %s ===\033[0m\n' "$*"; }
die() { say "DEMO FAILED: $*"; stop_cluster; exit 1; }

step "1+2. Start dnuma-master on $H and dnuma-agent on $SLAVE"
start_cluster || die "cluster did not start"
"$ROOT/scripts/cluster.sh" status
$CTL slave list

step "3. Attach $SLAVE dynamically"
attach || die "attach failed"

step "4. Combined logical NUMA resources"
$CTL topology
node=$(first_slave_node)

step "5-7. Memory backed by $SLAVE, code executed on $SLAVE writes it, $H verifies it"
$RUN "$BIN/examples/dnuma-hello" "$node" || die "hello failed"
$RUN "$T/t_memory" || die "t_memory failed"

step "8. Graceful disconnect while an application has threads and memory on $SLAVE"
$RUN "$T/t_worker" "$node" 64 12 2 > "$OUT/w.out" 2>&1 &
wpid=$!
wait_line "$OUT/w.out" "worker0-on $SLAVE" 20 || die "application did not start on $SLAVE"
sleep 2
grep TICK "$OUT/w.out" | tail -2
$CTL process list
$CTL slave disconnect "$SLAVE" --drain --timeout 120s || die "drain failed"
$CTL slave list
say "-- the application is still running, now on $H:"
sleep 1.5
grep TICK "$OUT/w.out" | tail -3
wait $wpid || die "application did not survive the drain"
tail -2 "$OUT/w.out"

step "9. Reconnect $SLAVE (new incarnation, same NUMA node numbers)"
attach || die "reattach failed"

step "10. Start a dependent application on $SLAVE and an unrelated one on $H"
$RUN "$T/t_worker" 0 16 8 2 > "$OUT/local.out" 2>&1 &
lpid=$!
$RUN "$T/t_worker" "$node" 32 120 2 > "$OUT/dep.out" 2>&1 &
dpid=$!
wait_line "$OUT/dep.out" "worker0-on $SLAVE" 20 || die "dependent application did not start"
wait_line "$OUT/local.out" "worker0-on $H" 20 || die "unrelated application did not start"
sleep 1
$CTL process list
$CTL memory status

step "11. Hard disconnect $SLAVE"
$CTL slave disconnect "$SLAVE" --hard --dry-run
$CTL slave disconnect "$SLAVE" --hard --yes

step "12. The dependent application is dead, the unrelated one is not"
wait $dpid 2>/dev/null; rc=$?
say "dependent application exit status: $rc ($([ $rc -eq 137 ] && echo 'killed by SIGKILL' || echo 'UNEXPECTED'))"
[ $rc -eq 137 ] || die "dependent application was not killed"
kill -0 $lpid 2>/dev/null && say "unrelated application (pid $lpid) is still running"
wait $lpid; rc=$?
tail -2 "$OUT/local.out"
[ $rc -eq 0 ] || die "unrelated application did not finish cleanly"
$CTL slave status "$SLAVE" | sed -n '1,8p'
$CTL process list

step "Summary of what was measured"
$CTL metrics | grep -E "remote_page_faults|page_transfers |page_invalidations|thread_migrations |forced_process|slave_attaches|fault_latency_ns|rdma_read_ns|thread_migration_ns|slave_drain_ns|rdma_read_rate"
stop_cluster
rm -rf "$OUT"
say; say "DEMO COMPLETE: all 12 steps succeeded"

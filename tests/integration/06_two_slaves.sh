#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Two slave identities attached to one master.
#
# IMPORTANT: this test needs only one physical slave machine. The second
# slave is a second dnuma-agent process running on the master's own host
# with its own identity, ports, page pool and RDMA endpoint. That makes every
# protocol path real - including a slave-to-slave RDMA link that crosses the
# fabric between the two machines - but it is NOT a demonstration of three
# physical machines.
. "$(dirname "$0")/lib.sh"
COLO=colo
# The master's own address on the RDMA network: by default, the local address
# the kernel uses to reach the slave's.
COLO_ADDR="${DNUMA_MASTER_RDMA_ADDR:-$(ip -o route get "$SLAVE_RDMA_ADDR" | sed -n 's/.* src \([^ ]*\).*/\1/p')}"
[ -n "$COLO_ADDR" ] || { say "cannot determine the master's RDMA address; set DNUMA_MASTER_RDMA_ADDR"; exit 2; }
CLOG=/tmp/dnuma-agent-colo.log
stop_colo() { sudo -n pkill -KILL -f -- "--state-dir /var/lib/dnuma-colo" 2>/dev/null; true; }
stop_colo
start_cluster --capacity 8M || { say "cannot start cluster"; exit 1; }
sudo -n rm -f $CLOG
sudo -n sh -c "nohup setsid $BIN/dnuma-agent --key-file $ROOT/.deps/cluster.key --port 7720 --rdma-port 7721 --rdma-ip $COLO_ADDR --state-dir /var/lib/dnuma-colo --pool-size 128M --log-file $CLOG >/dev/null 2>>$CLOG &"
for i in $(seq 100); do (exec 3<>/dev/tcp/$COLO_ADDR/7720) 2>/dev/null && break; sleep 0.1; done

attach > /dev/null
out=$($CTL slave connect $COLO --address $COLO_ADDR --port 7720); rc=$?
say "$out" | sed 's/^/      /'
expect "second slave attaches" "$rc" "0"
expect "two slaves ACTIVE" "$($CTL slave list | grep -c ' ACTIVE ')" "2"
topo=$($CTL topology)
expect "NUMA domains present (master + two slaves)" "$(echo "$topo" | awk '$NF=="ACTIVE" || $NF=="MASTER"' | grep -c -E '^[0-9]')" "$(( $(echo "$topo" | awk '$NF=="MASTER"' | grep -c -E '^[0-9]') * 2 + $(echo "$topo" | awk -v n="$SLAVE" '$3==n' | wc -l) ))"
check "the two slaves have distinct identities" test "$($CTL slave status $SLAVE | awk '/Identity/ {print $3}')" != "$($CTL slave status $COLO | awk '/Identity/ {print $3}')"
check "slave-to-slave RDMA link came up on $SLAVE" slave "grep -q 'link established with machine 2' $ALOG"

# --- data written on one slave, read and rewritten on the other ---
timeout 60 $RUN $T/t_peers > "$OUT/peers.out" 2>&1; rc=$?
sed 's/^/      /' "$OUT/peers.out"
expect "t_peers exit code" "$rc" "0"
check "t_peers reports PASS" grep -q "^RESULT PASS" "$OUT/peers.out"
reads_rain=$($CTL metrics --slave $SLAVE | awk '$1=="page_transfers_in" {gsub(",","",$2); print $2}')
reads_colo=$($CTL metrics --slave $COLO | awk '$1=="page_transfers_in" {gsub(",","",$2); print $2}')
sleep 2.5 # agents report metrics every 2 s
reads_rain=$($CTL metrics --slave $SLAVE | awk '$1=="page_transfers_in" {gsub(",","",$2); print $2}')
reads_colo=$($CTL metrics --slave $COLO | awk '$1=="page_transfers_in" {gsub(",","",$2); print $2}')
check "pages were pulled by $COLO directly from $SLAVE (${reads_colo:-0})" test "${reads_colo:-0}" -ge 256
check "pages were pulled by $SLAVE directly from $COLO (${reads_rain:-0})" test "${reads_rain:-0}" -ge 256

timeout 120 $RUN $T/t_exec > "$OUT/exec.out" 2>&1; rc=$?
tail -2 "$OUT/exec.out" | sed 's/^/      /'
expect "t_exec across all three node sets" "$rc" "0"

# --- drain one slave when the master is nearly full: pages go to the other slave ---
node=$(first_slave_node)
timeout 120 $RUN $T/t_worker "$node" 32 12 2 > "$OUT/w.out" 2>&1 &
wpid=$!
check "application started on $SLAVE" wait_line "$OUT/w.out" "worker0-on $SLAVE" 20
sleep 1
$CTL slave disconnect "$SLAVE" --drain --timeout 60s > "$OUT/drain.out" 2>&1; rc=$?
tail -4 "$OUT/drain.out" | sed 's/^/      /'
expect "drain of $SLAVE exit code" "$rc" "0"
mem=$($CTL memory status)
say "$mem" | sed -n '1,4p' | sed 's/^/      /'
colo_res=$(echo "$mem" | awk -v n="$COLO" '$1==n {print $4}')
check "evacuated pages landed on the other slave (master capacity is 8 MiB)" test "$(echo "$mem" | awk -v n="$COLO" '$1==n {print $5}')" = "MiB"
wait $wpid; rc=$?
tail -2 "$OUT/w.out" | sed 's/^/      /'
expect "application survived and finished" "$rc" "0"
check "application data is intact" grep -q "^RESULT PASS" "$OUT/w.out"

# --- one slave fails: only its dependents die ---
attach > /dev/null
cnode=$($CTL topology | awk -v n="$COLO" '$3==n && $NF=="ACTIVE" {print $1; exit}')
timeout 120 $RUN $T/t_worker "$node" 8 60 1 > "$OUT/a.out" 2>&1 &
apid=$!
timeout 120 $RUN $T/t_worker "$cnode" 8 7 1 > "$OUT/b.out" 2>&1 &
bpid=$!
check "application A started on $SLAVE" wait_line "$OUT/a.out" "worker0-on $SLAVE" 20
check "application B started on $COLO" wait_line "$OUT/b.out" "worker0-on $(hostname)" 20
slave "sudo -n pkill -KILL -x dnuma-agent"
wait $apid; rc=$?
expect "A (dependent on the failed slave) was killed" "$rc" "137"
check "B (on the other slave) is still running" kill -0 $bpid
expect "the other slave is unaffected" "$($CTL slave list | awk -v n="$COLO" '$1==n {print $2}')" "ACTIVE"
check "the surviving slave dropped its link to the failed one" sudo -n grep -q "peer machine 1 fenced" $CLOG
wait $bpid; rc=$?
expect "B finished normally" "$rc" "0"
check "B's data is intact" grep -q "^RESULT PASS" "$OUT/b.out"

stop_colo
stop_cluster
finish_test

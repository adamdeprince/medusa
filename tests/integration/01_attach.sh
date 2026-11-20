#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Attachment: the slave can be attached while the master runs, the master's
# topology reflects the slave's real resources, and repeating or botching a
# connect is handled cleanly.
. "$(dirname "$0")/lib.sh"
start_cluster || { say "cannot start cluster"; exit 1; }

expect "no slaves before attach" "$($CTL slave list | grep -c ACTIVE)" "0"
out=$(attach); rc=$?
expect "connect exit code" "$rc" "0"
contains "connect reports ACTIVE" "$out" "ACTIVE"
contains "RDMA READ/WRITE verified at attach" "$out" "verified WRITE"
expect "slave state" "$(slave_state)" "ACTIVE"

# The master's view must match what the slave's own kernel says about the
# NUMA nodes its agent offers (all of them unless the agent runs with --nodes).
real_cpus=$(slave_offered_cpus)
real_nodes=$(slave_offered_nodes | wc -l | tr -d ' ')
real_mem_kb=$(slave "awk '/MemTotal/ {print \$2}' /proc/meminfo")
status=$($CTL slave status "$SLAVE")
contains "CPU count matches the slave's kernel ($real_cpus)" "$status" "$real_cpus CPUs"
expect "NUMA domains match the slave's sysfs" "$(echo "$status" | grep -c '    logical')" "$real_nodes"
topo=$($CTL topology)
expect "slave NUMA nodes in the logical topology" "$(echo "$topo" | awk -v n="$SLAVE" '$3==n && $NF=="ACTIVE"' | wc -l | tr -d ' ')" "$real_nodes"
for n in $(slave_offered_nodes); do
  want=$(slave "awk '/MemTotal/ {print \$4}' /sys/devices/system/node/node$n/meminfo")
  got=$(echo "$status" | awk -v p="$n" '$1=="logical" && $4==p {print}' | head -1)
  # Compare in GiB with one decimal, as displayed.
  w=$(awk -v k="$want" 'BEGIN {v=k/1048576; printf (v>=100 ? "%.0f GiB" : "%.1f GiB"), v}')
  contains "physical node $n memory ($w)" "$got" "$w"
done
# The NUMA node of whichever network device carries the slave's RDMA address.
rdma_dev=$(slave "ip -o -4 addr show" | awk -v a="$SLAVE_RDMA_ADDR" 'index($4, a "/") == 1 {print $2; exit}')
hca_node=$(slave "cat /sys/class/net/$rdma_dev/device/numa_node 2>/dev/null")
[ -n "$hca_node" ] && [ "$hca_node" -ge 0 ] || hca_node=0 # the kernel reports -1 on machines without NUMA
contains "HCA locality reported (device $rdma_dev)" "$topo" "physical NUMA $hca_node"
say "  (slave kernel reports MemTotal ${real_mem_kb} kB)"

inc1=$(slave_inc)
out=$(attach); rc=$?
expect "second connect is idempotent (exit code)" "$rc" "0"
contains "second connect says already attached" "$out" "already attached"
expect "incarnation unchanged by repeated connect" "$(slave_inc)" "$inc1"

out=$($CTL slave connect ghost --address "$UNREACHABLE_ADDR" 2>&1); rc=$?
expect "connect to an unreachable host fails" "$rc" "1"
expect "failed connect leaves that slave DISCONNECTED" "$($CTL slave list | awk '$1=="ghost" {print $2}')" "DISCONNECTED"
expect "the attached slave is unaffected" "$(slave_state)" "ACTIVE"

out=$($CTL slave connect alias --address "$SLAVE_ADDR" 2>&1); rc=$?
expect "attaching the same agent under another name is refused" "$rc" "1"
contains "refusal names the existing identity" "$out" "already known as slave"

stop_cluster
finish_test

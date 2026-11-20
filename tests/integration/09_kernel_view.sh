#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The kernel's view of slave CPUs and exported work, checked with the files
# and tools an ordinary user would look at: /proc/cpuinfo, /proc/stat,
# /proc/<pid>/stat, sysfs "online", getconf, ps and top.
# Needs a master kernel built with CONFIG_DNUMA_VIEW (kernel/README.md);
# on any other kernel the test reports that and passes without checking.
. "$(dirname "$0")/lib.sh"
if [ ! -e /proc/dnuma_view ]; then
  say "  (not run: this kernel has no /proc/dnuma_view)"
  rm -rf "$OUT"; say "PASS $(basename "$0") (0 checks: kernel view not available)"; exit 0
fi
view() { sudo -n awk -v k="$1" '$1 == k {print $2}' /proc/dnuma_view; }
procs() { grep -c '^processor' /proc/cpuinfo; }
field() { sed 's/.*) //' "/proc/$1/stat" | awk -v n="$2" '{print $(n - 2)}'; } # numbered as in proc(5)

LOCAL=$(procs)
ONLINE0=$(cat /sys/devices/system/cpu/online)
start_cluster || { say "cannot start cluster"; exit 1; }
BASE=$(view nr_cpu_ids)
expect "processors in /proc/cpuinfo before a slave is attached" "$(procs)" "$LOCAL"
attach >/dev/null || { say "attach failed"; exit 1; }
SCPU=$(slave_offered_cpus)
LAST=$((BASE + SCPU - 1))

say "1. slave CPUs"
expect "processors in /proc/cpuinfo" "$(procs)" "$((LOCAL + SCPU))"
expect "of which marked as belonging to another machine" "$(grep -c '^dnuma host' /proc/cpuinfo)" "$SCPU"
expect "their numbers" "$(awk '/^processor/ {n = $3} /^dnuma host/ {print n}' /proc/cpuinfo | sed -n '1p;$p' | tr '\n' ' ')" "$BASE $LAST "
want_model=$(slave "awk -F': ' '/^model name/ {print \$2; exit}' /proc/cpuinfo")
expect "model name comes from the slave" "$(awk -F': ' '/^model name/ {m = $2} /^dnuma host/ {print m; exit}' /proc/cpuinfo)" "$want_model"
expect "cpu lines in /proc/stat" "$(grep -c '^cpu[0-9]' /proc/stat)" "$((LOCAL + SCPU))"
expect "sysfs online" "$(cat /sys/devices/system/cpu/online)" "$ONLINE0,$BASE-$LAST"
expect "online processors according to the C library" "$(getconf _NPROCESSORS_ONLN)" "$((LOCAL + SCPU))"
a=$(awk -v c="cpu$BASE" '$1 == c {print $2 + $4 + $5}' /proc/stat); sleep 2
b=$(awk -v c="cpu$BASE" '$1 == c {print $2 + $4 + $5}' /proc/stat)
check "counters of cpu$BASE advance with the slave's clock (+$((b - a)) ticks in 2 s)" test $((b - a)) -ge 150 -a $((b - a)) -le 250
expect "top shows a row for the last slave CPU" "$(top -b -n 1 -1 -w 200 | grep -o "%Cpu$LAST *:" | wc -l | tr -d ' ')" "1"

say "2. an exported process"
tmp=$(mktemp -d /tmp/dnuma-view.XXXXXX)
$T/n_spin 25 16 "$tmp" 50 > "$OUT/spin.out" 2>&1 & p=$!
sleep 1
home_cpu=$(field $p 39)
check "export succeeds" $CTL export $p "$SLAVE"
sleep 2
cpu=$(field $p 39)
check "/proc/$p/stat reports a slave CPU ($cpu; it was on $home_cpu)" test "$cpu" -ge "$BASE" -a "$cpu" -le "$LAST"
u1=$(( $(field $p 14) + $(field $p 15) )); sleep 3; u2=$(( $(field $p 14) + $(field $p 15) ))
check "its CPU time grows while its code runs on the slave (+$((u2 - u1)) ticks in 3 s)" test $((u2 - u1)) -ge 150
running=0; for i in $(seq 20); do [ "$(field $p 3)" = R ] && running=$((running + 1)); sleep 0.05; done
check "it is shown as running ($running of 20 samples)" test $running -ge 10
expect "ps reports the same CPU" "$(ps -o psr= -p $p | tr -d ' ')" "$cpu"
pct=$(top -b -n 2 -d 2 -p $p | awk -v p=$p '$1 == p {v = $9} END {print int(v)}')
check "top shows it using a CPU ($pct%)" test "$pct" -ge 50
check "top's row for CPU $cpu shows the work" sh -c "top -b -n 2 -d 2 -1 -w 200 | grep -o '%Cpu$cpu *: *[0-9.]* us' | tail -1 | awk '{exit !(\$3 >= 50)}'"
check "recall succeeds" $CTL recall $p
sleep 1
check "home again: a local CPU ($(field $p 39))" test "$(field $p 39)" -lt "$BASE"
u3=$(( $(field $p 14) + $(field $p 15) ))
check "the CPU time it used on the slave is still counted ($u3 >= $u2)" test "$u3" -ge "$u2"
wait $p; expect "exit code" "$?" "0"
check "program verified its memory throughout" grep -q "^RESULT PASS" "$OUT/spin.out"
sleep 0.5
expect "kernel entries left for exited processes" "$(view tasks)" "0"
rm -rf "$tmp"

say "3. detaching"
$CTL slave disconnect "$SLAVE" --drain > /dev/null 2>&1
wait_state DISCONNECTED 20
expect "processors in /proc/cpuinfo after the slave was drained" "$(procs)" "$LOCAL"
expect "sysfs online after the slave was drained" "$(cat /sys/devices/system/cpu/online)" "$ONLINE0"
attach >/dev/null
expect "processors after reattaching" "$(procs)" "$((LOCAL + SCPU))"
expect "same CPU numbers as before" "$(awk '/^processor/ {n = $3} /^dnuma host/ {print n; exit}' /proc/cpuinfo)" "$BASE"
stop_cluster
expect "processors after the master stopped" "$(procs)" "$LOCAL"
expect "kernel view after the master stopped" "$(view cpus) $(view tasks) $(view online)" "0 0 0"

finish_test

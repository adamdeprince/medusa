#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Automatic export: more CPU-bound processes are started (with plain xargs)
# than the master has CPUs, and the master moves the excess to the slave.
# Every process verifies its own memory and does file I/O in a directory that
# exists only on the master.
#   DNUMA_EXPORT_EXCESS   processes beyond the master's CPU count (default:
#                         as many as the smaller machine has CPUs)
#   DNUMA_EXPORT_SECONDS  how long each process runs (default 14)
. "$(dirname "$0")/lib.sh"
start_cluster --auto-export || { say "cannot start cluster"; exit 1; }
attach >/dev/null || { say "attach failed"; exit 1; }
NCPU=$(nproc --all)
SCPU=$(slave_offered_cpus)
excess=${DNUMA_EXPORT_EXCESS:-$(( SCPU < NCPU ? SCPU : NCPU ))}
secs=${DNUMA_EXPORT_SECONDS:-14}
total=$((NCPU + excess))
tmp=$(mktemp -d /tmp/dnuma-export.XXXXXX)
say "$total processes for ${secs}s on a $NCPU-CPU master; slave $SLAVE has $SCPU CPUs"

( seq "$total" | xargs -P "$total" -I{} sh -c "exec $T/n_spin $secs 8 $tmp 25 > $tmp/{}.out 2>&1" ) & xp=$!
peak=0
for i in $(seq $((secs - 2))); do
  sleep 1
  n=$($CTL process list | grep -c " $SLAVE "); [ "$n" -gt "$peak" ] && peak=$n
done
say "  exported at peak: $peak; slave load then: $(slave "cut -d' ' -f1 /proc/loadavg")"
wait $xp; expect "xargs exit code" "$?" "0"

expect "processes that reported PASS" "$(grep -l '^RESULT PASS' "$tmp"/*.out | wc -l | tr -d ' ')" "$total"
remote=$(awk -v n="$NCPU" '/^FINAL/ {split($5, a, "."); if (a[3] >= n) c++} END {print c + 0}' "$tmp"/*.out)
check "processes ran on slave CPUs ($remote; the balancer moves the excess of $excess)" test "$remote" -ge $((excess / 2))
say "  $(awk -v n="$NCPU" '/^FINAL/ {split($5, a, "."); if (a[3] >= n) {r += $3; nr++} else {l += $3; nl++}}
    END {printf "rounds completed per process: stayed home %.0f (n=%d), exported %.0f (n=%d)", nl ? l/nl : 0, nl, nr ? r/nr : 0, nr}' "$tmp"/*.out)"
expect "exported processes lost" "$(grep -c -E "export.*(lost|failed)" $MLOG)" "0"
expect "nothing left exported" "$($CTL process list | grep -c " $SLAVE ")" "0"
expect "bodies left on the slave" "$(slave "pgrep -c -x dnuma-body || true")" "0"
expect "errors in the agent log" "$(slave "grep -c ' ERROR ' $ALOG || true")" "0"
expect "scratch files left on the master" "$(ls "$tmp" | grep -c '^n_spin')" "0"
rm -rf "$tmp"

stop_cluster
finish_test

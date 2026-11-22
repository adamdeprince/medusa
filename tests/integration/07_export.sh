#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Process export: ordinary, unmodified programs started on the master are
# moved to the slave while running. Validated from the programs' own output
# (which CPU they ran on, whether their memory survived, exit status) and
# with files that exist only on the master.
. "$(dirname "$0")/lib.sh"
start_cluster || { say "cannot start cluster"; exit 1; }
attach >/dev/null || { say "attach failed"; exit 1; }
NCPU=$(nproc --all)
# n_spin reports the lowest and highest CPU number it ran on; CPUs of a slave
# are numbered after the master's own.
max_cpu() { awk '/^FINAL/ {split($5, a, "."); print a[3]}' "$1"; }
exported_count() { $CTL process list | grep -c " $SLAVE "; }
tmp=$(mktemp -d /tmp/dnuma-export.XXXXXX) # local to the master, never visible on the slave

say "1. export, then recall while running"
$T/n_spin 8 64 "$tmp" > "$OUT/1.out" 2>&1 & p=$!
sleep 1.5
check "export succeeds" $CTL export $p "$SLAVE"
sleep 2.5
expect "process is listed as exported" "$(exported_count)" "1"
check "recall succeeds" $CTL recall $p
sleep 0.5
expect "process is no longer exported" "$(exported_count)" "0"
wait $p; expect "exit code" "$?" "0"
check "program verified its memory throughout" grep -q "^RESULT PASS" "$OUT/1.out"
check "it ran on a slave CPU (highest CPU $(max_cpu "$OUT/1.out"), master has $NCPU)" test "$(max_cpu "$OUT/1.out")" -ge "$NCPU"
check "it ran at home again after the recall" awk -v n="$NCPU" '/^TICK/ {last=$6} END {exit !(last < n)}' "$OUT/1.out"

say "2. SIGTERM while exported"
$T/n_spin 30 16 "$tmp" > "$OUT/2.out" 2>&1 & p=$!
sleep 1; $CTL export $p "$SLAVE" >/dev/null; sleep 1.5
kill -TERM $p; wait $p
expect "killed by SIGTERM (128+15)" "$?" "143"
killed=$p # it had no chance to remove its scratch file
expect "nothing left exported" "$(exported_count)" "0"

say "3. SIGSTOP / SIGCONT while exported"
$T/n_spin 6 16 "$tmp" > "$OUT/3.out" 2>&1 & p=$!
sleep 1; $CTL export $p "$SLAVE" >/dev/null; sleep 1
kill -STOP $p; sleep 0.5
a=$(grep -c TICK "$OUT/3.out"); sleep 1; b=$(grep -c TICK "$OUT/3.out")
expect "process state while stopped" "$(awk '{print $3}' /proc/$p/stat)" "T"
expect "progress while stopped" "$((b - a))" "0"
kill -CONT $p; wait $p; expect "exit code after SIGCONT" "$?" "0"
check "program verified its memory throughout" grep -q "^RESULT PASS" "$OUT/3.out"

say "4. an ordinary tool reading a file that exists only on the master"
head -c 300000000 /dev/urandom > "$tmp/big"
want=$(sha256sum "$tmp/big" | cut -d' ' -f1)
sha256sum "$tmp/big" > "$OUT/4.out" 2>&1 & p=$!
sleep 0.1
$CTL export $p "$SLAVE" > "$OUT/4.export" 2>&1; exported=$?
wait $p; expect "sha256sum exit code" "$?" "0"
expect "digest computed while exported" "$(cut -d' ' -f1 "$OUT/4.out")" "$want"
expect "sha256sum was exported before it finished" "$exported" "0"

say "5. remote CPU selection and read-mostly native pages"
target=$($CTL slave status "$SLAVE" | awk '/^Cluster CPUs:/ {split($3, r, "-"); print r[2]}')
$T/n_readmostly "$target" > "$OUT/5.out" 2>&1 & p=$!
check "native pager test initialized" wait_line "$OUT/5.out" '^READY$' 5
check "place on the requested CPU" $CTL export "$p" --cpu "$target"
wait "$p"; expect "read-only protections, dirty-page count, CPU, recall" "$?" "0"

m=$($CTL metrics)
val() { echo "$m" | awk -v k="$1" '$1==k {gsub(",","",$2); print $2; exit}'; }
check "syscalls were forwarded to the master ($(val export_syscalls))" test "$(val export_syscalls)" -gt 100
check "pages were served from the master ($(val export_pages_out))" test "$(val export_pages_out)" -gt 1000
expect "exported processes lost" "$(grep -c -E "export.*(lost|failed)" $MLOG)" "0"
expect "bodies left on the slave" "$(slave "pgrep -c -x dnuma-body || true")" "0"
expect "scratch files left by programs that exited normally" "$(ls "$tmp" | grep n_spin | grep -c -v "n_spin\.$killed\$")" "0"
rm -rf "$tmp"

stop_cluster
finish_test

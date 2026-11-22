#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Test of the dnuma view, run as init in a throwaway VM.
export PATH=/bin
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev 2>/dev/null; mount -t tmpfs tmp /tmp
F=0; ok() { echo "  ok    $*"; }; bad() { echo "  FAIL  $*"; F=$((F+1)); }
eq() { if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1: got '$2', expected '$3'"; fi; }
echo "KTEST begin: $(uname -r)"
cat /proc/dnuma_view
N=$(grep -c ^processor /proc/cpuinfo)
ID=$(awk '/^nr_cpu_ids/ {print $2}' /proc/dnuma_view)
A=$ID; B=$((ID + 5))
tot0=$(awk '/^cpu / {print $2}' /proc/stat)
ls -l /proc/dnuma_view | grep -q '^-rw-------' && ok "control file is mode 0600" || bad "control file mode: $(ls -l /proc/dnuma_view)"

exec 3<>/proc/dnuma_view
( exec 4>/proc/dnuma_view ) 2>/dev/null && bad "a second writer was allowed" || ok "second writer refused"
cmd() { printf "$@" > /tmp/cmd; cat /tmp/cmd >&3 2>/dev/null; }
blk() { printf 'processor\t: %d\nmodel name\t: Remote CPU of %s\nphysical id\t: 9\ndnuma host\t: %s\n\n' "$1" "$2" "$2"; }
info() { blk "$1" "$2" > /tmp/b; { printf 'cpu %d info %d\n' "$1" "$(wc -c < /tmp/b)"; cat /tmp/b; } > /tmp/cmd; cat /tmp/cmd >&3 2>/dev/null; }

cmd 'cpu 0 info 0\n' && bad "a local CPU number was accepted" || ok "local CPU number refused"
cmd 'cpu %d stat 1 2 3 4 5 6 7 8 9 10\n' $A && bad "stat for an undefined CPU accepted" || ok "stat for an undefined CPU refused"
cmd 'bogus\n' && bad "unknown command accepted" || ok "unknown command refused"
cmd 'cpu %d del' $A && bad "command without newline accepted" || ok "incomplete command refused"
info $B hostB && ok "defined CPU $B" || bad "define CPU $B"
info $A hostA && ok "defined CPU $A (lower number, added later)" || bad "define CPU $A"
eq "processors in /proc/cpuinfo" "$(grep -c ^processor /proc/cpuinfo)" "$((N + 2))"
eq "order of processor numbers" "$(awk '/^processor/ {printf "%s ", $3}' /proc/cpuinfo | awk '{print $(NF-1), $NF}')" "$A $B"
eq "remote block text" "$(grep -c 'Remote CPU of host[AB]' /proc/cpuinfo)" "2"
eq "cpuinfo read in small pieces is identical" "$(dd if=/proc/cpuinfo bs=37 2>/dev/null | md5sum)" "$(cat /proc/cpuinfo | md5sum)"
cmd 'cpu %d stat 100 0 50 1000 5 0 1 0 0 0\ncpu %d stat 7 0 0 0 0 0 0 0 0 0\n' $A $B && ok "two stat commands in one write" || bad "stat batch"
eq "cpu$A line in /proc/stat" "$(grep "^cpu$A " /proc/stat)" "cpu$A 100 0 50 1000 5 0 1 0 0 0"
eq "cpu$B line in /proc/stat" "$(grep "^cpu$B " /proc/stat)" "cpu$B 7 0 0 0 0 0 0 0 0 0"
eq "cpu lines in /proc/stat" "$(grep -c '^cpu[0-9]' /proc/stat)" "$((N + 2))"
tot1=$(awk '/^cpu / {print $2}' /proc/stat)
[ $((tot1 - tot0)) -ge 107 ] && [ $((tot1 - tot0)) -le 150 ] && ok "total user time includes the remote CPUs (+$((tot1 - tot0)))" || bad "total user time delta $((tot1 - tot0))"
info $A hostA2; eq "replacing a CPU keeps its counters" "$(grep "^cpu$A " /proc/stat)" "cpu$A 100 0 50 1000 5 0 1 0 0 0"
eq "replaced text" "$(grep -c 'Remote CPU of hostA2' /proc/cpuinfo)" "1"

sleep 1000 & P=$!
sleep 0.2
f() { sed 's/.*) //' /proc/$P/$1 | awk -v n="$2" '{print $(n - 2)}'; }
eq "task state before" "$(f stat 3)" "S"
u0=$(f stat 14); c0=$(f stat 39)
cmd 'task %d %d R 1234 56\n' $P $B && ok "task described" || bad "task command"
eq "state shown" "$(f stat 3)" "R"
eq "utime shown" "$(f stat 14)" "$((u0 + 1234))"
eq "stime shown" "$(f stat 15)" "56"
eq "CPU shown" "$(f stat 39)" "$B"
eq "CPU shown per thread" "$(f task/$P/stat 39)" "$B"
eq "ps: state and CPU" "$(/bin/ps -o stat=,psr= -p $P | awk '{print substr($1,1,1), $2}')" "R $B"
eq "ps: CPU time" "$(/bin/ps -o time= -p $P | tr -d ' ')" "00:00:12"
TERM=dumb /bin/top -b -n 1 -1 -w 200 > /tmp/top.out 2>/tmp/top.err; eq "top exit code" "$?" "0"
eq "top: per-CPU entries without sysfs online" "$(grep -o '%Cpu[0-9]*' /tmp/top.out | wc -l)" "$N"
eq "sysfs online by default" "$(cat /sys/devices/system/cpu/online)" "0-$((N - 1))"
cmd 'online on\n'
eq "sysfs online with remote CPUs" "$(cat /sys/devices/system/cpu/online)" "0-$((N - 1)),$A,$B"
TERM=dumb /bin/top -b -n 1 -1 -w 200 > /tmp/top.out 2>/tmp/top.err
eq "top: per-CPU entries with sysfs online" "$(grep -o '%Cpu[0-9]*' /tmp/top.out | tr '\n' ' ')" "$(for i in $(seq 0 $((N - 1))) $A $B; do printf '%%Cpu%d ' $i; done)"
eq "status: state" "$(awk '/^State:/ {print $2, $3}' /proc/$P/status)" "R (running)"
info $((B + 1)) hostB; info $((B + 2)) hostB
eq "sysfs online ranges" "$(cat /sys/devices/system/cpu/online)" "0-$((N - 1)),$A,$B-$((B + 2))"
cmd 'cpu %d del\ncpu %d del\n' $((B + 1)) $((B + 2))
cmd 'online off\n'; eq "sysfs online off again" "$(cat /sys/devices/system/cpu/online)" "0-$((N - 1))"; cmd 'online on\n'
grep " sleep" /tmp/top.out | sed "s/^/        /"; head -3 /tmp/top.err
eq "top: state of the described task" "$(awk -v p=$P '$1 == p {print $8}' /tmp/top.out)" "R"
cmd 'task %d -1 - 2000 60\n' $P
eq "home again: real state" "$(f stat 3)" "S"
eq "home again: real CPU" "$(f stat 39)" "$c0"
eq "home again: remote time stays" "$(f stat 14)" "$((u0 + 2000))"
cmd 'task %d %d x 1 1\n' $P $B && bad "a state other than R or - accepted" || ok "invalid state refused"
cmd 'task %d %d R 1 1\ntask 1 0 R 1 1\n' $P $B && bad "task on a local CPU number accepted" || ok "task on a local CPU number refused"
eq "tasks counted" "$(awk '/^tasks/ {print $2}' /proc/dnuma_view)" "1"
kill $P; wait $P 2>/dev/null
cmd 'task %d %d R 1 1\n' $P $B && ok "update for an exited task is ignored" || bad "update for an exited task failed"
eq "exited task reaped" "$(awk '/^tasks/ {print $2}' /proc/dnuma_view)" "0"

echo "  stress: readers against a writer adding and removing CPUs"
for r in 1 2 3; do ( while [ ! -e /tmp/stop ]; do cat /proc/cpuinfo /proc/stat > /dev/null; dd if=/proc/cpuinfo bs=100 2>/dev/null | wc -c > /dev/null; done ) & done
sleep 500 & Q=$!
i=0; while [ $i -lt 1500 ]; do
  c=$((ID + 10 + i % 40)); info $c stress
  cmd 'cpu %d stat %d 0 0 0 0 0 0 0 0 0\ntask %d %d R %d 0\n' $c $i $Q $c $i
  [ $((i % 3)) = 0 ] && cmd 'cpu %d del\n' $((ID + 10 + (i * 7) % 40))
  [ $((i % 50)) = 0 ] && cmd 'task %d del\n' $Q
  cat /proc/$Q/stat > /dev/null
  i=$((i + 1))
done
touch /tmp/stop; kill $Q; wait 2>/dev/null
ok "stress finished ($(awk '/^cpus/ {print $2}' /proc/dnuma_view) CPUs defined at the end)"
cmd 'clear\n'; eq "after clear: cpus" "$(awk '/^cpus/ {print $2}' /proc/dnuma_view)" "0"
info $A again; cmd 'task %d %d R 5 5\n' $$ $A
exec 3>&-
eq "after the writer closed: processors" "$(grep -c ^processor /proc/cpuinfo)" "$N"
eq "after the writer closed: cpu lines" "$(grep -c '^cpu[0-9]' /proc/stat)" "$N"
eq "after the writer closed: counts" "$(awk '/^cpus|^tasks|^online/ {printf "%s ", $2}' /proc/dnuma_view)" "0 0 0 "
eq "after the writer closed: sysfs online" "$(cat /sys/devices/system/cpu/online)" "0-$((N - 1))"
exec 3<>/proc/dnuma_view && ok "a new writer can open it" || bad "reopen"; exec 3>&-
echo "  the master's client (tests/unit/test_kernel_view) against this kernel:"
/bin/test_kernel_view 2>&1 | sed 's/^/      /'
/bin/test_kernel_view > /tmp/client.out 2>&1; grep -q "0 failure" /tmp/client.out && ! grep -q "not run" /tmp/client.out && ok "client test ran and passed" || bad "client test"
case " $(cat /proc/cmdline) " in
  *" dnuma.test_native_pager=1 "*)
    /bin/n_native_pager && ok "native pthread heap" || bad "native pthread heap"
    /bin/n_native_pager --pair && ok "native page handoffs" || bad "native page handoffs"
    if [ -x /bin/n_go_heap ]; then
      /bin/n_native_pager /bin/n_go_heap && ok "native Go heap" || bad "native Go heap"
      /bin/n_native_pager --pair /bin/n_go_heap && ok "pthread and Go page handoffs" || bad "pthread and Go page handoffs"
    fi ;;
esac
echo "--- kernel log (warnings and worse)"
dmesg | grep -iE "BUG|WARNING|Oops|RCU|lockdep|sleeping function|KASAN|UBSAN|circular|dnuma" | head -40
echo "KTEST end: $F failure(s)"
sync; poweroff -f

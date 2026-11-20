#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Distributed memory, remote execution and coherence, validated from inside
# real distributed programs (contents, physical placement, executing host
# and CPU, ownership states) rather than from RPC return codes.
. "$(dirname "$0")/lib.sh"
start_cluster --trace-pages || { say "cannot start cluster"; exit 1; }
attach >/dev/null || { say "attach failed"; exit 1; }

for prog in t_memory t_exec "t_coherence ${DNUMA_COHERENCE_SCALE:-3}"; do
  name=${prog%% *}
  timeout 300 $RUN $T/$prog > "$OUT/$name.out" 2>&1; rc=$?
  sed 's/^/      /' "$OUT/$name.out"
  expect "$name exit code" "$rc" "0"
  check "$name reports PASS" grep -q "^RESULT PASS" "$OUT/$name.out"
done

m=$($CTL metrics)
tr() { echo "$m" | awk -v k="$1" '$1==k {gsub(",","",$2); print $2; exit}'; }
check "remote page faults were served ($(tr remote_page_faults))" test "$(tr remote_page_faults)" -gt 100
check "page invalidations happened ($(tr page_invalidations))" test "$(tr page_invalidations)" -gt 100
expect "revocations whose unmap could not be verified" "$(echo "$m" | grep -c revoke_verify_failures)" "0"
expect "forced process terminations" "$(echo "$m" | grep -c forced_process_terminations)" "0"
traced=$(grep -c "PAGE dir" $MLOG)
check "every ownership transition was traced ($traced lines)" test "$traced" -ge "$(tr page_ownership_transitions)"
expect "no distributed process left behind" "$($CTL process list | grep -c 'no distributed processes')" "1"
expect "no image left on the slave" "$(slave "pgrep -c -x 't_.*' || true")" "0"

stop_cluster
finish_test

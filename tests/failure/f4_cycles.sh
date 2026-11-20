#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Repeated connect/disconnect cycles, alternating graceful and forced, with
# real work in every cycle. Incarnations must strictly increase and node
# numbering must never change.
. "$(dirname "$0")/../integration/lib.sh"
start_cluster || { say "cannot start cluster"; exit 1; }
cycles=${DNUMA_CYCLES:-8}
last=0
node=""
for i in $(seq "$cycles"); do
  attach > /dev/null || { bad "cycle $i: attach"; break; }
  inc=$(slave_inc)
  [ -z "$node" ] && node=$(first_slave_node)
  if [ "$inc" -gt "$last" ] && [ "$(first_slave_node)" = "$node" ]; then ok "cycle $i: incarnation $inc, first slave node $node"; else bad "cycle $i: incarnation $inc (previous $last), node $(first_slave_node)"; fi
  last=$inc
  timeout 30 $RUN "$BIN/examples/dnuma-hello" "$node" > "$OUT/hello.$i" 2>&1 || bad "cycle $i: workload failed: $(tail -1 "$OUT/hello.$i")"
  if [ $((i % 2)) -eq 1 ]; then $CTL slave disconnect "$SLAVE" --drain --timeout 30s > "$OUT/d.$i" 2>&1 || bad "cycle $i: drain failed"
  else $CTL slave disconnect "$SLAVE" --hard --yes > "$OUT/d.$i" 2>&1 || bad "cycle $i: hard disconnect failed"; fi
  [ "$(slave_state)" = "DISCONNECTED" ] || bad "cycle $i: state $(slave_state) after disconnect"
done
expect "attach count recorded by the master" "$($CTL slave status "$SLAVE" | sed -n 's/.*attached \([0-9]*\) time.*/\1/p')" "$cycles"
expect "nothing was force-terminated" "$($CTL metrics | grep -c forced_process_terminations)" "0"
expect "master still healthy" "$($CTL slave list | grep -c MASTER)" "1"
stop_cluster
finish_test

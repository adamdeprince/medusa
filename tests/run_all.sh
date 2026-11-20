#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Runs the whole test suite on the master host:
#   unit tests (no hardware), integration tests and failure-injection tests
#   (both machines). Set DNUMA_ASAN=1 to run the daemons under ASan.
#
#   tests/run_all.sh [unit|integration|failure]...
root="$(cd "$(dirname "$0")/.." && pwd)"
what="${*:-unit integration failure}"
if [ "${DNUMA_ASAN:-0}" = 1 ]; then
  "$root/scripts/asan-build.sh" || { echo "the AddressSanitizer build failed"; exit 1; }
fi
pass=0; fail=0; failed=""
run() {
  local name="$1"; shift
  local log; log=$(mktemp)
  if "$@" > "$log" 2>&1; then pass=$((pass + 1)); printf '  PASS  %-28s %s\n' "$name" "$(tail -1 "$log")"
  else fail=$((fail + 1)); failed="$failed $name"; printf '  FAIL  %-28s\n' "$name"; grep -E "FAIL|error" "$log" | head -8 | sed 's/^/          /'; fi
  rm -f "$log"
}
for group in $what; do
  echo "== $group =="
  case $group in
  unit)
    dir="$root/build/tests"; [ "${DNUMA_ASAN:-0}" = 1 ] && dir="$root/build-asan/tests"
    for t in test_common test_wire test_lifecycle test_coherence test_kernel_view; do run "$t" "$dir/$t"; done ;;
  integration) for s in "$root"/tests/integration/[0-9]*.sh; do run "$(basename "$s")" "$s"; done ;;
  failure) for s in "$root"/tests/failure/f*.sh; do run "$(basename "$s")" "$s"; done ;;
  *) echo "unknown group $group"; exit 2 ;;
  esac
done
"$root/scripts/cluster.sh" stop > /dev/null 2>&1
echo
echo "$pass passed, $fail failed${failed:+:$failed}"
[ $fail -eq 0 ]

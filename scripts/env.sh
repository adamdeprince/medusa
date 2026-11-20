# SPDX-License-Identifier: GPL-2.0-only
# Site configuration for the helper scripts and the tests. Source this file.
#
# dnuma itself has no built-in knowledge of any machine: every host name and
# address the scripts use comes from the environment, or from a file of
# shell assignments - $DNUMA_ENV_FILE, by default .dnuma-env in the source
# tree. See scripts/dnuma-env.example for the variables.
DNUMA_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DNUMA_ENV_FILE="${DNUMA_ENV_FILE:-$DNUMA_ROOT/.dnuma-env}"
# Values already in the environment win over the file.
if [ -f "$DNUMA_ENV_FILE" ]; then
  while IFS= read -r _line; do
    case "$_line" in ''|'#'*) continue;; esac
    _name="${_line%%=*}"
    [ -n "${!_name+x}" ] || eval "export $_line"
  done < "$DNUMA_ENV_FILE"
  unset _line _name
fi

# dnuma_need VAR...: stop with an explanation if a variable is not set.
dnuma_need() {
  local v
  for v in "$@"; do
    if [ -z "${!v:-}" ]; then
      echo "$v is not set. Set it in the environment or in $DNUMA_ENV_FILE (see scripts/dnuma-env.example)." >&2
      exit 2
    fi
  done
}

#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NEOSHARED_ROOT_VALUE=""
FORWARD=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --neoshared-root) NEOSHARED_ROOT_VALUE="$2"; shift 2;;
    *) FORWARD+=("$1"); shift;;
  esac
done

if [[ -z "$NEOSHARED_ROOT_VALUE" ]]; then
  NEOSHARED_ROOT_VALUE="$("${CMAKE:-cmake}" -P "$ROOT_DIR/cmake/NeoSharedSource.cmake")"
fi
case "$NEOSHARED_ROOT_VALUE" in /*|[A-Za-z]:/*) ;; *) NEOSHARED_ROOT_VALUE="$ROOT_DIR/$NEOSHARED_ROOT_VALUE";; esac
[[ -f "$NEOSHARED_ROOT_VALUE/scripts/build-macos-app.sh" ]] || {
  echo "neoshared macOS helper was not found under: $NEOSHARED_ROOT_VALUE" >&2
  echo "The automatically selected NeoShared checkout is incomplete." >&2
  exit 2
}

bash "$NEOSHARED_ROOT_VALUE/scripts/build-macos-app.sh" \
  --source-root "$ROOT_DIR" \
  --app-name "NeoBIF" \
  --neoshared-root "$NEOSHARED_ROOT_VALUE" \
  "${FORWARD[@]+"${FORWARD[@]}"}"

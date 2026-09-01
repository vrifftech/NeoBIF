#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NEOSHARED_ROOT_VALUE="${NEOSHARED_ROOT:-$ROOT_DIR/../neoshared}"
DEPS_ROOT="${NEO_WASM_DEPS_ROOT:-$ROOT_DIR/../.neo-wasm-deps}"
forward=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --neoshared-root) NEOSHARED_ROOT_VALUE="$2"; shift 2;;
    --deps-root) DEPS_ROOT="$2"; shift 2;;
    *) forward+=("$1"); shift;;
  esac
done
case "$NEOSHARED_ROOT_VALUE" in /*) ;; *) NEOSHARED_ROOT_VALUE="$ROOT_DIR/$NEOSHARED_ROOT_VALUE";; esac
case "$DEPS_ROOT" in /*) ;; *) DEPS_ROOT="$ROOT_DIR/$DEPS_ROOT";; esac
[[ -f "$NEOSHARED_ROOT_VALUE/scripts/build-wasm-app.sh" ]] || {
  echo "neoshared browser-build helper was not found: $NEOSHARED_ROOT_VALUE" >&2
  exit 2
}
bash "$NEOSHARED_ROOT_VALUE/scripts/build-wasm-app.sh" \
  --source-root "$ROOT_DIR" \
  --neoshared-root "$NEOSHARED_ROOT_VALUE" \
  --deps-root "$DEPS_ROOT" \
  --app-target "NeoBIF" \
  --app-name "NeoBIF" \
  --slug "neobif" \
  --option-prefix "NEOBIF" \
  --cli-option "NEOBIF_BUILD_CLI" \
  --icon "resources/neobif.svg" \
  "${forward[@]+"${forward[@]}"}"

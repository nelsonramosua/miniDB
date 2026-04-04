#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
USECASES_DIR="$ROOT_DIR/scripts/useCases"

if [[ ! -d "$USECASES_DIR" ]]; then
  echo "No use-cases directory found at $USECASES_DIR"
  exit 0
fi

echo "==> Use-case smoke checks"

check_node_usecase() {
  local pkg_dir="$1"
  echo "[node] $pkg_dir"

  pushd "$pkg_dir" >/dev/null

  if [[ -f package-lock.json ]]; then
    npm ci
  else
    npm install
  fi

  npm run -s test --if-present
  npm run -s build --if-present

  # Parse-check all JS files without executing application runtime.
  while IFS= read -r -d '' js_file; do
    node --check "$js_file"
  done < <(find . -type f -name '*.js' -print0)

  popd >/dev/null
}

check_go_usecase() {
  local mod_dir="$1"
  echo "[go] $mod_dir"

  pushd "$mod_dir" >/dev/null
  go mod tidy
  go build ./...
  popd >/dev/null
}

check_python_usecase() {
  local req_file="$1"
  local req_dir
  req_dir="$(dirname "$req_file")"
  echo "[python] $req_dir"

  pushd "$req_dir" >/dev/null
  python -m pip install --upgrade pip
  python -m pip install -r requirements.txt

  while IFS= read -r -d '' py_file; do
    python -m py_compile "$py_file"
  done < <(find . -type f -name '*.py' -print0)

  popd >/dev/null
}

while IFS= read -r -d '' pkg_json; do
  check_node_usecase "$(dirname "$pkg_json")"
done < <(find "$USECASES_DIR" -type f -name package.json -print0)

while IFS= read -r -d '' go_mod; do
  check_go_usecase "$(dirname "$go_mod")"
done < <(find "$USECASES_DIR" -type f -name go.mod -print0)

while IFS= read -r -d '' requirements_txt; do
  check_python_usecase "$requirements_txt"
done < <(find "$USECASES_DIR" -type f -name requirements.txt -print0)

echo "Use-case smoke checks passed"

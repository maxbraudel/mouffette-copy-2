#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cmake -DSOURCE_ROOT="$ROOT_DIR" \
  -P "$ROOT_DIR/tests/cmake/verify_qml_architecture.cmake"

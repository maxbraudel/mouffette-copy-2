#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ALLOWLIST_FILE="$ROOT_DIR/tools/architecture_screen_canvas_allowlist.txt"

if [[ ! -f "$ALLOWLIST_FILE" ]]; then
  echo "[ARCH-CHECK] FAIL: Missing allowlist file: $ALLOWLIST_FILE"
  exit 1
fi

cd "$ROOT_DIR"

echo "[ARCH-CHECK] Running architecture boundary checks..."

# Rule A: backend must not include Quick/QML types
QUICK_QML_MATCHES="$(grep -RInE --include='*.h' --include='*.hpp' --include='*.cpp' --include='*.mm' '#include[[:space:]]*[<"](QQuick|QQuickWidget|QQuickItem|QQuickWindow|QQml|QQmlEngine|QQmlComponent|QtQuick|QtQml)' src/backend 2>/dev/null || true)"
if [[ -n "$QUICK_QML_MATCHES" ]]; then
  echo "[ARCH-CHECK] FAIL: Backend includes Quick/QML types (forbidden)."
  echo "$QUICK_QML_MATCHES"
  exit 1
fi

# Rule B: orchestration should not grow new concrete renderer internals dependencies
ORCH_MATCHES="$(grep -RInE --include='*.h' --include='*.hpp' --include='*.cpp' --include='*.mm' '#include[[:space:]]+"frontend/rendering/canvas/[^"]+"' src/backend/controllers src/backend/handlers src/backend/managers 2>/dev/null || true)"

UNALLOWLISTED=""
if [[ -n "$ORCH_MATCHES" ]]; then
  while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    file_path="${line%%:*}"
    if ! grep -Fxq "$file_path" "$ALLOWLIST_FILE"; then
      UNALLOWLISTED+="$line"$'\n'
    fi
  done <<< "$ORCH_MATCHES"
fi

if [[ -n "$UNALLOWLISTED" ]]; then
  echo "[ARCH-CHECK] FAIL: New orchestration -> concrete renderer include(s) detected outside allowlist."
  echo "$UNALLOWLISTED"
  echo "[ARCH-CHECK] If intentional legacy debt, add file path to: tools/architecture_screen_canvas_allowlist.txt"
  exit 1
fi

echo "[ARCH-CHECK] PASS: Boundary checks OK."

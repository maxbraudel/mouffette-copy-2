#!/usr/bin/env bash

set -euo pipefail

INTERNAL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_ROOT="$(cd "$INTERNAL_DIR/../.." && pwd)"
CONFIGURATION="dev"
PACKAGED=false

usage() {
    cat <<'EOF'
Internal usage: run-macos.sh [dev|prod] [--packaged] [-- <application arguments>]

  dev          Run out/build/macos-debug/Mouffette.app (default)
  prod         Run out/build/macos-release/Mouffette.app
  --packaged   Run the standalone app from out/stage/macos-release
EOF
}

if [[ $# -gt 0 && "$1" != --* ]]; then
    CONFIGURATION="$1"
    shift
fi

APP_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --packaged) PACKAGED=true; shift ;;
        --) shift; APP_ARGS+=("$@"); break ;;
        -h|--help) usage; exit 0 ;;
        *) APP_ARGS+=("$1"); shift ;;
    esac
done

if [[ "$PACKAGED" == true ]]; then
    APP="$CLIENT_ROOT/out/stage/macos-release/Mouffette.app"
    LABEL="packaged production"
else
    case "$CONFIGURATION" in
        dev|debug) APP="$CLIENT_ROOT/out/build/macos-debug/Mouffette.app"; LABEL="development" ;;
        prod|production|release) APP="$CLIENT_ROOT/out/build/macos-release/Mouffette.app"; LABEL="production Release" ;;
        *) echo "Configuration must be dev or prod." >&2; usage >&2; exit 2 ;;
    esac
fi

EXECUTABLE="$APP/Contents/MacOS/Mouffette"
if [[ ! -x "$EXECUTABLE" ]]; then
    echo "Mouffette $LABEL was not found at: $APP" >&2
    if [[ "$PACKAGED" == true ]]; then
        echo "Create it with: scripts/package-release.sh" >&2
    else
        echo "Build it with the matching script in client/scripts." >&2
    fi
    exit 1
fi

echo "Starting Mouffette ($LABEL)..."
if [[ ${#APP_ARGS[@]} -gt 0 ]]; then
    exec "$EXECUTABLE" "${APP_ARGS[@]}"
else
    exec "$EXECUTABLE"
fi

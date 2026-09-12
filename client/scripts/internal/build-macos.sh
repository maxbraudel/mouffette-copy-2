#!/usr/bin/env bash

set -euo pipefail

INTERNAL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_ROOT="$(cd "$INTERNAL_DIR/../.." && pwd)"
cd "$CLIENT_ROOT"

usage() {
    cat <<'EOF'
Internal usage: build-macos.sh [dev|prod] [--clean] [--target <target>]

  dev   Debug build in out/build/macos-debug (default)
  prod  Release build in out/build/macos-release

Use scripts/package-release.sh to create the standalone Release DMG.
EOF
}

CONFIGURATION="dev"
CLEAN=false
TARGET=""
if [[ $# -gt 0 && "$1" != --* ]]; then
    CONFIGURATION="$1"
    shift
fi
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) CLEAN=true; shift ;;
        --target)
            [[ $# -ge 2 ]] || { echo "Missing value after --target" >&2; exit 2; }
            TARGET="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$CONFIGURATION" in
    dev|debug) PRESET="macos-debug"; LABEL="development (Debug)" ;;
    prod|production|release) PRESET="macos-release"; LABEL="production (Release)" ;;
    *) echo "Configuration must be dev or prod." >&2; usage >&2; exit 2 ;;
esac

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This script builds macOS. Use the matching .ps1 command on Windows." >&2
    exit 2
fi

echo "Building Mouffette for $LABEL..."
./tools/check_architecture_boundaries.sh

if [[ -z "${MOUFFETTE_QT_ROOT:-}" ]] && command -v brew >/dev/null 2>&1; then
    MOUFFETTE_QT_ROOT="$(brew --prefix qt 2>/dev/null || true)"
fi
if [[ -z "${MOUFFETTE_QT_ROOT:-}" || ! -d "$MOUFFETTE_QT_ROOT" ]]; then
    echo "No Qt 6 installation selected. Set MOUFFETTE_QT_ROOT." >&2
    exit 1
fi

QT_GUI_BINARY="$MOUFFETTE_QT_ROOT/lib/QtGui.framework/Versions/A/QtGui"
if [[ -f "$QT_GUI_BINARY" ]] && otool -L "$QT_GUI_BINARY" 2>/dev/null | grep -q 'AGL\.framework'; then
    echo "The selected QtGui still depends on the removed AGL framework:" >&2
    echo "  $QT_GUI_BINARY" >&2
    exit 1
fi
export MOUFFETTE_QT_ROOT

# Homebrew can build Qt add-on frameworks for the host macOS only. Defaulting
# to the active host keeps Info.plist truthful and avoids claiming support the
# linked frameworks do not have. Release builders can select an older target
# explicitly when using a compatible official/custom Qt SDK.
if [[ -z "${MOUFFETTE_MACOS_DEPLOYMENT_TARGET:-}" ]]; then
    MOUFFETTE_MACOS_DEPLOYMENT_TARGET="$(sw_vers -productVersion | awk -F. '{print $1 "." $2}')"
fi
export MOUFFETTE_MACOS_DEPLOYMENT_TARGET
echo "macOS deployment target: $MOUFFETTE_MACOS_DEPLOYMENT_TARGET"

CONFIGURE_ARGS=(--preset "$PRESET")
if [[ "$CLEAN" == true ]]; then
    CONFIGURE_ARGS+=(--fresh)
fi
cmake "${CONFIGURE_ARGS[@]}"

BUILD_ARGS=(--build --preset "$PRESET" --parallel)
if [[ -n "$TARGET" ]]; then
    BUILD_ARGS+=(--target "$TARGET")
fi
cmake "${BUILD_ARGS[@]}"

echo "Build successful: out/build/$PRESET/Mouffette.app"

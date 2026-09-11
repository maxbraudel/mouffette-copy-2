#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "🔨 Building Mouffette Client..."

if [ -x "./tools/check_architecture_boundaries.sh" ]; then
    echo "🛡️ Running architecture boundary guardrails..."
    ./tools/check_architecture_boundaries.sh || {
        echo "❌ Architecture boundary checks failed!"
        exit 1
    }
fi

# Prefer an explicit Qt root, then discover Homebrew's active Qt installation.
if [ -z "${MOUFFETTE_QT_ROOT:-}" ] && command -v brew >/dev/null 2>&1; then
    MOUFFETTE_QT_ROOT="$(brew --prefix qt 2>/dev/null || true)"
fi

if [ -z "${MOUFFETTE_QT_ROOT:-}" ]; then
    echo "❌ No Qt 6 installation was selected."
    echo "   Set MOUFFETTE_QT_ROOT to the root of a Qt distribution."
    exit 1
fi

QT_GUI_BINARY="$MOUFFETTE_QT_ROOT/lib/QtGui.framework/Versions/A/QtGui"
if [ "$(uname -s)" = "Darwin" ] && [ -f "$QT_GUI_BINARY" ] \
    && otool -L "$QT_GUI_BINARY" 2>/dev/null | grep -q 'AGL\.framework'; then
    echo "❌ The selected Qt installation is incompatible with the active macOS SDK:"
    echo "   $MOUFFETTE_QT_ROOT"
    echo "   Its QtGui framework still depends on the removed AGL framework."
    if command -v brew >/dev/null 2>&1 \
        && [ "$MOUFFETTE_QT_ROOT" = "$(brew --prefix qt 2>/dev/null || true)" ]; then
        echo ""
        echo "   Update it with: brew upgrade qt"
    else
        echo "   Select a newer Qt build with MOUFFETTE_QT_ROOT=<qt-prefix>."
    fi
    exit 1
fi

export MOUFFETTE_QT_ROOT

echo "⚙️ Configuring CMake..."
cmake --preset qt6-debug || {
    echo "❌ CMake configuration failed!"
    exit 1
}

echo "🔨 Building..."
if cmake --build --preset qt6-debug --parallel; then
    echo "✅ Build successful!"
    echo "📦 Executable: out/build/qt6-debug/MouffetteClient.app"
else
    echo "❌ Build failed!"
    exit 1
fi

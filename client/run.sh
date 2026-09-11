#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_EXECUTABLE="$SCRIPT_DIR/out/build/qt6-debug/MouffetteClient.app/Contents/MacOS/MouffetteClient"

echo "🚀 Launching Mouffette Client..."

# Only the current CMake preset output is supported.
if [ ! -x "$CLIENT_EXECUTABLE" ]; then
    echo "❌ Client not found at:"
    echo "   $CLIENT_EXECUTABLE"
    echo "💡 Build with: ./build.sh"
    exit 1
fi

echo "📱 Starting Mouffette client in system tray..."
echo "🔍 Look for the blue icon in your menu bar (macOS) or taskbar"
echo "👆 Click the icon to show the main window"
echo "🎯 The client will auto-connect to localhost:8080"
echo ""

exec "$CLIENT_EXECUTABLE" "$@"

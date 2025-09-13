#!/bin/bash

echo "🚀 Launching Mouffette Client..."

# Check if we're in the client directory
if [ ! -f "build/MouffetteClient.app/Contents/MacOS/MouffetteClient" ]; then
    echo "❌ Client not found. Please run this from the client directory after building."
    echo "💡 Build with: ./build.sh"
    exit 1
fi

echo "📱 Starting Mouffette client in system tray..."
echo "🔍 Look for the blue icon in your menu bar (macOS) or taskbar"
echo "👆 Click the icon to show the main window"
echo "🎯 The client will auto-connect to localhost:8080"
echo ""

# Launch the client
cd build
./MouffetteClient.app/Contents/MacOS/MouffetteClient

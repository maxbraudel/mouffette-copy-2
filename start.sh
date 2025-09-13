#!/bin/bash

echo "🎯 Starting Mouffette System..."

# Check if we're in the right directory
if [ ! -d "server" ] || [ ! -d "client" ]; then
    echo "❌ Please run this script from the mouffette root directory"
    exit 1
fi

# Function to kill background processes on exit
cleanup() {
    echo "🛑 Stopping Mouffette system..."
    if [ ! -z "$SERVER_PID" ]; then
        kill $SERVER_PID 2>/dev/null
    fi
    exit 0
}

# Set up cleanup on script exit
trap cleanup SIGINT SIGTERM

# Start the server in the background
echo "🖥️ Starting server..."
cd server
node server.js &
SERVER_PID=$!
cd ..

# Wait a moment for server to start
sleep 2

# Check if server is still running
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "❌ Server failed to start"
    exit 1
fi

echo "✅ Server started (PID: $SERVER_PID)"
echo "🖥️ Server running at ws://localhost:8080"
echo ""
echo "📱 To test the client:"
echo "   cd client/build"
echo "   ./MouffetteClient.app/Contents/MacOS/MouffetteClient"
echo ""
echo "Press Ctrl+C to stop the server..."

# Wait for the server process
wait $SERVER_PID

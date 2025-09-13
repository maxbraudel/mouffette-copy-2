#!/bin/bash

echo "🔨 Building Mouffette Client..."

# Create build directory
mkdir -p build
cd build

# Configure with CMake
echo "⚙️ Configuring CMake..."
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Check if CMake configuration was successful
if [ $? -ne 0 ]; then
    echo "❌ CMake configuration failed!"
    exit 1
fi

# Build the project
echo "🔨 Building..."
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Check if build was successful
if [ $? -eq 0 ]; then
    echo "✅ Build successful!"
    echo "📱 You can now run the client:"
    if [[ "$OSTYPE" == "darwin"* ]]; then
        echo "   ./MouffetteClient.app/Contents/MacOS/MouffetteClient"
    else
        echo "   ./MouffetteClient"
    fi
else
    echo "❌ Build failed!"
    exit 1
fi

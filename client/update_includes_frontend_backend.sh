#!/bin/bash
# Update all include paths for frontend/backend separation

echo "🔄 Updating include paths for frontend/backend architecture..."

# Find all source files
find src -type f \( -name "*.h" -o -name "*.cpp" -o -name "*.mm" \) | while read -r file; do
    echo "Processing: $file"
    
    # Frontend paths
    sed -i '' \
        -e 's|#include "ui/pages/|#include "frontend/ui/pages/|g' \
        -e 's|#include "ui/widgets/|#include "frontend/ui/widgets/|g' \
        -e 's|#include "ui/layout/|#include "frontend/ui/layout/|g' \
        -e 's|#include "ui/notifications/|#include "frontend/ui/notifications/|g' \
        -e 's|#include "ui/theme/|#include "frontend/ui/theme/|g' \
        -e 's|#include "rendering/canvas/|#include "frontend/rendering/canvas/|g' \
        -e 's|#include "rendering/remote/|#include "frontend/rendering/remote/|g' \
        -e 's|#include "rendering/navigation/|#include "frontend/rendering/navigation/|g' \
        -e 's|#include "handlers/WindowEventHandler|#include "frontend/handlers/WindowEventHandler|g' \
        -e 's|#include "handlers/UploadSignalConnector|#include "frontend/handlers/UploadSignalConnector|g' \
        -e 's|#include "managers/ui/|#include "frontend/managers/ui/|g' \
        -e 's|#include "core/AppColors|#include "frontend/ui/theme/AppColors|g' \
        "$file"
    
    # Backend paths
    sed -i '' \
        -e 's|#include "domain/models/|#include "backend/domain/models/|g' \
        -e 's|#include "domain/session/|#include "backend/domain/session/|g' \
        -e 's|#include "domain/media/|#include "backend/domain/media/|g' \
        -e 's|#include "network/|#include "backend/network/|g' \
        -e 's|#include "files/|#include "backend/files/|g' \
        -e 's|#include "controllers/|#include "backend/controllers/|g' \
        -e 's|#include "platform/macos/|#include "backend/platform/macos/|g' \
        -e 's|#include "platform/windows/|#include "backend/platform/windows/|g' \
        -e 's|#include "handlers/WebSocketMessageHandler|#include "backend/handlers/WebSocketMessageHandler|g' \
        -e 's|#include "handlers/ScreenEventHandler|#include "backend/handlers/ScreenEventHandler|g' \
        -e 's|#include "handlers/ClientListEventHandler|#include "backend/handlers/ClientListEventHandler|g' \
        -e 's|#include "handlers/UploadEventHandler|#include "backend/handlers/UploadEventHandler|g' \
        -e 's|#include "managers/app/|#include "backend/managers/app/|g' \
        -e 's|#include "managers/network/|#include "backend/managers/network/|g' \
        -e 's|#include "managers/system/|#include "backend/managers/system/|g' \
        "$file"
done

echo "✅ Include paths updated!"

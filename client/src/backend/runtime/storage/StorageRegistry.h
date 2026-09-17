#pragma once

#include "StorageUpgradeEngine.h"
#include "backend/runtime/RuntimeProfile.h"

namespace RuntimeStorage {
QList<Component> components(const RuntimeProfileContext& context);
Operation purgeReceivedMedia(const RuntimeProfileContext& context);
}

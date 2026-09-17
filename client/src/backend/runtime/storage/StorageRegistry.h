#pragma once

#include "StorageUpgradeEngine.h"
#include "backend/runtime/RuntimeProfile.h"

namespace RuntimeStorage {
QList<Component> components(const RuntimeProfileContext& context);
Operation purgeReceivedMedia(const RuntimeProfileContext& context);
// Explicit user action only. All services/readers and the profile lock must
// have been released; retain the instance coordination lock until process exit.
Operation clearProfileStorage(const RuntimeProfileContext& context);
}

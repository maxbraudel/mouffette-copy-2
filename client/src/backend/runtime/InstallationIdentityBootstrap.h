#pragma once

#include "backend/runtime/RuntimeProfile.h"
#include "backend/runtime/storage/StorageUpgradeEngine.h"

// One durable installation identity per channel, shared by all process slots.
// This transaction never resets an existing private key.
class InstallationIdentityBootstrap final {
public:
    static bool prepare(const RuntimeProfileContext& context, QString* error = nullptr,
                        RuntimeStorage::Report* report = nullptr);
};

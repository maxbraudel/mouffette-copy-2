#pragma once
#include "backend/runtime/storage/StorageUpgradeEngine.h"

namespace RuntimeStorage::Migrations {
Operation settingsV0ToV1(const QString& root, const QString& path);
}

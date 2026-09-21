#pragma once
#include "backend/runtime/storage/StorageUpgradeEngine.h"

namespace RuntimeStorage::Migrations {
Operation historyV1ToV2(const QString& root, const QString& path);
}

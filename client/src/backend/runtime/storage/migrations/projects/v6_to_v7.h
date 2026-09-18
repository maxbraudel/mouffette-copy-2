#pragma once
#include "backend/runtime/storage/StorageUpgradeEngine.h"

namespace RuntimeStorage::Migrations {
Operation projectsV6ToV7(const QString& root, const QString& path);
}

#include "v0_to_v1.h"
#include "backend/runtime/storage/StorageIO.h"

namespace RuntimeStorage::Migrations {
Operation settingsV0ToV1(const QString& root, const QString& path)
{
    const SettingsData old = readSettings(root, path);
    if (old.inspection.state == State::IoError)
        return {Failure::IoError, old.inspection.reason};
    if (old.inspection.version != 0 || old.inspection.state != State::Incompatible)
        return {Failure::InvalidData, QStringLiteral("Invalid unversioned settings")};
    // Freeze the destination version: future bumps add new transition files.
    return writeSettings(root, path, old.values, 1);
}
}

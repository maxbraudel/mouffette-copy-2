#ifndef PROJECTSTORE_H
#define PROJECTSTORE_H

#include "backend/domain/project/ProjectModel.h"
#include "backend/runtime/storage/StorageVersions.h"
#include "backend/runtime/storage/StorageUpgradeEngine.h"

#include <QList>
#include <QString>

/** Atomic, versioned JSON persistence for local projects. */
class ProjectStore {
public:
    static constexpr int SchemaVersion = StorageVersions::Projects;

    explicit ProjectStore(QString filePath = defaultFilePath());

    static QString defaultFilePath();
    QString filePath() const { return m_filePath; }
    QString lastError() const { return m_lastError; }
    RuntimeStorage::Failure lastReadFailure() const { return m_readFailure; }

    bool load(QList<ProjectRecord>* projects);
    bool save(const QList<ProjectRecord>& projects);

private:
    QString m_filePath;
    QString m_lastError;
    RuntimeStorage::Failure m_readFailure = RuntimeStorage::Failure::None;
};

#endif // PROJECTSTORE_H

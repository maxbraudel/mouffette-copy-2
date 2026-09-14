#ifndef PROJECTSTORE_H
#define PROJECTSTORE_H

#include "backend/domain/project/ProjectModel.h"

#include <QList>
#include <QString>

/** Atomic, versioned JSON persistence for local projects. */
class ProjectStore {
public:
    static constexpr int SchemaVersion = 3;

    explicit ProjectStore(QString filePath = defaultFilePath());

    static QString defaultFilePath();
    QString filePath() const { return m_filePath; }
    QString lastError() const { return m_lastError; }

    bool load(QList<ProjectRecord>* projects);
    bool save(const QList<ProjectRecord>& projects);

private:
    QString m_filePath;
    QString m_lastError;
};

#endif // PROJECTSTORE_H

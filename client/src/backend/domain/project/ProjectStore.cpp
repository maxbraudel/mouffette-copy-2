#include "backend/domain/project/ProjectStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <utility>

ProjectStore::ProjectStore(QString filePath)
    : m_filePath(std::move(filePath))
{
}

QString ProjectStore::defaultFilePath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.mouffette");
    }
    return QDir(base).filePath(QStringLiteral("projects-v1.json"));
}

bool ProjectStore::load(QList<ProjectRecord>* projects)
{
    m_lastError.clear();
    if (!projects) {
        m_lastError = QStringLiteral("Missing projects output");
        return false;
    }
    projects->clear();

    QFile file(m_filePath);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = QStringLiteral("Cannot open project store: %1").arg(file.errorString());
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        m_lastError = QStringLiteral("Invalid project store JSON: %1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schemaVersion")).toInt(-1) != SchemaVersion) {
        m_lastError = QStringLiteral("Unsupported project store schema version");
        return false;
    }
    const QJsonValue recordsValue = root.value(QStringLiteral("projects"));
    if (!recordsValue.isArray()) {
        m_lastError = QStringLiteral("Project store is missing its projects array");
        return false;
    }

    QSet<QString> projectIds;
    QSet<QString> targetDeviceIds;
    QList<ProjectRecord> parsed;
    for (const QJsonValue& value : recordsValue.toArray()) {
        ProjectRecord project;
        QString error;
        if (!value.isObject() || !ProjectRecord::fromJson(value.toObject(), &project, &error)) {
            m_lastError = QStringLiteral("Invalid project record: %1").arg(error);
            return false;
        }
        if (projectIds.contains(project.projectId) || targetDeviceIds.contains(project.targetDeviceId)) {
            m_lastError = QStringLiteral("Project store contains duplicate project or target identifiers");
            return false;
        }
        projectIds.insert(project.projectId);
        targetDeviceIds.insert(project.targetDeviceId);
        parsed.append(project);
    }

    *projects = parsed;
    return true;
}

bool ProjectStore::save(const QList<ProjectRecord>& projects)
{
    m_lastError.clear();
    QSet<QString> projectIds;
    QSet<QString> targetDeviceIds;
    QJsonArray records;
    for (const ProjectRecord& project : projects) {
        if (!project.isValid()
            || projectIds.contains(project.projectId)
            || targetDeviceIds.contains(project.targetDeviceId)) {
            m_lastError = QStringLiteral("Refusing to persist invalid or duplicate project data");
            return false;
        }
        projectIds.insert(project.projectId);
        targetDeviceIds.insert(project.targetDeviceId);
        records.append(project.toJson());
    }

    const QFileInfo info(m_filePath);
    QDir directory = info.dir();
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        m_lastError = QStringLiteral("Cannot create project store directory");
        return false;
    }

    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), SchemaVersion);
    root.insert(QStringLiteral("projects"), records);

    QSaveFile file(m_filePath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        m_lastError = QStringLiteral("Cannot open project store for writing: %1").arg(file.errorString());
        return false;
    }
    const QByteArray encoded = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (file.write(encoded) != encoded.size()) {
        m_lastError = QStringLiteral("Cannot write project store: %1").arg(file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        m_lastError = QStringLiteral("Cannot atomically commit project store: %1").arg(file.errorString());
        return false;
    }
    return true;
}

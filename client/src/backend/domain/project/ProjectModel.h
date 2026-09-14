#ifndef PROJECTMODEL_H
#define PROJECTMODEL_H

#include "backend/domain/models/ClientInfo.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

enum class ProjectLifecycleState {
    Visible,
    Hidden,
    Deleted
};

QString projectLifecycleStateToString(ProjectLifecycleState state);
bool projectLifecycleStateFromString(const QString& value, ProjectLifecycleState* state);

/** Stable descriptive reference to the endpoint owning this project. */
struct ProjectTargetReference {
    QString endpointId;
    QString machineName;
    QString platform;

    bool isValid() const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject& json, ProjectTargetReference* target,
                         QString* error = nullptr);

    static ProjectTargetReference fromClientInfo(const ClientInfo& client);
    ClientInfo toClientInfo(bool online) const;
};

/**
 * Durable reference to a local source. Upload state deliberately does not
 * belong here: every restored project starts with remote assets unavailable.
 */
struct ProjectMediaReference {
    QString mediaId;
    QString assetId;
    QString canonicalSourcePath;
    QString sourceIdentity;
    QString sha256;
    QString mediaType;

    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject& json, ProjectMediaReference* reference, QString* error = nullptr);
};

struct ProjectRecord {
    QString projectId;
    QString targetEndpointId;
    ProjectTargetReference target;
    QList<ScreenInfo> savedScreens;
    ProjectLifecycleState state = ProjectLifecycleState::Hidden;
    qint64 createdAtMs = -1;
    qint64 updatedAtMs = -1;
    qint64 hiddenAtMs = -1;
    qint64 lastCheckpointAtMs = -1;
    QJsonObject canvasState;
    QList<ProjectMediaReference> mediaReferences;

    bool isValid() const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject& json, ProjectRecord* project, QString* error = nullptr);

    // Produces runtime state without leaking transient session/upload fields
    // into the durable representation. Videos are always restored paused and
    // every file-backed media item is marked as not uploaded.
    QJsonObject canvasStateForRestore() const;
};

struct ProjectClientEntry {
    ClientInfo client;
    QString endpointId;
    QString projectId;
    bool hasProject = false;
    bool online = false;
    ProjectLifecycleState projectState = ProjectLifecycleState::Deleted;
    qint64 projectDeleteAtMs = -1;
};

Q_DECLARE_METATYPE(ProjectRecord)
Q_DECLARE_METATYPE(ProjectClientEntry)
Q_DECLARE_METATYPE(ProjectLifecycleState)

#endif // PROJECTMODEL_H

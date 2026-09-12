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

/**
 * Durable copy of the last state advertised by a remote installation.
 * The websocket/server id is informational only; targetDeviceId remains the
 * sole key used to reconcile a returning device.
 */
struct ClientSnapshot {
    QString deviceId;
    QString serverConnectionId;
    QString machineName;
    QString platform;
    QString status;
    QList<ScreenInfo> screens;
    int volumePercent = -1;
    qint64 lastSeenAtMs = -1;

    bool isValid() const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject& json, ClientSnapshot* snapshot, QString* error = nullptr);

    static ClientSnapshot fromClientInfo(const ClientInfo& client, qint64 seenAtMs);
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
    QString targetDeviceId;
    ClientSnapshot clientSnapshot;
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
    QString deviceId;
    QString projectId;
    bool hasProject = false;
    bool online = false;
    ProjectLifecycleState projectState = ProjectLifecycleState::Deleted;
    qint64 remoteSessionCloseAtMs = -1;
    qint64 projectDeleteAtMs = -1;
};

Q_DECLARE_METATYPE(ProjectRecord)
Q_DECLARE_METATYPE(ProjectClientEntry)
Q_DECLARE_METATYPE(ProjectLifecycleState)

#endif // PROJECTMODEL_H

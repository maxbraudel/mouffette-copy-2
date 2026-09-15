#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include "backend/network/RemoteSessionCoordinator.h"

// Protocol-v4 scene state which is deliberately independent from a canvas.
// Remote sessions own transport/cache lifetime; a SceneRun only owns one
// immutable render revision inside such a session.
class SceneRunCoordinator final : public QObject
{
    Q_OBJECT

public:
    enum class Phase {
        Draft,
        Preparing,
        Prepared,
        Armed,
        Scheduled,
        Live,
        Stopping,
        Stopped,
        Failed
    };
    Q_ENUM(Phase)

    using SessionBinding = RemoteSessionCoordinator::Binding;

    struct Run {
        QString remoteSessionId;
        quint64 generation = 0;
        QString sceneRunId;
        quint64 revision = 0;
        QString digest;
        QString ownerEndpointId;
        QString targetEndpointId;
        QJsonArray manifest;
        QJsonObject scene;
        Phase phase = Phase::Draft;
        qint64 prepareDeadlineEpochMs = 0;
        qint64 startEpochMs = 0;
        qint64 startServerMonotonicMs = 0;
        quint64 lastSnapshotSequence = 0;
    };

    explicit SceneRunCoordinator(QObject* parent = nullptr);

    void setLocalEndpointId(const QString& endpointId);
    void setPrepareTimeoutMs(int timeoutMs);
    bool upsertSession(const QJsonObject& sessionEnvelope,
                       quint64 localConnectionGeneration = 0);
    bool removeSession(const QJsonObject& closedEnvelope,
                       quint64 localConnectionGeneration = 0);
    // Used only after an authenticated server error proves that the retained
    // local session no longer exists or belongs to this runtime. This cannot
    // manufacture a wire-level Closed acknowledgement.
    bool discardSessionAfterAuthoritativeRejection(
        const QString& remoteSessionId);
    void clearSessions();

    SessionBinding sessionForPeer(const QString& peerEndpointId) const;
    SessionBinding sessionById(const QString& remoteSessionId) const;
    QList<SessionBinding> sessions() const;
    RemoteSessionCoordinator* remoteSessions() const { return m_remoteSessions; }
    Run run(const QString& sceneRunId) const;

    bool createOutgoingRun(const QString& peerEndpointId,
                           quint64 revision,
                           const QJsonArray& manifest,
                           const QJsonObject& scene,
                           Run* result,
                           QString* errorMessage = nullptr);
    bool acceptInboundEnvelope(const QJsonObject& envelope,
                               QString* errorMessage = nullptr);
    void finishRun(const QString& sceneRunId, bool failed);

    static QJsonArray normalizeManifest(const QJsonArray& manifest,
                                        QString* errorMessage = nullptr);
    static QByteArray canonicalJson(const QJsonValue& value);
    static QString computeDigest(quint64 revision,
                                 const QJsonArray& normalizedManifest,
                                 const QJsonObject& scene);
    // Keep local runtime identities separate from the strict wire checklist,
    // which only permits itemId, stage and ready. Screen entries have no media.
    static QJsonArray createLocalChecklist(
        const QJsonObject& scene,
        QHash<QString, QString>* mediaIdsByItemId = nullptr);
    static QString phaseName(Phase phase);

signals:
    void runChanged(const QString& sceneRunId, SceneRunCoordinator::Phase phase);

private:
    static bool isOpaqueId(const QString& value);
    static bool isSha256(const QString& value);
    static Phase phaseFromWire(const QString& value, Phase fallback);
    static bool isLegalTransition(Phase from, Phase to);

    QString m_localEndpointId;
    // Supplied only by welcome.policy; there is intentionally no duplicated
    // client-side protocol timeout default.
    int m_prepareTimeoutMs = 0;
    RemoteSessionCoordinator* m_remoteSessions = nullptr;
    QHash<QString, Run> m_runsById;
};

Q_DECLARE_METATYPE(SceneRunCoordinator::Phase)

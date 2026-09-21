#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

// Memory-only authenticated RemoteSession index. It deliberately stores no
// canvas/project state; its resume token never crosses the persistence layer.
class RemoteSessionCoordinator final : public QObject
{
    Q_OBJECT

public:
    struct Binding {
        QString remoteSessionId;
        quint64 generation = 0;
        quint64 stateRevision = 0;
        qint64 validUntilServerMonotonicMs = -1;
        quint64 ownerConnectionGeneration = 0;
        quint64 targetConnectionGeneration = 0;
        QString ownerEndpointId;
        QString targetEndpointId;
        QString resumeToken;
        QString teardownId;
        QString phase;
        bool active = false;
        bool commandReady = false;
        bool degraded = false;
    };

    explicit RemoteSessionCoordinator(QObject* parent = nullptr);

    void setLocalEndpointId(const QString& endpointId);
    QString localEndpointId() const { return m_localEndpointId; }
    bool upsert(const QJsonObject& envelope,
                quint64 localConnectionGeneration = 0,
                QString* validationError = nullptr);
    bool acceptSnapshot(const QJsonObject& envelope,
                        quint64 localConnectionGeneration = 0,
                        bool allowInitialReplay = false);
    // An accepted idempotent replay may contain an older initial snapshot.
    // Consumers must project the latest accepted reading, not that replay.
    QJsonObject latestSnapshot(const QString& remoteSessionId) const {
        return m_latestSnapshotBySession.value(remoteSessionId);
    }
    static bool validateSnapshot(const QJsonObject& snapshot);
    bool isClosedDuplicate(const QJsonObject& envelope) const;
    void suspend(const QString& remoteSessionId);
    bool canClose(const QJsonObject& envelope,
                  quint64 localConnectionGeneration = 0) const;
    void remove(const QString& remoteSessionId, const QJsonObject& finalEnvelope = {});
    void clear();

    // A device can simultaneously be the target of our outgoing session and
    // the owner of its own incoming session. Keep those role indexes separate
    // so the reverse direction cannot evict or alias the first binding.
    Binding outgoingForPeer(const QString& peerEndpointId) const;
    Binding incomingForPeer(const QString& peerEndpointId) const;
    // Role-agnostic lookup for observers. Outgoing is selected deterministically
    // when both directions exist.
    Binding forPeer(const QString& peerEndpointId) const;
    Binding byId(const QString& remoteSessionId) const;
    QList<Binding> all() const;

signals:
    void sessionChanged(const QString& remoteSessionId, quint64 generation,
                        const QString& phase);
    void sessionRemoved(const QString& remoteSessionId);

private:
    static bool isEndpointId(const QString& value);
    static bool isOpaqueId(const QString& value);
    static bool isAllowedPhase(const QString& phase);
    static bool isAllowedSameGenerationTransition(const QString& from,
                                                  const QString& to);

    QString m_localEndpointId;
    QHash<QString, Binding> m_byId;
    QHash<QString, QString> m_outgoingIdByPeer;
    QHash<QString, QString> m_incomingIdByPeer;
    QHash<QString, quint64> m_lastSnapshotSequenceBySession;
    QHash<QString, quint64> m_lastSnapshotRevisionBySession;
    QSet<QString> m_closedSessionIds;
    QList<QString> m_closedSessionOrder;
    QHash<QString, Binding> m_closedBindings;
    QHash<QString, QJsonObject> m_initialSnapshotBySession;
    QHash<QString, QJsonObject> m_latestSnapshotBySession;
};

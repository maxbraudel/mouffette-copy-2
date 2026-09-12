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
        quint64 ownerConnectionGeneration = 0;
        quint64 targetConnectionGeneration = 0;
        QString ownerDeviceId;
        QString targetDeviceId;
        QString resumeToken;
        QString teardownId;
        QString phase;
        bool active = false;
    };

    explicit RemoteSessionCoordinator(QObject* parent = nullptr);

    void setLocalDeviceId(const QString& deviceId);
    QString localDeviceId() const { return m_localDeviceId; }
    bool upsert(const QJsonObject& envelope,
                quint64 localConnectionGeneration = 0);
    bool canClose(const QJsonObject& envelope,
                  quint64 localConnectionGeneration = 0) const;
    void remove(const QString& remoteSessionId);
    void clear();

    // A device can simultaneously be the target of our outgoing session and
    // the owner of its own incoming session. Keep those role indexes separate
    // so the reverse direction cannot evict or alias the first binding.
    Binding outgoingForPeer(const QString& peerDeviceId) const;
    Binding incomingForPeer(const QString& peerDeviceId) const;
    // Compatibility lookup for role-agnostic observers. Outgoing is selected
    // deterministically when both directions exist.
    Binding forPeer(const QString& peerDeviceId) const;
    Binding byId(const QString& remoteSessionId) const;
    QList<Binding> all() const;

signals:
    void sessionChanged(const QString& remoteSessionId, quint64 generation,
                        const QString& phase);
    void sessionRemoved(const QString& remoteSessionId);

private:
    static bool isDeviceId(const QString& value);
    static bool isOpaqueId(const QString& value);
    static bool isAllowedPhase(const QString& phase);
    static bool isAllowedSameGenerationTransition(const QString& from,
                                                  const QString& to);

    QString m_localDeviceId;
    QHash<QString, Binding> m_byId;
    QHash<QString, QString> m_outgoingIdByPeer;
    QHash<QString, QString> m_incomingIdByPeer;
    QSet<QString> m_closedSessionIds;
};

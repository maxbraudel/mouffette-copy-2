#ifndef CLIENTLISTMODEL_H
#define CLIENTLISTMODEL_H

#include <QAbstractListModel>
#include <QList>
#include <QSet>

#include "backend/domain/models/ClientInfo.h"

class QTimer;

class ClientListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        EndpointIdRole = Qt::UserRole + 1,
        PrimaryTextRole,
        SecondaryTextRole,
        BadgeTextRole,
        BadgeKindRole,
        SelectableRole,
        OnlineRole,
        PlatformRole,
        ProjectIdRole,
        HasProjectRole,
        IdentifierRole
    };

    explicit ClientListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setClients(const QList<ClientInfo>& clients);
    ClientInfo client(const QString& endpointId) const;
    QList<ClientInfo> clients() const { return m_clients; }

private:
    static int badgeKind(const QString& status);
    static QString endpointIdFor(const ClientInfo& client);
    QSet<QString> activeCountdownEndpointIds(qint64 nowMs) const;
    void refreshCountdowns();
    void updateCountdownTimer();

    QList<ClientInfo> m_clients;
    QTimer* m_countdownTimer = nullptr;
    QSet<QString> m_countdownEndpointIds;
};

#endif // CLIENTLISTMODEL_H

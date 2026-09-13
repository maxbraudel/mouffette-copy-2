#ifndef CLIENTLISTMODEL_H
#define CLIENTLISTMODEL_H

#include <QAbstractListModel>
#include <QList>

#include "backend/domain/models/ClientInfo.h"

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

    QList<ClientInfo> m_clients;
};

#endif // CLIENTLISTMODEL_H

#include "frontend/qml/ClientListModel.h"

#include <QDateTime>

ClientListModel::ClientListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ClientListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_clients.size();
}

QVariant ClientListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_clients.size()) {
        return {};
    }

    const ClientInfo& client = m_clients.at(index.row());
    const QString endpointId = client.endpointId().isEmpty()
        ? client.getId() : client.endpointId();
    const QString status = client.availabilityBadgeText();
    switch (role) {
    case EndpointIdRole: return endpointId;
    case PrimaryTextRole: return client.getIdentityDisplayText();
    case SecondaryTextRole:
        return client.getProjectSummaryText(QDateTime::currentMSecsSinceEpoch());
    case BadgeTextRole: return status;
    case BadgeKindRole: return badgeKind(status);
    case SelectableRole: return true;
    case OnlineRole: return client.isOnline();
    case PlatformRole: return client.getPlatform();
    case ProjectIdRole: return client.projectId();
    case IdentifierRole: return endpointId;
    default: return {};
    }
}

QHash<int, QByteArray> ClientListModel::roleNames() const
{
    return {
        { EndpointIdRole, QByteArrayLiteral("endpointId") },
        { PrimaryTextRole, QByteArrayLiteral("primaryText") },
        { SecondaryTextRole, QByteArrayLiteral("secondaryText") },
        { BadgeTextRole, QByteArrayLiteral("badgeText") },
        { BadgeKindRole, QByteArrayLiteral("badgeKind") },
        { SelectableRole, QByteArrayLiteral("selectable") },
        { OnlineRole, QByteArrayLiteral("online") },
        { PlatformRole, QByteArrayLiteral("platform") },
        { ProjectIdRole, QByteArrayLiteral("projectId") },
        { IdentifierRole, QByteArrayLiteral("identifier") }
    };
}

void ClientListModel::setClients(const QList<ClientInfo>& clients)
{
    beginResetModel();
    m_clients = clients;
    endResetModel();
}

ClientInfo ClientListModel::client(const QString& endpointId) const
{
    for (const ClientInfo& candidate : m_clients) {
        const QString candidateId = candidate.endpointId().isEmpty()
            ? candidate.getId() : candidate.endpointId();
        if (candidateId == endpointId) {
            return candidate;
        }
    }
    return {};
}

int ClientListModel::badgeKind(const QString& status)
{
    if (status == QLatin1String("Available")
        || status == QLatin1String("Connected")
        || status == QLatin1String("Live")) {
        return 0;
    }
    if (status == QLatin1String("Connecting")
        || status == QLatin1String("Reconnecting")
        || status == QLatin1String("Degraded")) {
        return 1;
    }
    return 2;
}

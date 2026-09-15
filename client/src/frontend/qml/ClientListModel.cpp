#include "frontend/qml/ClientListModel.h"

#include "backend/config/AppConfig.h"

#include <QDateTime>
#include <QTimer>

ClientListModel::ClientListModel(QObject* parent)
    : QAbstractListModel(parent)
    , m_countdownTimer(new QTimer(this))
{
    m_countdownTimer->setObjectName(QStringLiteral("clientCountdownRefreshTimer"));
    m_countdownTimer->setTimerType(Qt::PreciseTimer);
    m_countdownTimer->setInterval(
        AppConfig::instance().clientCountdownRefreshIntervalMs());
    connect(m_countdownTimer, &QTimer::timeout,
            this, &ClientListModel::refreshCountdowns);
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
    const QString endpointId = endpointIdFor(client);
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
    case HasProjectRole: return client.hasProject();
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
        { HasProjectRole, QByteArrayLiteral("hasProject") },
        { IdentifierRole, QByteArrayLiteral("identifier") }
    };
}

void ClientListModel::setClients(const QList<ClientInfo>& clients)
{
    beginResetModel();
    m_clients = clients;
    endResetModel();
    updateCountdownTimer();
}

ClientInfo ClientListModel::client(const QString& endpointId) const
{
    for (const ClientInfo& candidate : m_clients) {
        const QString candidateId = endpointIdFor(candidate);
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

QString ClientListModel::endpointIdFor(const ClientInfo& client)
{
    return client.endpointId().isEmpty() ? client.getId() : client.endpointId();
}

QSet<QString> ClientListModel::activeCountdownEndpointIds(qint64 nowMs) const
{
    QSet<QString> result;
    for (const ClientInfo& client : m_clients) {
        const bool sessionCountdown = client.remoteSessionCloseAtMs() > nowMs;
        const bool projectCountdown = client.projectDeleteAtMs() > nowMs;
        if (sessionCountdown || projectCountdown) {
            result.insert(endpointIdFor(client));
        }
    }
    return result;
}

void ClientListModel::refreshCountdowns()
{
    const QSet<QString> previous = m_countdownEndpointIds;
    const QSet<QString> current = activeCountdownEndpointIds(
        QDateTime::currentMSecsSinceEpoch());

    for (int row = 0; row < m_clients.size(); ++row) {
        const QString endpointId = endpointIdFor(m_clients.at(row));
        if (previous.contains(endpointId) || current.contains(endpointId)) {
            emit dataChanged(index(row), index(row), {SecondaryTextRole});
        }
    }

    m_countdownEndpointIds = current;
    if (m_countdownEndpointIds.isEmpty()) {
        m_countdownTimer->stop();
    }
}

void ClientListModel::updateCountdownTimer()
{
    m_countdownEndpointIds = activeCountdownEndpointIds(
        QDateTime::currentMSecsSinceEpoch());
    if (m_countdownEndpointIds.isEmpty()) {
        m_countdownTimer->stop();
    } else if (!m_countdownTimer->isActive()) {
        m_countdownTimer->start();
    }
}

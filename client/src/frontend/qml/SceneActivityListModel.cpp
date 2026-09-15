#include "frontend/qml/SceneActivityListModel.h"

#include "backend/config/AppConfig.h"
#include "frontend/qml/ClientListModel.h"

#include <QDateTime>

SceneActivityListModel::SceneActivityListModel(QObject* parent)
    : QAbstractListModel(parent)
{
    m_durationTimer.setInterval(
        AppConfig::instance().sceneActivityRefreshIntervalMs());
    m_durationTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_durationTimer, &QTimer::timeout, this, [this]() {
        if (!m_rows.isEmpty()) {
            emit dataChanged(index(0), index(m_rows.size() - 1),
                             { SecondaryTextRole });
        }
    });
    m_durationTimer.start();
}

int SceneActivityListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant SceneActivityListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const SceneActivityModel::Activity& activity = m_rows.at(index.row());
    const bool outgoing = activity.direction == SceneActivityModel::Direction::Outgoing;
    const QString peer = peerDisplayName(activity.peerEndpointId);
    switch (role) {
    case SceneRunIdRole: return activity.sceneRunId;
    case PrimaryTextRole:
        return outgoing ? QStringLiteral("Sent to %1").arg(peer)
                        : QStringLiteral("Received from %1").arg(peer);
    case SecondaryTextRole: {
        QString value = QStringLiteral("Started %1 · %2")
            .arg(QDateTime::fromMSecsSinceEpoch(activity.startedAtEpochMs)
                     .toString(QStringLiteral("HH:mm:ss")),
                 formatDuration(QDateTime::currentMSecsSinceEpoch()
                                - activity.startedAtEpochMs));
        if (activity.degraded) value += QStringLiteral(" · Network degraded");
        return value;
    }
    case BadgeTextRole: return activity.degraded ? QStringLiteral("Degraded")
                                                 : QStringLiteral("Live");
    case BadgeKindRole: return activity.degraded ? 1 : 0;
    case SelectableRole: return true;
    case DirectionRole: return outgoing ? 0 : 1;
    case PeerEndpointIdRole: return activity.peerEndpointId;
    case StartedAtRole: return activity.startedAtEpochMs;
    case DegradedRole: return activity.degraded;
    case IdentifierRole: return activity.sceneRunId;
    default: return {};
    }
}

QHash<int, QByteArray> SceneActivityListModel::roleNames() const
{
    return {
        { SceneRunIdRole, QByteArrayLiteral("sceneRunId") },
        { PrimaryTextRole, QByteArrayLiteral("primaryText") },
        { SecondaryTextRole, QByteArrayLiteral("secondaryText") },
        { BadgeTextRole, QByteArrayLiteral("badgeText") },
        { BadgeKindRole, QByteArrayLiteral("badgeKind") },
        { SelectableRole, QByteArrayLiteral("selectable") },
        { DirectionRole, QByteArrayLiteral("direction") },
        { PeerEndpointIdRole, QByteArrayLiteral("peerEndpointId") },
        { StartedAtRole, QByteArrayLiteral("startedAt") },
        { DegradedRole, QByteArrayLiteral("degraded") },
        { IdentifierRole, QByteArrayLiteral("identifier") }
    };
}

void SceneActivityListModel::setSource(SceneActivityModel* source)
{
    if (m_source == source) return;
    if (m_source) disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (m_source) {
        connect(m_source, &SceneActivityModel::activitiesChanged,
                this, &SceneActivityListModel::reload);
    }
    reload();
}

void SceneActivityListModel::setClientsModel(ClientListModel* clientsModel)
{
    m_clientsModel = clientsModel;
    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0), index(m_rows.size() - 1),
                         { PrimaryTextRole });
    }
}

void SceneActivityListModel::reload()
{
    beginResetModel();
    m_rows = m_source ? m_source->liveActivities()
                      : QList<SceneActivityModel::Activity>{};
    endResetModel();
}

QString SceneActivityListModel::peerDisplayName(const QString& endpointId) const
{
    if (m_clientsModel) {
        const ClientInfo client = m_clientsModel->client(endpointId);
        if (!client.getMachineName().trimmed().isEmpty()) {
            return client.getMachineName().trimmed();
        }
    }
    const QString abbreviated = endpointId.left(8);
    return abbreviated.isEmpty() ? QStringLiteral("Unknown device")
                                 : QStringLiteral("Device %1").arg(abbreviated);
}

QString SceneActivityListModel::formatDuration(qint64 elapsedMs)
{
    const qint64 totalSeconds = qMax<qint64>(0, elapsedMs) / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds / 60) % 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours).arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

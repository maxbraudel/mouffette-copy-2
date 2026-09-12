/**
 * @file ClientListPage.cpp
 * @brief Implementation of the client list page
 */

#include "frontend/ui/pages/ClientListPage.h"
#include "frontend/ui/widgets/ClientListDelegate.h"
#include "frontend/ui/theme/ThemeManager.h"
#include "frontend/ui/theme/AppColors.h"
#include "frontend/ui/theme/StyleConfig.h"
#include "backend/domain/models/ClientInfo.h"
#include "backend/domain/scene/SceneActivityModel.h"
#include <QDateTime>
#include <QListWidgetItem>
#include <QTimer>
#include <algorithm>

namespace {
QString formatDuration(qint64 elapsedMs)
{
    const qint64 totalSeconds = qMax<qint64>(0, elapsedMs) / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds / 60) % 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(seconds, 2, 10, QLatin1Char('0'));
}
}

ClientListPage::ClientListPage(SceneActivityModel* sceneActivityModel,
                               QWidget* parent)
    : QWidget(parent),
      m_sceneActivityModel(sceneActivityModel),
      m_layout(nullptr),
      m_clientsLabel(nullptr),
      m_clientListWidget(nullptr),
      m_ongoingScenesLabel(nullptr),
      m_ongoingScenesList(nullptr),
      m_countdownTimer(new QTimer(this))
{
    setupUI();
    if (m_sceneActivityModel) {
        connect(m_sceneActivityModel, &SceneActivityModel::activitiesChanged,
                this, &ClientListPage::refreshOngoingScenesList);
    }
    m_countdownTimer->setInterval(1000);
    connect(m_countdownTimer, &QTimer::timeout,
            this, &ClientListPage::refreshClientCountdowns);
    m_countdownTimer->start();
    refreshOngoingScenesList();
}

void ClientListPage::setupUI() {
    m_layout = new QVBoxLayout(this);
    m_layout->setSpacing(gInnerContentGap);
    m_layout->setContentsMargins(0, 0, 0, 0);

    m_clientsLabel = new QLabel(QStringLiteral("Clients · 0 authenticated"));
    ThemeManager::instance()->applyTitleText(m_clientsLabel);
    m_layout->addWidget(m_clientsLabel);

    // Client list widget - simple and flexible
    m_clientListWidget = new QListWidget();
    applyListWidgetStyle(m_clientListWidget);
    
    connect(m_clientListWidget, &QListWidget::itemClicked, this, &ClientListPage::onClientItemClicked);
    m_clientListWidget->setFocusPolicy(Qt::NoFocus);
    m_clientListWidget->setMouseTracking(true);
    m_clientListWidget->setItemDelegate(new ClientListSeparatorDelegate(m_clientListWidget));
    
    // Simple size policy: expand in both directions, let Qt handle sizing naturally
    m_clientListWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_clientListWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_clientListWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    
    m_layout->addWidget(m_clientListWidget);

    // Ongoing scenes section mirrors client list styling
    m_ongoingScenesLabel = new QLabel(QStringLiteral("Ongoing Scenes · 0 live"));
    ThemeManager::instance()->applyTitleText(m_ongoingScenesLabel);
    m_layout->addWidget(m_ongoingScenesLabel);

    m_ongoingScenesList = new QListWidget();
    applyListWidgetStyle(m_ongoingScenesList);
    m_ongoingScenesList->setFocusPolicy(Qt::NoFocus);
    m_ongoingScenesList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_ongoingScenesList->setMouseTracking(true);
    m_ongoingScenesList->setItemDelegate(new ClientListSeparatorDelegate(m_ongoingScenesList));
    m_ongoingScenesList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_ongoingScenesList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_ongoingScenesList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_ongoingScenesList, &QListWidget::itemClicked, this, &ClientListPage::onOngoingSceneItemClicked);
    m_layout->addWidget(m_ongoingScenesList);
}

void ClientListPage::applyListWidgetStyle(QListWidget* listWidget) {
    ThemeManager::instance()->applyListWidgetStyle(listWidget);
}

void ClientListPage::ensureClientListPlaceholder() {
    if (!m_clientListWidget) return;
    if (m_clientListWidget->count() == 0) {
        QListWidgetItem* item = new QListWidgetItem("No clients connected. Make sure other devices are running Mouffette and connected to the same server.");
        item->setFlags(Qt::NoItemFlags);
        item->setTextAlignment(Qt::AlignCenter);
        QFont font = item->font(); 
        font.setItalic(true); 
        font.setPointSize(16); 
        item->setFont(font);
        item->setForeground(AppColors::gTextMuted);
        m_clientListWidget->addItem(item);
    }
}

void ClientListPage::ensureOngoingScenesPlaceholder() {
    if (!m_ongoingScenesList) return;
    if (m_ongoingScenesList->count() == 0) {
        QListWidgetItem* item = new QListWidgetItem("No current ongoing scenes.");
        item->setFlags(Qt::NoItemFlags);
        item->setTextAlignment(Qt::AlignCenter);
        QFont font = item->font(); 
        font.setItalic(true); 
        font.setPointSize(16); 
        item->setFont(font);
        item->setForeground(AppColors::gTextMuted);
        m_ongoingScenesList->addItem(item);
    }
}

void ClientListPage::refreshOngoingScenesList() {
    if (!m_ongoingScenesList) return;

    const QString previouslySelectedRun = m_ongoingScenesList->currentItem()
        ? m_ongoingScenesList->currentItem()->data(ClientListRoles::ClientId).toString()
        : QString();

    m_ongoingScenesList->clear();
    const QList<SceneActivityModel::Activity> activities = m_sceneActivityModel
        ? m_sceneActivityModel->liveActivities()
        : QList<SceneActivityModel::Activity>{};
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    for (const SceneActivityModel::Activity& activity : activities) {
        const QString peerName = peerDisplayName(activity.peerDeviceId);
        const bool outgoing = activity.direction == SceneActivityModel::Direction::Outgoing;
        const QString primary = outgoing
            ? QStringLiteral("Sent to %1").arg(peerName)
            : QStringLiteral("Received from %1").arg(peerName);
        const QString startTime = QDateTime::fromMSecsSinceEpoch(activity.startedAtEpochMs)
                                      .toString(QStringLiteral("HH:mm:ss"));
        QString secondary = QStringLiteral("Started %1 · %2")
                                .arg(startTime,
                                     formatDuration(nowMs - activity.startedAtEpochMs));
        if (activity.degraded) {
            secondary += QStringLiteral(" · Network degraded");
        }

        QListWidgetItem* item = new QListWidgetItem(primary + QLatin1Char('\n') + secondary);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        item->setData(ClientListRoles::ClientId, activity.sceneRunId);
        item->setData(ClientListRoles::IsClientRow, true);
        item->setData(ClientListRoles::PrimaryText, primary);
        item->setData(ClientListRoles::AvailabilityStatus,
                      activity.degraded ? QStringLiteral("Degraded")
                                        : QStringLiteral("Live"));
        item->setData(ClientListRoles::SecondaryText, secondary);
        item->setData(Qt::AccessibleTextRole, primary + QStringLiteral(". ") + secondary);
        item->setToolTip(primary + QLatin1Char('\n') + secondary);
        m_ongoingScenesList->addItem(item);
        if (activity.sceneRunId == previouslySelectedRun) {
            m_ongoingScenesList->setCurrentItem(item);
        }
    }

    if (m_ongoingScenesList->count() == 0) {
        ensureOngoingScenesPlaceholder();
    }
    updateSectionTitles();
}

void ClientListPage::updateClientList(const QList<ClientInfo>& clients) {
    if (!m_clientListWidget) {
        m_availableClients = clients;
        return;
    }

    const QListWidgetItem* currentItem = m_clientListWidget->currentItem();
    const QString previouslySelectedId = currentItem
        ? currentItem->data(ClientListRoles::ClientId).toString()
        : QString();

    m_availableClients = clients;
    updateSectionTitles();

    m_clientListWidget->setUpdatesEnabled(false);

    // Remove any placeholder items before rebuilding list
    for (int i = m_clientListWidget->count() - 1; i >= 0; --i) {
        QListWidgetItem* existing = m_clientListWidget->item(i);
        if (existing && existing->flags() == Qt::NoItemFlags) {
            delete m_clientListWidget->takeItem(i);
        }
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (clients.isEmpty()) {
        m_clientListWidget->clear();
        QListWidgetItem* item = new QListWidgetItem("No clients connected. Make sure other devices are running Mouffette and connected to the same server.");
        item->setFlags(Qt::NoItemFlags);
        item->setTextAlignment(Qt::AlignCenter);
        QFont font = item->font();
        font.setItalic(true);
        font.setPointSize(16);
        item->setFont(font);
        item->setForeground(AppColors::gTextMuted);
        m_clientListWidget->addItem(item);
    } else {
    const int existingCount = m_clientListWidget->count();
    const int sharedCount = (existingCount < clients.size()) ? existingCount : static_cast<int>(clients.size());

        for (int i = 0; i < sharedCount; ++i) {
            QListWidgetItem* item = m_clientListWidget->item(i);
            const ClientInfo& client = clients.at(i);
            updateClientItem(item, client, nowMs);
        }

        // Remove excess items if the new list is shorter
        for (int i = existingCount - 1; i >= sharedCount; --i) {
            delete m_clientListWidget->takeItem(i);
        }

        // Append new items if needed
        for (int i = sharedCount; i < clients.size(); ++i) {
            const ClientInfo& client = clients.at(i);
            QListWidgetItem* item = new QListWidgetItem();
            updateClientItem(item, client, nowMs);
            m_clientListWidget->addItem(item);
        }
    }

    // Restore selection if the previously selected client still exists
    if (!clients.isEmpty() && !previouslySelectedId.isEmpty()) {
        for (int i = 0; i < clients.size(); ++i) {
            const QString clientId = clients.at(i).clientId().isEmpty()
                ? clients.at(i).getId()
                : clients.at(i).clientId();
            if (clientId == previouslySelectedId) {
                m_clientListWidget->setCurrentRow(i);
                if (QListWidgetItem* restored = m_clientListWidget->item(i)) {
                    restored->setSelected(true);
                }
                break;
            }
        }
    }

    m_clientListWidget->setUpdatesEnabled(true);
    m_clientListWidget->update();

    refreshOngoingScenesList();
}

void ClientListPage::updateClientItem(QListWidgetItem* item,
                                      const ClientInfo& client,
                                      qint64 nowMs)
{
    if (!item) {
        return;
    }

    const QString clientId = client.clientId().isEmpty()
        ? client.getId()
        : client.clientId();
    const QString secondary = client.getProjectSummaryText(nowMs);
    const QString accessible = secondary.isEmpty()
        ? client.getDisplayText()
        : QStringLiteral("%1\n%2").arg(client.getDisplayText(), secondary);

    item->setText(accessible);
    item->setData(ClientListRoles::ClientId, clientId);
    item->setData(ClientListRoles::IsClientRow, true);
    item->setData(ClientListRoles::PrimaryText, client.getIdentityDisplayText());
    item->setData(ClientListRoles::AvailabilityStatus, client.availabilityBadgeText());
    item->setData(ClientListRoles::SecondaryText, secondary);
    item->setData(Qt::AccessibleTextRole, accessible);
    item->setToolTip(accessible);
    // An offline durable project is still a normal editable row. Connectivity
    // only disables remote actions inside the canvas.
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
}

void ClientListPage::refreshClientCountdowns()
{
    refreshOngoingScenesList();
    if (!m_clientListWidget || m_availableClients.isEmpty()) return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QString selectedId = m_clientListWidget->currentItem()
        ? m_clientListWidget->currentItem()->data(ClientListRoles::ClientId).toString()
        : QString();
    const int count = qMin(m_availableClients.size(), m_clientListWidget->count());
    for (int i = 0; i < count; ++i) {
        QListWidgetItem* item = m_clientListWidget->item(i);
        if (item && item->data(ClientListRoles::IsClientRow).toBool()) {
            updateClientItem(item, m_availableClients.at(i), nowMs);
        }
    }

    if (!selectedId.isEmpty()) {
        for (int i = 0; i < count; ++i) {
            QListWidgetItem* item = m_clientListWidget->item(i);
            if (item && item->data(ClientListRoles::ClientId).toString() == selectedId) {
                m_clientListWidget->setCurrentItem(item);
                item->setSelected(true);
                break;
            }
        }
    }
    m_clientListWidget->viewport()->update();
}

void ClientListPage::setEnabled(bool enabled) {
    if (m_clientListWidget) {
        m_clientListWidget->setEnabled(enabled);
    }
}

void ClientListPage::onClientItemClicked(QListWidgetItem* item) {
    if (!item) return;
    int index = m_clientListWidget->row(item);
    if (index >= 0 && index < m_availableClients.size()) {
        ClientInfo client = m_availableClients[index];
        emit clientClicked(client, index);
    }
}

void ClientListPage::onOngoingSceneItemClicked(QListWidgetItem* item) {
    if (!item) return;
    if (item->flags() == Qt::NoItemFlags) return;

    const QString sceneRunId = item->data(ClientListRoles::ClientId).toString();
    if (!sceneRunId.isEmpty()) {
        emit ongoingSceneClicked(sceneRunId);
    }
}

void ClientListPage::updateSectionTitles()
{
    const int authenticated = static_cast<int>(std::count_if(
        m_availableClients.cbegin(), m_availableClients.cend(),
        [](const ClientInfo& client) { return client.isOnline(); }));
    const int live = m_sceneActivityModel ? m_sceneActivityModel->liveCount() : 0;

    if (m_clientsLabel) {
        m_clientsLabel->setText(QStringLiteral("Clients · %1 authenticated")
                                    .arg(authenticated));
    }
    if (m_ongoingScenesLabel) {
        m_ongoingScenesLabel->setText(QStringLiteral("Ongoing Scenes · %1 live")
                                          .arg(live));
    }
    if (authenticated != m_authenticatedDeviceCount || live != m_liveSceneCount) {
        m_authenticatedDeviceCount = authenticated;
        m_liveSceneCount = live;
        emit summaryCountsChanged(authenticated, live);
    }
}

QString ClientListPage::peerDisplayName(const QString& deviceId) const
{
    for (const ClientInfo& client : m_availableClients) {
        if (client.clientId() != deviceId) continue;
        const QString machineName = client.getMachineName().trimmed();
        if (!machineName.isEmpty()) return machineName;
        break;
    }
    const QString abbreviated = deviceId.left(8);
    return abbreviated.isEmpty()
        ? QStringLiteral("Unknown device")
        : QStringLiteral("Device %1").arg(abbreviated);
}

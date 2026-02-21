/**
 * @file ClientListPage.cpp
 * @brief Implementation of the client list page
 */

#include "frontend/ui/pages/ClientListPage.h"
#include "frontend/ui/widgets/ClientListDelegate.h"
#include "frontend/ui/theme/ThemeManager.h"
#include "frontend/ui/theme/AppColors.h"
#include "frontend/ui/theme/StyleConfig.h"
#include "shared/rendering/ICanvasHost.h"
#include "backend/domain/session/SessionManager.h"
#include "backend/domain/models/ClientInfo.h"
#include <QListWidgetItem>
#include <algorithm>

ClientListPage::ClientListPage(SessionManager* sessionManager, QWidget* parent)
    : QWidget(parent),
      m_sessionManager(sessionManager),
      m_layout(nullptr),
      m_clientListWidget(nullptr),
      m_ongoingScenesLabel(nullptr),
      m_ongoingScenesList(nullptr)
{
    setupUI();
}

void ClientListPage::setupUI() {
    m_layout = new QVBoxLayout(this);
    m_layout->setSpacing(gInnerContentGap);
    m_layout->setContentsMargins(0, 0, 0, 0);

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
    m_ongoingScenesLabel = new QLabel("Ongoing Scenes");
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
    if (!m_ongoingScenesList || !m_sessionManager) return;

    m_ongoingScenesList->clear();

    for (SessionManager::CanvasSession* session : m_sessionManager->getAllSessions()) {
        ICanvasHost* canvas = session->canvas;
        if (!canvas) continue;
        if (!canvas->isRemoteSceneLaunched()) continue;

        QString display = session->lastClientInfo.getDisplayText();
        if (display.trimmed().isEmpty()) {
            display = session->lastClientInfo.getMachineName();
        }
        if (display.trimmed().isEmpty()) {
            display = QStringLiteral("Unnamed client");
        }

        QString itemText = QStringLiteral("%1 — Scene live").arg(display.trimmed());
        QListWidgetItem* item = new QListWidgetItem(itemText);
        item->setFlags(Qt::ItemIsEnabled);
        item->setData(Qt::UserRole, session->persistentClientId);
        item->setData(Qt::UserRole + 1, session->persistentClientId);
        m_ongoingScenesList->addItem(item);
    }

    if (m_ongoingScenesList->count() == 0) {
        ensureOngoingScenesPlaceholder();
    }
}

void ClientListPage::updateClientList(const QList<ClientInfo>& clients) {
    if (!m_clientListWidget) {
        m_availableClients = clients;
        return;
    }

    const int previousRow = m_clientListWidget->currentRow();
    QString previouslySelectedId;
    if (previousRow >= 0 && previousRow < m_availableClients.size()) {
        previouslySelectedId = m_availableClients.at(previousRow).getId();
    }

    m_availableClients = clients;

    m_clientListWidget->setUpdatesEnabled(false);

    // Remove any placeholder items before rebuilding list
    for (int i = m_clientListWidget->count() - 1; i >= 0; --i) {
        QListWidgetItem* existing = m_clientListWidget->item(i);
        if (existing && existing->flags() == Qt::NoItemFlags) {
            delete m_clientListWidget->takeItem(i);
        }
    }

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
            const QString display = client.getDisplayText();
            if (item->text() != display) {
                item->setText(display);
            }
            item->setData(Qt::UserRole, client.getId());
        }

        // Remove excess items if the new list is shorter
        for (int i = existingCount - 1; i >= sharedCount; --i) {
            delete m_clientListWidget->takeItem(i);
        }

        // Append new items if needed
        for (int i = sharedCount; i < clients.size(); ++i) {
            const ClientInfo& client = clients.at(i);
            QListWidgetItem* item = new QListWidgetItem(client.getDisplayText());
            item->setData(Qt::UserRole, client.getId());
            m_clientListWidget->addItem(item);
        }
    }

    // Restore selection if the previously selected client still exists
    if (!clients.isEmpty() && !previouslySelectedId.isEmpty()) {
        for (int i = 0; i < clients.size(); ++i) {
            if (clients.at(i).getId() == previouslySelectedId) {
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

    QString persistentClientId = item->data(Qt::UserRole).toString();
    if (persistentClientId.isEmpty()) {
        persistentClientId = item->data(Qt::UserRole + 1).toString();
    }

    if (!persistentClientId.isEmpty()) {
        emit ongoingSceneClicked(persistentClientId);
    }
}

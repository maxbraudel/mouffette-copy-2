/**
 * @file ClientListPage.cpp
 * @brief Implementation of the client list page
 */

#include "ui/pages/ClientListPage.h"
#include "ui/widgets/ClientListDelegate.h"
#include "managers/ThemeManager.h"
#include "AppColors.h"
#include "ui/StyleConfig.h"
#include "ScreenCanvas.h"
#include "SessionManager.h"
#include "ClientInfo.h"
#include <QListWidgetItem>

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
        ScreenCanvas* canvas = session->canvas;
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
    m_availableClients = clients;
    m_clientListWidget->clear();
    
    if (clients.isEmpty()) {
        // Simple "no clients" message
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
        // Add client items
        for (const ClientInfo& client : clients) {
            QListWidgetItem* item = new QListWidgetItem(client.getDisplayText());
            m_clientListWidget->addItem(item);
        }
    }

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

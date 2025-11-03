#include "backend/managers/app/SystemTrayManager.h"
#include <QIcon>
#include <QPixmap>

SystemTrayManager::SystemTrayManager(QObject* parent)
    : QObject(parent)
{
}

SystemTrayManager::~SystemTrayManager() = default;

void SystemTrayManager::setup() {
    if (m_trayIcon) {
        return; // Already setup
    }
    
    // Create tray icon (no context menu, just click handling)
    m_trayIcon = new QSystemTrayIcon(this);
    
    // Set icon - try to load from resources, fallback to simple icon
    QIcon trayIconIcon(":/icons/mouffette.png");
    if (trayIconIcon.isNull()) {
        // Fallback to simple colored icon
        QPixmap pixmap(16, 16);
        pixmap.fill(Qt::blue);
        trayIconIcon = QIcon(pixmap);
    }
    m_trayIcon->setIcon(trayIconIcon);
    
    // Connect tray icon activation to forward the signal
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &SystemTrayManager::activated);
    
    // Show the tray icon
    m_trayIcon->show();
}

void SystemTrayManager::show() {
    if (m_trayIcon) {
        m_trayIcon->show();
    }
}

void SystemTrayManager::hide() {
    if (m_trayIcon) {
        m_trayIcon->hide();
    }
}

void SystemTrayManager::setToolTip(const QString&) {
    if (m_trayIcon) {
        // Intentionally keep tray icon tooltip blank across platforms
        m_trayIcon->setToolTip(QString());
    }
}

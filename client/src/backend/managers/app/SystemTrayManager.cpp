#include "backend/managers/app/SystemTrayManager.h"
#include <QIcon>
#include <QGuiApplication>

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
    
    QIcon trayIconIcon = QGuiApplication::windowIcon();
#ifdef Q_OS_MACOS
    // The template preserves the logo's cutouts. AppKit chooses its colour
    // for light/dark menu bars, including the highlighted state.
    trayIconIcon = QIcon(QStringLiteral(":/icons/logo/mouffette-tray-macos.png"));
    trayIconIcon.addFile(QStringLiteral(":/icons/logo/mouffette-tray-macos@2x.png"));
    trayIconIcon.setIsMask(true);
#endif
    m_trayIcon->setIcon(trayIconIcon);
    
    // Connect tray icon activation to forward the signal
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
        ActivationReason mapped = ActivationReason::Unknown;
        switch (reason) {
        case QSystemTrayIcon::Trigger: mapped = ActivationReason::Trigger; break;
        case QSystemTrayIcon::DoubleClick: mapped = ActivationReason::DoubleClick; break;
        case QSystemTrayIcon::Context: mapped = ActivationReason::Context; break;
        default: break;
        }
        emit activated(mapped);
    });
    
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

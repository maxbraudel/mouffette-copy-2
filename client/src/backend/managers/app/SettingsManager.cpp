#include "backend/managers/app/SettingsManager.h"
#include "backend/config/AppConfig.h"
#include "MainWindow.h"
#include "backend/network/WebSocketClient.h"
#include "frontend/ui/theme/ThemeManager.h"
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QSettings>
#include <QMessageBox>
#include <QDebug>
#include <algorithm>

// Import helper functions from MainWindow namespace
namespace {
    // Temporary wrapper functions for backward compatibility
    void applyPillBtn(QPushButton* b) {
        ThemeManager::instance()->applyPillButton(b);
    }

    void applyPrimaryBtn(QPushButton* b) {
        ThemeManager::instance()->applyPrimaryButton(b);
    }
    
}

SettingsManager::SettingsManager(MainWindow* mainWindow, WebSocketClient* webSocketClient, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
    , m_webSocketClient(webSocketClient)
    , m_serverUrlConfig(AppConfig::instance().serverUrl())
    , m_autoUploadImportedMedia(AppConfig::instance().autoUploadImportedMedia())
    , m_useQuickCanvasRenderer(AppConfig::instance().useQuickCanvasRenderer())
{
}

void SettingsManager::loadSettings() {
    const AppConfig& config = AppConfig::instance();
    m_serverUrlConfig = config.serverUrl();
    m_autoUploadImportedMedia = config.autoUploadImportedMedia();
    m_useQuickCanvasRenderer = config.useQuickCanvasRenderer();
    m_quickCanvasFlagSource = config.provenance(AppConfig::Key::UseQuickCanvasRenderer);
    
    qDebug() << "SettingsManager: Settings loaded - URL:" << m_serverUrlConfig
             << "Auto-upload:" << m_autoUploadImportedMedia
             << "useQuickCanvasRenderer:" << m_useQuickCanvasRenderer
             << "flagSource:" << m_quickCanvasFlagSource;
}

void SettingsManager::saveSettings() {
    QSettings settings("Mouffette", "Client");
    settings.setValue("serverUrl", m_serverUrlConfig.isEmpty()
                                      ? AppConfig::instance().serverUrl()
                                      : m_serverUrlConfig);
    settings.setValue("autoUploadImportedMedia", m_autoUploadImportedMedia);
    settings.sync();
    
    qDebug() << "SettingsManager: Settings saved";
    emit settingsChanged();
}

void SettingsManager::setServerUrl(const QString& url) {
    QUrl normalized;
    QString error;
    if (!AppConfig::validateServerUrl(url, &normalized, &error)) {
        qWarning().noquote() << error;
        return;
    }
    const QString canonical = normalized.toString(QUrl::FullyEncoded);
    if (m_serverUrlConfig != canonical) {
        m_serverUrlConfig = canonical;
        QSettings settings("Mouffette", "Client");
        settings.setValue("serverUrl", m_serverUrlConfig);
        settings.sync();
        emit serverUrlChanged(canonical);
    }
}

void SettingsManager::setAutoUploadImportedMedia(bool enabled) {
    if (m_autoUploadImportedMedia != enabled) {
        m_autoUploadImportedMedia = enabled;
        QSettings settings("Mouffette", "Client");
        settings.setValue("autoUploadImportedMedia", m_autoUploadImportedMedia);
        settings.sync();
    }
}

void SettingsManager::showSettingsDialog() {
    QDialog dialog(m_mainWindow);
    dialog.setWindowTitle("Settings");
    QVBoxLayout* v = new QVBoxLayout(&dialog);
    QLabel* urlLabel = new QLabel("Server URL");
    QLineEdit* urlEdit = new QLineEdit(&dialog);
    if (m_serverUrlConfig.isEmpty()) m_serverUrlConfig = AppConfig::instance().serverUrl();
    urlEdit->setText(m_serverUrlConfig);
    v->addWidget(urlLabel);
    v->addWidget(urlEdit);

    // Auto-upload imported media checkbox
    QCheckBox* autoUploadChk = new QCheckBox("Upload imported media automatically", &dialog);
    autoUploadChk->setChecked(m_autoUploadImportedMedia);
    v->addSpacing(8);
    v->addWidget(autoUploadChk);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    QPushButton* cancelBtn = ThemeManager::createPillButton("Cancel");
    QPushButton* saveBtn = ThemeManager::createPrimaryButton("Save");
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(saveBtn);
    v->addLayout(btnRow);

    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, this, [this, urlEdit, autoUploadChk, &dialog]() {
        const QString newUrl = urlEdit->text().trimmed();
        if (!newUrl.isEmpty()) {
            QUrl normalized;
            QString validationError;
            if (!AppConfig::validateServerUrl(
                    newUrl, &normalized, &validationError)) {
                QMessageBox::warning(
                    &dialog, QStringLiteral("Invalid Server URL"),
                    validationError);
                return;
            }
            const QString canonical = normalized.toString(QUrl::FullyEncoded);
            bool changed = (canonical != (m_serverUrlConfig.isEmpty()
                                        ? AppConfig::instance().serverUrl()
                                        : m_serverUrlConfig));
            m_serverUrlConfig = canonical;
            if (changed) {
                // Restart connection to apply new server URL
                m_mainWindow->setUserDisconnected(false);
                m_mainWindow->connectToServer();
            }
        }
        m_autoUploadImportedMedia = autoUploadChk->isChecked();
        
        saveSettings();
        dialog.accept();
    });

    dialog.exec();
}

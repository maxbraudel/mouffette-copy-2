#include "managers/app/SettingsManager.h"
#include "MainWindow.h"
#include "network/WebSocketClient.h"
#include "ui/theme/ThemeManager.h"
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QSettings>
#include <QSysInfo>
#include <QHostInfo>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QUuid>
#include <QRegularExpression>
#include <QDebug>

// Import helper functions from MainWindow namespace
namespace {
    // Temporary wrapper functions for backward compatibility
    void applyPillBtn(QPushButton* b) {
        ThemeManager::instance()->applyPillButton(b);
    }

    void applyPrimaryBtn(QPushButton* b) {
        ThemeManager::instance()->applyPrimaryButton(b);
    }
    
    const QString DEFAULT_SERVER_URL = "ws://localhost:3000";
}

SettingsManager::SettingsManager(MainWindow* mainWindow, WebSocketClient* webSocketClient, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
    , m_webSocketClient(webSocketClient)
    , m_serverUrlConfig(DEFAULT_SERVER_URL)
    , m_autoUploadImportedMedia(false)
{
}

void SettingsManager::loadSettings() {
    QSettings settings("Mouffette", "Client");
    m_serverUrlConfig = settings.value("serverUrl", DEFAULT_SERVER_URL).toString();
    m_autoUploadImportedMedia = settings.value("autoUploadImportedMedia", false).toBool();
    
    // Generate or load persistent client ID
    m_persistentClientId = generateOrLoadPersistentClientId();
    
    // Apply to WebSocket client
    if (m_webSocketClient) {
        m_webSocketClient->setPersistentClientId(m_persistentClientId);
    }
    
    qDebug() << "SettingsManager: Settings loaded - URL:" << m_serverUrlConfig
             << "Auto-upload:" << m_autoUploadImportedMedia
             << "Client ID:" << m_persistentClientId;
}

void SettingsManager::saveSettings() {
    QSettings settings("Mouffette", "Client");
    settings.setValue("serverUrl", m_serverUrlConfig.isEmpty() ? DEFAULT_SERVER_URL : m_serverUrlConfig);
    settings.setValue("autoUploadImportedMedia", m_autoUploadImportedMedia);
    settings.sync();
    
    qDebug() << "SettingsManager: Settings saved";
    emit settingsChanged();
}

void SettingsManager::setServerUrl(const QString& url) {
    if (m_serverUrlConfig != url) {
        m_serverUrlConfig = url;
        saveSettings();
        emit serverUrlChanged(url);
    }
}

void SettingsManager::setAutoUploadImportedMedia(bool enabled) {
    if (m_autoUploadImportedMedia != enabled) {
        m_autoUploadImportedMedia = enabled;
        saveSettings();
    }
}

void SettingsManager::showSettingsDialog() {
    QDialog dialog(m_mainWindow);
    dialog.setWindowTitle("Settings");
    QVBoxLayout* v = new QVBoxLayout(&dialog);
    QLabel* urlLabel = new QLabel("Server URL");
    QLineEdit* urlEdit = new QLineEdit(&dialog);
    if (m_serverUrlConfig.isEmpty()) m_serverUrlConfig = DEFAULT_SERVER_URL;
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
    QPushButton* cancelBtn = new QPushButton("Cancel");
    QPushButton* saveBtn = new QPushButton("Save");
    applyPillBtn(cancelBtn);
    applyPrimaryBtn(saveBtn);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(saveBtn);
    v->addLayout(btnRow);

    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, this, [this, urlEdit, autoUploadChk, &dialog]() {
        const QString newUrl = urlEdit->text().trimmed();
        if (!newUrl.isEmpty()) {
            bool changed = (newUrl != (m_serverUrlConfig.isEmpty() ? DEFAULT_SERVER_URL : m_serverUrlConfig));
            m_serverUrlConfig = newUrl;
            if (changed) {
                // Restart connection to apply new server URL
                if (m_webSocketClient && m_webSocketClient->isConnected()) {
                    m_mainWindow->setUserDisconnected(false); // not a manual disconnect
                    m_webSocketClient->disconnect();
                }
                m_mainWindow->connectToServer();
            }
        }
        m_autoUploadImportedMedia = autoUploadChk->isChecked();
        
        saveSettings();
        dialog.accept();
    });

    dialog.exec();
}

// Private helper methods

QString SettingsManager::generateOrLoadPersistentClientId() {
    QSettings settings("Mouffette", "Client");
    
    QString machineId = getMachineId();
    QString sanitizedMachineId = machineId;
    sanitizedMachineId.replace(QRegularExpression("[^A-Za-z0-9_]"), "_");
    
    QString instanceSuffix = getInstanceSuffix();
    QString sanitizedInstanceSuffix = instanceSuffix;
    sanitizedInstanceSuffix.replace(QRegularExpression("[^A-Za-z0-9_]"), "_");
    
    QString installFingerprint = getInstallFingerprint();
    
    QString settingsKey = QStringLiteral("persistentClientId_%1_%2").arg(sanitizedMachineId, installFingerprint);
    if (!sanitizedInstanceSuffix.isEmpty()) {
        settingsKey += QStringLiteral("_%1").arg(sanitizedInstanceSuffix);
    }
    
    QString persistentClientId = settings.value(settingsKey).toString();
    if (persistentClientId.isEmpty()) {
        persistentClientId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(settingsKey, persistentClientId);
        qDebug() << "SettingsManager: Generated new persistent client ID:" << persistentClientId
                 << "using key" << settingsKey << "machineId:" << machineId
                 << "instanceSuffix:" << sanitizedInstanceSuffix;
    } else {
        qDebug() << "SettingsManager: Loaded persistent client ID:" << persistentClientId
                 << "using key" << settingsKey << "machineId:" << machineId
                 << "instanceSuffix:" << sanitizedInstanceSuffix;
    }
    
    return persistentClientId;
}

QString SettingsManager::getMachineId() const {
    QString machineId = QSysInfo::machineUniqueId();
    if (machineId.isEmpty()) {
        machineId = QHostInfo::localHostName();
    }
    if (machineId.isEmpty()) {
        machineId = QStringLiteral("unknown-machine");
    }
    return machineId;
}

QString SettingsManager::getInstanceSuffix() const {
    QString instanceSuffix = qEnvironmentVariable("MOUFFETTE_INSTANCE_SUFFIX");
    if (instanceSuffix.isEmpty()) {
        const QStringList args = QCoreApplication::arguments();
        for (int i = 1; i < args.size(); ++i) {
            const QString& arg = args.at(i);
            if (arg.startsWith("--instance-suffix=")) {
                instanceSuffix = arg.section('=', 1);
                break;
            }
            if (arg == "--instance-suffix" && (i + 1) < args.size()) {
                instanceSuffix = args.at(i + 1);
                break;
            }
        }
    }
    return instanceSuffix;
}

QString SettingsManager::getInstallFingerprint() const {
    const QByteArray installHashBytes = QCryptographicHash::hash(
        QCoreApplication::applicationDirPath().toUtf8(), 
        QCryptographicHash::Sha1
    );
    QString installFingerprint = QString::fromUtf8(installHashBytes.toHex());
    if (installFingerprint.isEmpty()) {
        installFingerprint = QStringLiteral("unknowninstall");
    }
    return installFingerprint.left(16); // keep key compact
}

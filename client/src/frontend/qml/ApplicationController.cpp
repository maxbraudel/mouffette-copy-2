#include "frontend/qml/ApplicationController.h"

#include "backend/runtime/ApplicationRuntime.h"
#include "backend/config/AppConfig.h"
#include "backend/domain/scene/SceneActivityModel.h"
#include "backend/managers/app/SettingsManager.h"
#include "backend/notifications/NotificationCenter.h"
#include "backend/runtime/RuntimeProfile.h"
#include "frontend/qml/CanvasSessionViewModel.h"
#include "frontend/qml/ClientListModel.h"
#include "frontend/qml/NotificationListModels.h"
#include "frontend/qml/SceneActivityListModel.h"
#include "shared/rendering/ICanvasHost.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QTimer>
#include <QUrl>

ApplicationController::ApplicationController(RuntimeProfileContext runtimeProfile,
                                             QStringList arguments,
                                             QObject* parent)
    : QObject(parent)
    , m_runtimeProfile(std::move(runtimeProfile))
    , m_arguments(std::move(arguments))
    , m_storageBootstrap(m_runtimeProfile)
    , m_clientsModel(new ClientListModel(this))
    , m_sceneActivitiesModel(new SceneActivityListModel(this))
    , m_historyModel(new HistoryListModel(this))
    , m_toastModel(new ToastListModel(this))
{
    m_sceneActivitiesModel->setClientsModel(m_clientsModel);
}

ApplicationController::~ApplicationController() = default;

QString ApplicationController::pageTitle() const
{
    switch (m_applicationPage) {
    case ApplicationPage::Clients: return QStringLiteral("Clients");
    case ApplicationPage::Canvas: return QStringLiteral("Canvas");
    case ApplicationPage::History: return QStringLiteral("Notification History");
    }
    return QStringLiteral("Mouffette");
}

QObject* ApplicationController::clientsModel() const { return m_clientsModel; }
QObject* ApplicationController::sceneActivitiesModel() const { return m_sceneActivitiesModel; }
QObject* ApplicationController::historyModel() const { return m_historyModel; }
QObject* ApplicationController::toastModel() const { return m_toastModel; }
QObject* ApplicationController::activeCanvasSession() const
{
    return m_activeCanvasSession.data();
}

bool ApplicationController::connectionEnabled() const
{
    return m_runtime && !m_runtime->isUserDisconnected();
}

QString ApplicationController::localStatusText() const
{
    return m_runtime ? m_runtime->localStatusText()
                               : QStringLiteral("DISCONNECTED");
}

ApplicationController::ConnectionState ApplicationController::localConnectionState() const
{
    return connectionStateFromStatus(localStatusText());
}

QString ApplicationController::remoteDisplayName() const
{
    return m_runtime ? m_runtime->remoteDisplayName() : QString();
}

QString ApplicationController::remoteStatusText() const
{
    return m_runtime ? m_runtime->remoteStatusText()
                               : QStringLiteral("DISCONNECTED");
}

ApplicationController::ConnectionState ApplicationController::remoteConnectionState() const
{
    return connectionStateFromStatus(remoteStatusText());
}

QString ApplicationController::remoteVolumeText() const
{
    if (!m_runtime || m_runtime->remoteVolumePercent() < 0) {
        return {};
    }
    return QStringLiteral("%1%").arg(m_runtime->remoteVolumePercent());
}

bool ApplicationController::remoteVolumeVisible() const
{
    return m_runtime && m_runtime->remoteVolumePercent() >= 0;
}

bool ApplicationController::remoteBusy() const
{
    return m_runtime && m_runtime->remoteBusy();
}

bool ApplicationController::canCloseSession() const
{
    return m_runtime && m_runtime->canCloseActiveSession();
}

bool ApplicationController::closingSession() const
{
    return m_runtime && m_runtime->closingActiveSession();
}

bool ApplicationController::canDeleteProject() const
{
    return m_runtime && m_runtime->canDeleteActiveProject();
}

QString ApplicationController::settingsServerUrl() const
{
    SettingsManager* settings = m_runtime
        ? m_runtime->getSettingsManager() : nullptr;
    return settings ? settings->getServerUrl() : AppConfig::instance().serverUrl();
}

bool ApplicationController::settingsAutoUpload() const
{
    SettingsManager* settings = m_runtime
        ? m_runtime->getSettingsManager() : nullptr;
    return settings ? settings->getAutoUploadImportedMedia()
                    : AppConfig::instance().autoUploadImportedMedia();
}

void ApplicationController::start()
{
    if (m_bootstrapStarted) return;
    m_bootstrapStarted = true;
    QTimer::singleShot(0, this, &ApplicationController::runBootstrap);
}

void ApplicationController::runBootstrap()
{
    m_bootstrapDecision = BootstrapDecision::None;
    m_bootstrapDecisionRequired = false;
    m_bootstrapTitle = QStringLiteral("Starting Mouffette");
    m_bootstrapDetail = QStringLiteral("Preparing local storage…");
    emit bootstrapChanged();

    m_bootstrapResult = m_storageBootstrap.run(
        [this](RuntimeStorageBootstrap::Stage stage) { setBootstrapStage(stage); });
    if (!m_bootstrapResult.succeeded()) {
        m_bootstrapDecision = BootstrapDecision::Retry;
        m_bootstrapDecisionRequired = true;
        m_bootstrapTitle = QStringLiteral("Mouffette could not start");
        m_bootstrapDetail = m_bootstrapResult.cause.isEmpty()
            ? m_bootstrapResult.code : m_bootstrapResult.cause;
        m_bootstrapPrimaryText = QStringLiteral("Retry");
        emit bootstrapChanged();
        return;
    }

    QString configError;
    if (!AppConfig::instance().initializeWithSettings(
            m_arguments, RuntimeProfile::readSettings(), &configError)) {
        m_bootstrapDecision = BootstrapDecision::Retry;
        m_bootstrapDecisionRequired = true;
        m_bootstrapTitle = QStringLiteral("Invalid runtime configuration");
        m_bootstrapDetail = configError;
        m_bootstrapPrimaryText = QStringLiteral("Retry");
        emit bootstrapChanged();
        return;
    }

    if (m_bootstrapResult.hadReset()) {
        m_bootstrapDecision = BootstrapDecision::AcknowledgeReset;
        m_bootstrapDecisionRequired = true;
        m_bootstrapTitle = QStringLiteral("Local data recovered");
        m_bootstrapDetail = m_bootstrapResult.cause.isEmpty()
            ? QStringLiteral("Mouffette repaired its local storage and can now continue.")
            : m_bootstrapResult.cause;
        m_bootstrapPrimaryText = QStringLiteral("Continue");
        emit bootstrapChanged();
        return;
    }
    finishBootstrap();
}

void ApplicationController::acceptBootstrapDecision()
{
    if (m_bootstrapDecision == BootstrapDecision::Retry) {
        QTimer::singleShot(0, this, &ApplicationController::runBootstrap);
        return;
    }
    if (m_bootstrapDecision == BootstrapDecision::AcknowledgeReset) {
        finishBootstrap();
    }
}

void ApplicationController::quitBootstrap()
{
    QCoreApplication::quit();
}

void ApplicationController::finishBootstrap()
{
    m_bootstrapDecision = BootstrapDecision::None;
    m_bootstrapDecisionRequired = false;
    initializeBackend();
}

void ApplicationController::initializeBackend()
{
    if (m_runtime) return;
    m_runtime = std::make_unique<ApplicationRuntime>(m_runtimeProfile);

    connect(m_runtime.get(), &ApplicationRuntime::displayClientsChanged,
            this, [this](const QList<ClientInfo>& clients) {
        m_clientsModel->setClients(clients);
    });
    connect(m_runtime.get(), &ApplicationRuntime::presentationStateChanged,
            this, &ApplicationController::refreshPresentation);
    connect(m_runtime.get(), &ApplicationRuntime::applicationPageChanged,
            this, [this](int page) {
        setApplicationPage(static_cast<ApplicationPage>(page));
        refreshActiveCanvasSession();
    });
    connect(m_runtime.get(), &ApplicationRuntime::qmlRaiseRequested,
            this, &ApplicationController::raiseRequested);
    connect(m_runtime.get(), &ApplicationRuntime::qmlHideRequested,
            this, &ApplicationController::hideRequested);
    connect(m_runtime.get(), &ApplicationRuntime::activeSessionChanged,
            this, [this]() { refreshActiveCanvasSession(); });

    m_clientsModel->setClients(m_runtime->displayClients());
    m_sceneActivitiesModel->setSource(m_runtime->getSceneActivityModel());
    m_historyModel->setSource(m_runtime->getNotificationCenter());
    m_toastModel->setSource(m_runtime->getNotificationCenter());

    m_ready = true;
    emit readyChanged();
    emit bootstrapChanged();
    refreshPresentation();
}

void ApplicationController::setBootstrapStage(RuntimeStorageBootstrap::Stage stage)
{
    m_bootstrapDetail = RuntimeStorageBootstrap::stageLabel(stage);
    emit bootstrapChanged();
}

void ApplicationController::openClient(const QString& endpointId)
{
    if (!m_runtime) return;
    m_runtime->activateClient(endpointId);
    setApplicationPage(ApplicationPage::Canvas);
    refreshActiveCanvasSession();
}

void ApplicationController::openOngoingScene(const QString& sceneRunId)
{
    if (!m_runtime || !m_runtime->getSceneActivityModel()) return;
    const SceneActivityModel::Activity activity =
        m_runtime->getSceneActivityModel()->activity(sceneRunId);
    if (activity.sceneRunId.isEmpty()) return;
    if (activity.direction == SceneActivityModel::Direction::Outgoing) {
        m_runtime->activateOngoingScene(sceneRunId);
        setApplicationPage(ApplicationPage::Canvas);
        refreshActiveCanvasSession();
        return;
    }

    const ClientInfo peer = m_clientsModel->client(activity.peerEndpointId);
    const QString peerName = peer.getMachineName().trimmed().isEmpty()
        ? QStringLiteral("Device %1").arg(activity.peerEndpointId.left(8))
        : peer.getMachineName().trimmed();
    const qint64 elapsedSeconds = qMax<qint64>(0,
        QDateTime::currentMSecsSinceEpoch() - activity.startedAtEpochMs) / 1000;
    const QString duration = elapsedSeconds >= 3600
        ? QStringLiteral("%1:%2:%3").arg(elapsedSeconds / 3600)
              .arg((elapsedSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
              .arg(elapsedSeconds % 60, 2, 10, QLatin1Char('0'))
        : QStringLiteral("%1:%2").arg(elapsedSeconds / 60)
              .arg(elapsedSeconds % 60, 2, 10, QLatin1Char('0'));
    showDialog(DialogKind::IncomingSceneInfo, QStringLiteral("Ongoing Scene"),
               QStringLiteral("Received from %1\n\nStarted: %2\nDuration: %3\nNetwork: %4\n\nThis incoming scene is read-only.")
                   .arg(peerName,
                        QDateTime::fromMSecsSinceEpoch(activity.startedAtEpochMs)
                            .toString(QStringLiteral("HH:mm:ss")),
                        duration,
                        activity.degraded ? QStringLiteral("Degraded")
                                          : QStringLiteral("Healthy")),
               QStringLiteral("OK"), QString(), false, false);
}

void ApplicationController::goBack()
{
    if (!m_runtime) return;
    m_runtime->navigateToClients();
    setApplicationPage(ApplicationPage::Clients);
    refreshActiveCanvasSession();
}

void ApplicationController::showHistory()
{
    if (!m_runtime) return;
    m_runtime->navigateToHistory();
    setApplicationPage(ApplicationPage::History);
    refreshActiveCanvasSession();
}

void ApplicationController::toggleConnection()
{
    if (m_runtime) m_runtime->toggleConnectionEnabled();
}

void ApplicationController::closeSession()
{
    if (m_runtime) m_runtime->closeActiveSession();
}

void ApplicationController::requestDeleteProject()
{
    showDialog(DialogKind::DeleteProject, QStringLiteral("Delete project?"),
               QStringLiteral("Delete this local project and its canvas?\n\nSource files will never be deleted."),
               QStringLiteral("Delete"), QStringLiteral("Cancel"), true, true);
}

void ApplicationController::requestClearHistory()
{
    showDialog(DialogKind::ClearHistory, QStringLiteral("Clear history?"),
               QStringLiteral("All notification history will be permanently removed."),
               QStringLiteral("Clear"), QStringLiteral("Cancel"), true, true);
}

void ApplicationController::acceptDialog()
{
    const DialogKind accepted = m_dialogKind;
    clearDialog();
    if (!m_runtime) return;
    if (accepted == DialogKind::DeleteProject) {
        m_runtime->deleteActiveProjectConfirmed();
        setApplicationPage(ApplicationPage::Clients);
        refreshActiveCanvasSession();
    } else if (accepted == DialogKind::ClearHistory) {
        if (NotificationCenter* center = m_runtime->getNotificationCenter()) {
            center->clearHistory();
        }
    }
}

void ApplicationController::rejectDialog()
{
    clearDialog();
}

QString ApplicationController::saveSettings(const QString& serverUrl,
                                            bool autoUpload)
{
    if (!m_runtime || !m_runtime->getSettingsManager()) {
        return QStringLiteral("Settings are not ready yet.");
    }
    QUrl normalized;
    QString error;
    if (!AppConfig::validateServerUrl(serverUrl, &normalized, &error)) {
        return error;
    }
    SettingsManager* settings = m_runtime->getSettingsManager();
    const QString canonical = normalized.toString(QUrl::FullyEncoded);
    const bool reconnect = canonical != settings->getServerUrl();
    settings->setServerUrl(canonical);
    settings->setAutoUploadImportedMedia(autoUpload);
    settings->saveSettings();
    if (reconnect) {
        m_runtime->setUserDisconnected(false);
        m_runtime->connectToServer();
    }
    emit settingsChanged();
    return {};
}

void ApplicationController::hideWindow()
{
    setWindowVisible(false);
    emit hideRequested();
}

void ApplicationController::setWindowVisible(bool visible)
{
    if (m_runtime) m_runtime->setQmlWindowVisible(visible);
}

void ApplicationController::handleApplicationStateChanged(Qt::ApplicationState state)
{
    if (m_runtime) m_runtime->handleApplicationStateChanged(state);
}

void ApplicationController::handleNativeSystemSuspendedChanged(bool suspended)
{
    if (m_runtime) {
        m_runtime->handleNativeSystemSuspendedChanged(suspended);
    }
}

void ApplicationController::handleApplicationAboutToQuit()
{
    if (m_runtime) m_runtime->handleApplicationAboutToQuit();
}

void ApplicationController::setApplicationPage(ApplicationPage page)
{
    if (m_applicationPage == page) return;
    m_applicationPage = page;
    emit applicationPageChanged();
}

void ApplicationController::refreshPresentation()
{
    emit presentationChanged();
}

void ApplicationController::refreshActiveCanvasSession()
{
    if (!m_runtime || m_applicationPage != ApplicationPage::Canvas) {
        if (m_activeCanvasSession) {
            m_activeCanvasSession = nullptr;
            emit activeCanvasSessionChanged();
        }
        return;
    }
    const QString id = m_runtime->getActiveSessionIdentity();
    ApplicationRuntime::CanvasSession* session = m_runtime->findCanvasSession(id);
    if (!session) return;

    CanvasSessionViewModel* viewModel = m_canvasSessions.value(id);
    if (!viewModel) {
        viewModel = new CanvasSessionViewModel(
            id, session->canvas,
            [backend = m_runtime.get()]() {
                if (backend) backend->onUploadButtonClicked();
            }, m_runtime->getUploadManager(),
            [backend = m_runtime.get(), id]() {
                const ApplicationRuntime::CanvasSession* current = backend
                    ? backend->findCanvasSession(id) : nullptr;
                return current && current->upload.remoteFilesPresent;
            },
            [backend = m_runtime.get(), id]() {
                return backend && backend->hasUnuploadedFilesForTarget(id);
            }, this);
        m_canvasSessions.insert(id, viewModel);
    } else {
        viewModel->setCanvas(session->canvas);
    }
    if (m_activeCanvasSession != viewModel) {
        m_activeCanvasSession = viewModel;
        emit activeCanvasSessionChanged();
    }
}

void ApplicationController::clearDialog()
{
    m_dialogKind = DialogKind::None;
    m_dialogTitle.clear();
    m_dialogMessage.clear();
    emit dialogChanged();
}

void ApplicationController::showDialog(DialogKind kind, const QString& title,
                                       const QString& message,
                                       const QString& acceptText,
                                       const QString& rejectText,
                                       bool destructive, bool showReject)
{
    m_dialogKind = kind;
    m_dialogTitle = title;
    m_dialogMessage = message;
    m_dialogAcceptText = acceptText;
    m_dialogRejectText = rejectText;
    m_dialogDestructive = destructive;
    m_dialogShowReject = showReject;
    emit dialogChanged();
    emit dialogRequested();
}

ApplicationController::ConnectionState
ApplicationController::connectionStateFromStatus(const QString& status)
{
    const QString normalized = status.trimmed().toUpper();
    if (normalized == QLatin1String("CONNECTED")
        || normalized == QLatin1String("AVAILABLE")) {
        return ConnectionState::Connected;
    }
    if (normalized.contains(QStringLiteral("CONNECTING"))
        || normalized == QLatin1String("ERROR")) {
        return ConnectionState::Transitional;
    }
    return ConnectionState::Disconnected;
}

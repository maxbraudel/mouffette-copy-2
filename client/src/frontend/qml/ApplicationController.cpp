#include "frontend/qml/ApplicationController.h"

#include "backend/runtime/ApplicationRuntime.h"
#include "backend/config/AppConfig.h"
#include "backend/media/MediaBackendBootstrap.h"
#include "backend/domain/scene/SceneActivityModel.h"
#include "backend/domain/project/ProjectManager.h"
#include "backend/managers/app/SettingsManager.h"
#include "backend/domain/profile/ClientProfileCache.h"
#include "backend/domain/profile/ProfileImage.h"
#include "frontend/qml/ProfilePictureProvider.h"
#include "frontend/qml/QmlRuntime.h"
#include <QQmlEngine>
#include "backend/notifications/NotificationCenter.h"
#include "backend/runtime/RuntimeProfile.h"
#include "frontend/qml/ClientWorkspaceViewModel.h"
#include "frontend/qml/ClientListModel.h"
#include "frontend/qml/NotificationListModels.h"
#include "frontend/qml/SceneActivityListModel.h"
#include "shared/rendering/ICanvasHost.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QFutureWatcher>
#include <QTimer>
#include <QUrl>
#include <utility>

ApplicationController::ApplicationController(RuntimeProfileContext runtimeProfile,
                                             QStringList arguments,
                                             QObject* parent,
                                             MediaBootstrapFunction mediaBootstrap)
    : QObject(parent)
    , m_runtimeProfile(std::move(runtimeProfile))
    , m_arguments(std::move(arguments))
    , m_storageBootstrap(m_runtimeProfile)
    , m_mediaBootstrap(mediaBootstrap ? std::move(mediaBootstrap)
                                      : MediaBackendBootstrap::initialize)
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
QObject* ApplicationController::activeWorkspace() const
{
    return m_activeWorkspace.data();
}

bool ApplicationController::connectionEnabled() const
{
    return m_runtime && !m_runtime->isUserDisconnected();
}

bool ApplicationController::screenContentVisible() const
{
    const auto* settings = m_runtime ? m_runtime->getSettingsManager() : nullptr;
    return !settings || settings->getScreenContentVisible();
}

QString ApplicationController::localConnectionDetail() const { return m_runtime ? m_runtime->localConnectionDetail() : QString(); }
QString ApplicationController::remoteConnectionDetail() const { return m_runtime ? m_runtime->remoteConnectionDetail() : QString(); }
QString ApplicationController::clientConnectionDetail(const QString& endpoint) const { return m_runtime ? m_runtime->clientConnectionDetail(endpoint) : QString(); }

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

QString ApplicationController::remoteEndpointId() const
{
    return m_runtime ? m_runtime->activeWorkspaceEndpointId() : QString();
}

QString ApplicationController::remoteProfilePictureSource() const
{
    return profilePictureSource(remoteEndpointId());
}

QString ApplicationController::profilePictureSource(const QString& endpointId) const
{
    return m_runtime ? m_runtime->profileCache()->pictureSource(endpointId)
                     : ClientProfileCache::defaultSource();
}

void ApplicationController::requestProfilePicture(const QString& endpointId)
{
    if (m_runtime) m_runtime->profileCache()->requestPicture(endpointId);
}

QString ApplicationController::clientDisplayName(const QString& endpointId, const QString& hostname, int ordinal) const
{
    const ClientInfo known = m_clientsModel->client(endpointId);
    const QString machineName = known.getMachineName().isEmpty() ? hostname : known.getMachineName();
    const int instance = known.endpointId().isEmpty() ? ordinal : known.instanceOrdinal();
    const QString fallback = machineName.isEmpty()
        ? QStringLiteral("Device %1").arg(endpointId.left(8)) : machineName;
    if (m_runtime) return m_runtime->profileCache()->displayName(endpointId, fallback, instance);
    ClientInfo client(endpointId, fallback, {});
    client.setInstanceOrdinal(instance);
    return client.getInstanceDisplayName();
}

QString ApplicationController::settingsUsername() const
{
    return m_runtime ? m_runtime->getSettingsManager()->username() : QString();
}

QString ApplicationController::settingsHostname() const
{
    return m_runtime ? m_runtime->getMachineName() : QString();
}

QString ApplicationController::settingsProfilePictureSource() const
{
    return m_runtime ? m_runtime->profileCache()->localPictureSource(QStringLiteral("saved"))
                     : ClientProfileCache::defaultSource();
}

QString ApplicationController::settingsProfilePictureDraftSource() const
{
    return m_runtime && m_profileEditing
        ? m_runtime->profileCache()->localPictureSource(QStringLiteral("draft"))
        : settingsProfilePictureSource();
}

void ApplicationController::beginProfileEdit()
{
    if (!m_runtime) return;
    m_profileEditing = true;
    m_draftProfilePicture = m_runtime->getSettingsManager()->profilePictureJpeg();
    m_runtime->profileCache()->setLocalPicture(QStringLiteral("draft"), m_draftProfilePicture);
    emit profileDraftChanged();
}

QString ApplicationController::importProfilePicture(const QUrl& file)
{
    if (!m_runtime || m_clearingStorage) return QStringLiteral("Settings are not ready yet.");
    QByteArray jpeg;
    QString error;
    if (!ProfileImage::importFile(file, &jpeg, &error)) return error;
    if (!m_profileEditing) beginProfileEdit();
    m_draftProfilePicture = jpeg;
    m_runtime->profileCache()->setLocalPicture(QStringLiteral("draft"), jpeg);
    emit profileDraftChanged();
    return {};
}

void ApplicationController::removeProfilePicture()
{
    if (!m_runtime) return;
    if (!m_profileEditing) beginProfileEdit();
    m_draftProfilePicture.clear();
    m_runtime->profileCache()->setLocalPicture(QStringLiteral("draft"), {});
    emit profileDraftChanged();
}

void ApplicationController::cancelProfileEdit()
{
    m_profileEditing = false;
    m_draftProfilePicture.clear();
    if (m_runtime) m_runtime->profileCache()->setLocalPicture(QStringLiteral("draft"), {});
    emit profileDraftChanged();
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

bool ApplicationController::canDeleteProject() const
{
    return m_runtime && m_runtime->canDeleteActiveProject();
}

bool ApplicationController::hasProject() const
{
    return m_runtime && m_runtime->activeProjectExists();
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

bool ApplicationController::settingsAppAlwaysOnTop() const
{
    auto* settings = m_runtime ? m_runtime->getSettingsManager() : nullptr;
    return settings ? settings->getAppAlwaysOnTop() : AppConfig::instance().appAlwaysOnTop();
}

bool ApplicationController::settingsScreenSharingEnabled() const
{
    const auto* settings = m_runtime ? m_runtime->getSettingsManager() : nullptr;
    return settings && settings->getScreenSharingEnabled();
}

QString ApplicationController::settingsScreenSharingStatus() const
{
    return m_runtime ? m_runtime->screenSharingStatus() : QString();
}

void ApplicationController::start()
{
    if (m_bootstrapStarted) return;
    m_bootstrapStarted = true;
    QTimer::singleShot(0, this, &ApplicationController::runBootstrap);
}

void ApplicationController::runBootstrap()
{
    if (m_clearingStorage) return;
    m_bootstrapDecision = BootstrapDecision::None;
    m_bootstrapDecisionRequired = false;
    m_bootstrapCanClearStorage = false;
    m_bootstrapTitle = QStringLiteral("Starting Mouffette");
    m_bootstrapDetail = QStringLiteral("Preparing local storage…");
    emit bootstrapChanged();

    m_bootstrapResult = m_storageBootstrap.run(
        [this](RuntimeStorageBootstrap::Stage stage) { setBootstrapStage(stage); });
    if (!m_bootstrapResult.succeeded()) {
        m_bootstrapDecision = BootstrapDecision::Retry;
        m_bootstrapDecisionRequired = true;
        m_bootstrapCanClearStorage = m_bootstrapResult.code.endsWith(
            QStringLiteral("_storage_failed"));
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
        m_bootstrapCanClearStorage = true;
        m_bootstrapTitle = QStringLiteral("Invalid runtime configuration");
        m_bootstrapDetail = configError;
        m_bootstrapPrimaryText = QStringLiteral("Retry");
        emit bootstrapChanged();
        return;
    }

    finishBootstrap();
}

void ApplicationController::acceptBootstrapDecision()
{
    if (!m_clearingStorage && m_bootstrapDecision == BootstrapDecision::Retry) {
        m_bootstrapDecision = BootstrapDecision::None;
        m_bootstrapDecisionRequired = false;
        m_bootstrapCanClearStorage = false;
        emit bootstrapChanged();
        QTimer::singleShot(0, this, &ApplicationController::runBootstrap);
        return;
    }
}

void ApplicationController::quitBootstrap()
{
    QCoreApplication::quit();
}

void ApplicationController::finishBootstrap()
{
    if (m_multimediaBootstrapPending || m_ready) return;
    m_bootstrapDecision = BootstrapDecision::None;
    m_bootstrapDecisionRequired = false;
    m_bootstrapCanClearStorage = false;
    m_bootstrapDetail = QStringLiteral("Preparing audio and video…");
    m_multimediaBootstrapPending = true;
    emit bootstrapChanged();
    auto* watcher = new QFutureWatcher<MediaBackendBootstrap::Result>(this);
    connect(watcher, &QFutureWatcher<MediaBackendBootstrap::Result>::finished,
            this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_multimediaBootstrapPending = false;
        if (!result.ready) {
            m_bootstrapDecision = BootstrapDecision::Retry;
            m_bootstrapDecisionRequired = true;
            m_bootstrapCanClearStorage = false;
            m_bootstrapTitle = QStringLiteral("Mouffette could not start");
            m_bootstrapDetail = result.error;
            m_bootstrapPrimaryText = QStringLiteral("Retry");
            emit bootstrapChanged();
            return;
        }
        initializeBackend();
    });
    watcher->setFuture(m_mediaBootstrap());
}

void ApplicationController::initializeBackend()
{
    if (m_runtime) return;
    m_runtime = std::make_unique<ApplicationRuntime>(m_runtimeProfile);
    connect(m_runtime.get(), &ApplicationRuntime::screenSharingStatusChanged,
            this, &ApplicationController::screenSharingStatusChanged);
    connect(m_runtime->getSettingsManager(), &SettingsManager::screenContentVisibleChanged,
            this, &ApplicationController::screenContentVisibleChanged);
    auto* profiles = m_runtime->profileCache();
    profiles->setLocalPicture(QStringLiteral("saved"), m_runtime->getSettingsManager()->profilePictureJpeg());
    QmlRuntime::engine()->addImageProvider(QStringLiteral("profiles"), new ProfilePictureProvider(profiles));
    connect(profiles, &ClientProfileCache::profilesChanged, this, [this] {
        ++m_profileRevision;
        emit profilesChanged();
    });

    connect(m_runtime.get(), &ApplicationRuntime::displayClientsChanged,
            this, [this](const QList<ClientInfo>& clients) {
        m_clientsModel->setClients(clients);
        ++m_profileRevision;
        emit profilesChanged();
    });
    connect(m_runtime.get(), &ApplicationRuntime::presentationStateChanged,
            this, &ApplicationController::refreshPresentation);
    connect(m_runtime.get(), &ApplicationRuntime::activeWorkspaceChanged, this, [this] {
        ++m_profileRevision;
        emit profilesChanged();
    });
    connect(m_runtime.get(), &ApplicationRuntime::applicationPageChanged,
            this, [this](int page) {
        setApplicationPage(static_cast<ApplicationPage>(page));
        refreshActiveWorkspace();
    });
    connect(m_runtime.get(), &ApplicationRuntime::qmlRaiseRequested,
            this, &ApplicationController::raiseRequested);
    connect(m_runtime.get(), &ApplicationRuntime::qmlHideRequested,
            this, &ApplicationController::hideRequested);
    connect(m_runtime.get(), &ApplicationRuntime::activeWorkspaceChanged,
            this, [this]() { refreshActiveWorkspace(); });
    if (WorkspaceManager* workspaces = m_runtime->getWorkspaceManager()) {
        connect(workspaces, &WorkspaceManager::workspaceDeleted,
                this, [this](const QString& endpointId) {
            ClientWorkspaceViewModel* removed = m_workspaces.take(endpointId);
            if (!removed) return;
            removed->setCanvas(nullptr);
            if (m_activeWorkspace == removed) {
                m_activeWorkspace = nullptr;
                emit activeWorkspaceChanged();
            }
            removed->deleteLater();
        });
    }

    m_clientsModel->setClients(m_runtime->displayClients());
    m_sceneActivitiesModel->setSource(m_runtime->getSceneActivityModel());
    m_historyModel->setSource(m_runtime->getNotificationCenter());
    m_toastModel->setSource(m_runtime->getNotificationCenter());

    m_ready = true;
    emit settingsChanged();
    emit screenContentVisibleChanged();
    emit screenSharingStatusChanged();
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
    refreshActiveWorkspace();
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
        refreshActiveWorkspace();
        return;
    }

    const ClientInfo peer = m_clientsModel->client(activity.peerEndpointId);
    const qint64 elapsedSeconds = qMax<qint64>(0,
        QDateTime::currentMSecsSinceEpoch() - activity.startedAtEpochMs) / 1000;
    const QString duration = elapsedSeconds >= 3600
        ? QStringLiteral("%1:%2:%3").arg(elapsedSeconds / 3600)
              .arg((elapsedSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
              .arg(elapsedSeconds % 60, 2, 10, QLatin1Char('0'))
        : QStringLiteral("%1:%2").arg(elapsedSeconds / 60)
              .arg(elapsedSeconds % 60, 2, 10, QLatin1Char('0'));
    showDialog(DialogKind::IncomingSceneInfo, QStringLiteral("Ongoing Scene"),
               QStringLiteral("Started: %1\nDuration: %2\nNetwork: %3\n\nThis incoming scene is read-only.")
                   .arg(QDateTime::fromMSecsSinceEpoch(activity.startedAtEpochMs)
                            .toString(QStringLiteral("HH:mm:ss")),
                        duration,
                        activity.degraded ? QStringLiteral("Degraded")
                                          : QStringLiteral("Healthy")),
               QStringLiteral("OK"), QString(), false, false,
               {QVariantMap{{QStringLiteral("endpointId"), activity.peerEndpointId},
                            {QStringLiteral("machineName"), peer.getMachineName()},
                            {QStringLiteral("instanceOrdinal"), peer.instanceOrdinal()},
                            {QStringLiteral("role"), QStringLiteral("From")}}});
}

void ApplicationController::goBack()
{
    if (!m_runtime) return;
    m_runtime->navigateToClients();
    setApplicationPage(ApplicationPage::Clients);
    refreshActiveWorkspace();
}

void ApplicationController::showHistory()
{
    if (!m_runtime) return;
    m_runtime->navigateToHistory();
    setApplicationPage(ApplicationPage::History);
    refreshActiveWorkspace();
}

void ApplicationController::toggleConnection()
{
    setConnectionEnabled(!connectionEnabled());
}

void ApplicationController::setConnectionEnabled(bool enabled)
{
    if (m_runtime) m_runtime->setConnectionEnabled(enabled);
}

void ApplicationController::setScreenContentVisible(bool visible)
{
    if (!m_runtime || m_clearingStorage) return;
    QString error;
    if (!m_runtime->getSettingsManager()->setScreenContentVisible(visible, &error)) {
        NotificationRequest request;
        request.severity = NotificationSeverity::Error;
        request.category = QStringLiteral("Settings");
        request.message = tr("Could not save the screen content preference: %1").arg(error);
        m_runtime->getNotificationCenter()->publish(request);
    }
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
        refreshActiveWorkspace();
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
                                            bool autoUpload, bool appAlwaysOnTop,
                                            const QString& username)
{
    return saveSettings(serverUrl, autoUpload, appAlwaysOnTop, username,
                        settingsScreenSharingEnabled());
}

QString ApplicationController::saveSettings(const QString& serverUrl,
                                            bool autoUpload, bool appAlwaysOnTop,
                                            const QString& username, bool screenSharingEnabled)
{
    if (m_clearingStorage) return QStringLiteral("The application is closing.");
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
    const QByteArray picture = m_profileEditing ? m_draftProfilePicture : settings->profilePictureJpeg();
    if (!settings->commitSettings(canonical, autoUpload, appAlwaysOnTop, username, picture,
                                  screenSharingEnabled, &error)) return error;
    m_runtime->profileCache()->setLocalPicture(QStringLiteral("saved"), picture);
    cancelProfileEdit();
    if (reconnect) {
        m_runtime->connectToServer();
    } else m_runtime->syncRegistration();
    emit settingsChanged();
    return {};
}

void ApplicationController::clearStorageAndClose()
{
    if ((!m_ready && !(m_bootstrapDecisionRequired && m_bootstrapCanClearStorage))
        || m_clearingStorage) return;
    m_clearingStorage = true;
    emit clearingStorageChanged();
    // main owns the removal: it must run after runtime/QML destruction, while
    // the instance slot still prevents another process opening this profile.
    emit clearStorageOnExitRequested();
    QCoreApplication::quit();
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

void ApplicationController::setPointerInside(bool inside)
{
    if (m_runtime) m_runtime->setPointerInsideControlWindow(inside);
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

void ApplicationController::refreshActiveWorkspace()
{
    if (!m_runtime || m_applicationPage != ApplicationPage::Canvas) {
        if (m_activeWorkspace) {
            m_activeWorkspace = nullptr;
            emit activeWorkspaceChanged();
        }
        return;
    }
    const QString id = m_runtime->activeWorkspaceEndpointId();
    ApplicationRuntime::ClientWorkspace* workspace = m_runtime->findWorkspace(id);
    if (!workspace || !workspace->canvas) {
        if (ClientWorkspaceViewModel* cached = m_workspaces.value(id)) {
            cached->setCanvas(nullptr);
            cached->refreshCapabilities();
        }
        if (m_activeWorkspace) {
            m_activeWorkspace = nullptr;
            emit activeWorkspaceChanged();
        }
        return;
    }

    ClientWorkspaceViewModel* viewModel = m_workspaces.value(id);
    if (!viewModel) {
        viewModel = new ClientWorkspaceViewModel(
            id, workspace->canvas,
            [backend = m_runtime.get()]() {
                if (backend) backend->onUploadButtonClicked();
            }, m_runtime->getUploadManager(),
            [backend = m_runtime.get(), id]() {
                const ApplicationRuntime::ClientWorkspace* current = backend
                    ? backend->findWorkspace(id) : nullptr;
                return current && current->upload.remoteFilesPresent;
            },
            [backend = m_runtime.get(), id]() {
                return backend && backend->hasUnuploadedFilesForTarget(id);
            },
            [backend = m_runtime.get(), id]() {
                return backend && backend->getProjectManager()
                    && backend->getProjectManager()->hasProjectForTarget(id);
            }, this);
        m_workspaces.insert(id, viewModel);
    } else {
        viewModel->setCanvas(workspace->canvas);
        viewModel->refreshCapabilities();
    }
    if (m_activeWorkspace != viewModel) {
        m_activeWorkspace = viewModel;
        emit activeWorkspaceChanged();
    }
}

void ApplicationController::clearDialog()
{
    m_dialogKind = DialogKind::None;
    m_dialogTitle.clear();
    m_dialogMessage.clear();
    m_dialogPeers.clear();
    emit dialogChanged();
}

void ApplicationController::showDialog(DialogKind kind, const QString& title,
                                       const QString& message,
                                       const QString& acceptText,
                                       const QString& rejectText,
                                       bool destructive, bool showReject, const QVariantList& peers)
{
    m_dialogKind = kind;
    m_dialogTitle = title;
    m_dialogMessage = message;
    m_dialogPeers = peers;
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
        || normalized == QLatin1String("SYNCHRONIZING")
        || normalized == QLatin1String("AUTHENTICATING")
        || normalized == QLatin1String("DEGRADED")
        || normalized == QLatin1String("CLEANUP PENDING")
        || normalized == QLatin1String("ERROR")) {
        return ConnectionState::Transitional;
    }
    return ConnectionState::Disconnected;
}

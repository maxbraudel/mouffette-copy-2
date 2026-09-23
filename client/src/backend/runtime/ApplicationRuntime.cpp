#include "backend/runtime/ApplicationRuntime.h"
#include "backend/runtime/SuspendInclusiveClock.h"
#include "backend/media/MediaResidencyManager.h"
#include "backend/runtime/ApplicationActivityMonitor.h"
#include "backend/network/WebSocketClient.h"
#include "backend/domain/models/ClientInfo.h"
#include "backend/domain/profile/ClientProfileCache.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "backend/network/UploadManager.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/RemoteCacheStore.h"
#include "backend/files/FileWatcher.h"
#include "shared/rendering/ICanvasHost.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/files/FileManager.h"
#include "frontend/rendering/remote/RemoteSceneController.h"
#include "backend/domain/workspace/WorkspaceManager.h"
#include "backend/domain/session/IncomingSessionOrphanWatchdog.h"
#include "backend/domain/project/ProjectManager.h"
#include "backend/domain/project/ProjectScreenPreviewStore.h"
#include "backend/domain/project/ProjectModel.h"
#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/scene/SceneActivityModel.h"
#include "backend/network/SceneRunCoordinator.h"
#include "backend/managers/system/SystemMonitor.h"
#include "backend/managers/app/SystemTrayManager.h"
#include "backend/handlers/WebSocketMessageHandler.h"
#include "backend/handlers/ScreenEventHandler.h"
#include "backend/handlers/ClientListEventHandler.h"
#include "backend/handlers/UploadEventHandler.h"
#include "backend/controllers/ClientWorkspaceController.h"
#include "backend/config/AppConfig.h"
#include "backend/managers/app/SettingsManager.h"
#include "backend/screensharing/ScreenSharingService.h"
#include "backend/audiosharing/AudioSharingService.h"
#include "backend/managers/network/ClientListBuilder.h"
#include "backend/managers/network/ConnectionManager.h"
#include "frontend/handlers/UploadSignalConnector.h"
#include <QHostInfo>
#include <QGuiApplication>
#include <QEvent>

// Forward declaration for system UI extraction
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <utility>
#include <QSettings>
#include <QRandomGenerator>
#include <QTimer>
#include <QPen>
#include <QBrush>
#include <QUrl>
#include <QImage>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSysInfo>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <vector>
#  include <string>
// WinAPI monitor enumeration helper
struct WinMonRect { std::wstring name; RECT rc; RECT rcWork; bool primary; };
static BOOL CALLBACK MouffetteEnumMonProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* out = reinterpret_cast<std::vector<WinMonRect>*>(lParam);
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        WinMonRect r; r.name = mi.szDevice; r.rc = mi.rcMonitor; r.rcWork = mi.rcWork; r.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0; out->push_back(r);
    }
    return TRUE;
}
#endif
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QUuid>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <climits>
#include <memory>
#include <limits>

namespace {
constexpr double kMaxSafeJsonInteger = 9007199254740991.0;

bool readSafePositiveJsonInteger(const QJsonValue& value, quint64* output)
{
    if (!output || !value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 1.0
        || number > kMaxSafeJsonInteger || std::floor(number) != number) {
        return false;
    }
    *output = static_cast<quint64>(number);
    return true;
}

bool isCommandReadyBinding(
    const WebSocketClient* client,
    const RemoteSessionCoordinator::Binding& binding)
{
    if (!client || !client->canIssueSessionCommands(binding.remoteSessionId)
        || binding.phase != QLatin1String("Active")) {
        return false;
    }
    const QString localEndpointId = client->endpointId();
    quint64 bindingConnectionGeneration = 0;
    if (binding.ownerEndpointId == localEndpointId) {
        bindingConnectionGeneration = binding.ownerConnectionGeneration;
    } else if (binding.targetEndpointId == localEndpointId) {
        bindingConnectionGeneration = binding.targetConnectionGeneration;
    } else {
        return false;
    }
    // An Active lease belongs to the authenticated transport tuple on which
    // it was granted. Retaining it during reconnect is useful for RESUME, but
    // it cannot authorize UI commands on an older or unauthenticated socket.
    return bindingConnectionGeneration > 0
        && bindingConnectionGeneration == client->connectionGeneration();
}

bool hasRetainedSession(const WebSocketClient* client, const RemoteSessionCoordinator::Binding& binding)
{
    return client && !binding.remoteSessionId.isEmpty()
        && (binding.phase == QLatin1String("Active") || binding.phase == QLatin1String("Grace"))
        && client->sessionRecoveryRemainingMs(binding.remoteSessionId) > 0;
}


ProjectManager::TimingPolicy projectTimingPolicyFromConfig() {
    ProjectManager::TimingPolicy timing;
    timing.projectMediaHiddenTimeoutMs =
        AppConfig::instance().projectMediaHiddenTimeoutMs();
    timing.projectHiddenRetentionMs =
        AppConfig::instance().projectHiddenRetentionMs();
    timing.autosaveDelayMs = AppConfig::instance().projectAutosaveDelayMs();
    timing.checkpointIntervalMs = AppConfig::instance().projectCheckpointIntervalMs();
    timing.deadlinePollIntervalMs = AppConfig::instance().projectDeadlinePollIntervalMs();
    return timing;
}

QString canonicalExistingPath(const QString& sourcePath) {
    const QFileInfo info(sourcePath);
    if (!info.exists() || !info.isFile() || !info.isReadable()) {
        return {};
    }
    return info.canonicalFilePath();
}

QString sourceIdentityForPath(const QString& canonicalPath) {
    const QFileInfo info(canonicalPath);
    if (!info.exists() || !info.isFile()) {
        return {};
    }
    return QStringLiteral("%1:%2")
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch());
}


}

#ifdef Q_OS_MACOS
#include "backend/platform/macos/MacWindowManager.h"
#endif
#include <QSet>
#include <QElapsedTimer>
#include <QDateTime>
#include <QRunnable>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWindow>
#include <QObject>
#include <atomic>
#include <thread>
#include <optional>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <combaseapi.h>
#endif
#ifdef Q_OS_MACOS
#include <QProcess>
#endif

// Z-ordering constants used throughout the scene
namespace {
constexpr qreal Z_SCREENS = -1000.0;
constexpr qreal Z_MEDIA_BASE = 1.0;
constexpr qreal Z_REMOTE_CURSOR = 10000.0;
constexpr qreal Z_SCENE_OVERLAY = 12000.0; // above all scene content
}


QString ApplicationRuntime::localConnectionDetail() const
{
    return m_connectionManager ? m_connectionManager->connectionDetail() : QString();
}

QString ApplicationRuntime::clientConnectionDetail(const QString& endpoint) const
{
    if (!m_connectionManager || !m_connectionManager->isReady())
        return QStringLiteral("Remote availability cannot be confirmed until the server connection is ready.\n") + localConnectionDetail();
    QStringList details;
    if (m_remoteSessionAutoOpenBlockedTargets.contains(endpoint)) details << QStringLiteral("Session opening blocked; corrective action required");
    const qint64 next = m_sessionRecovery.nextAttemptAtMs(endpoint);
    if (next >= 0) details << QStringLiteral("Next session attempt in %1 s").arg(qMax<qint64>(0, next - MouffetteClock::nowMs()) / 1000.0, 0, 'f', 1);
    if (!m_sessionRecovery.reason(endpoint).isEmpty()) details << m_sessionRecovery.reason(endpoint);
    for (const auto& client : m_discoveredClients) {
        if (client.endpointId() != endpoint) continue;
        if (!client.canAcceptSession()) details << QStringLiteral("Waiting for remote availability");
        if (!client.canAcceptSession() && !client.presenceReason().isEmpty()) details << client.presenceReason();
        break;
    }
    if (m_remoteSessionOpenPendingTargets.contains(endpoint)) details << QStringLiteral("Waiting for the authenticated session snapshot");
    return details.join(QStringLiteral("\n"));
}

QString ApplicationRuntime::remoteConnectionStatus(
    const QString& targetEndpointId, const ClientInfo* presence,
    bool localDiscoveryUsable) const
{
    const auto binding = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator()->outgoingForPeer(targetEndpointId)
        : RemoteSessionCoordinator::Binding();
    const bool closing = hasPendingOutgoingSessionClose(targetEndpointId)
        || hasCancelledInitialOpenForTarget(targetEndpointId)
        || m_locallyTerminatingRemoteSessions.contains(binding.remoteSessionId)
        || binding.phase == QLatin1String("Terminating")
        || binding.phase == QLatin1String("CleanupPending");
    const bool retained = !closing && hasRetainedSession(m_webSocketClient, binding);

    // Without our server connection, an old presence observation cannot prove
    // that the peer is offline. Explicit Disable also revokes recovery intent.
    if (!localDiscoveryUsable || isUserDisconnected() || isConnectionDraining()) {
        return retained && !isUserDisconnected() && !isConnectionDraining()
            ? QStringLiteral("Degraded") : QStringLiteral("Unreachable");
    }
    if (presence) {
        if (!presence->isOnline()) {
            return retained ? QStringLiteral("Degraded") : QStringLiteral("Disconnected");
        }
        if (!presence->canAcceptSession()) {
            return presence->availabilityBadgeText();
        }
    } else {
        return retained ? QStringLiteral("Degraded") : QStringLiteral("Disconnected");
    }
    if (closing) return QStringLiteral("Disconnecting");
    if (isCommandReadyBinding(m_webSocketClient, binding)) return QStringLiteral("Connected");
    if (retained) {
        const bool recovering = binding.degraded || binding.phase == QLatin1String("Grace")
            || m_webSocketClient->isSessionRecovering(binding.remoteSessionId)
            || m_webSocketClient->sessionRecoveryInProgress(binding.remoteSessionId);
        return recovering ? QStringLiteral("Degraded") : QStringLiteral("Connecting");
    }
    if (m_remoteSessionOpenPendingTargets.contains(targetEndpointId)
        || binding.phase == QLatin1String("Opening")) {
        return QStringLiteral("Connecting");
    }
    // A terminal or expired binding is not evidence of a network outage.
    return QStringLiteral("Available");
}

void ApplicationRuntime::refreshRemoteConnectionPresentation(bool propagateLoss)
{
    const ClientInfo* presence = nullptr;
    for (const ClientInfo& client : m_discoveredClients) {
        if (client.endpointId() == m_activeWorkspaceEndpointId) {
            presence = &client;
            break;
        }
    }
    m_remoteStatusText = remoteConnectionStatus(m_activeWorkspaceEndpointId, presence,
        m_connectionManager && m_connectionManager->isReady()).toUpper();
    const auto binding = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator()->outgoingForPeer(m_activeWorkspaceEndpointId)
        : RemoteSessionCoordinator::Binding();
    // Command capability remains independent of the presentation label.
    m_remoteClientConnected = isCommandReadyBinding(m_webSocketClient, binding)
        && m_connectionManager && m_connectionManager->isReady()
        && !isUserDisconnected() && !isConnectionDraining()
        && !hasPendingOutgoingSessionClose(m_activeWorkspaceEndpointId)
        && !m_locallyTerminatingRemoteSessions.contains(binding.remoteSessionId);
    // Volume is retained project data, just like screen topology. Losing
    // command readiness must not erase the last authenticated reading.
    const ProjectRecord* project = m_projectManager
        ? m_projectManager->projectForTarget(m_activeWorkspaceEndpointId) : nullptr;
    m_remoteVolumePercent = project ? project->savedVolumePercent : -1;
    const bool retainedRecovery = m_webSocketClient && !binding.remoteSessionId.isEmpty()
        && (binding.phase == QLatin1String("Active") || binding.phase == QLatin1String("Grace"))
        && m_webSocketClient->sessionRecoveryRemainingMs(binding.remoteSessionId) > 0;
    refreshOverlayActionsState(m_remoteClientConnected, propagateLoss && !retainedRecovery);
    emit presentationStateChanged();
}

void ApplicationRuntime::refreshOverlayActionsState(bool remoteConnected, bool propagateLoss) {
    m_remoteOverlayActionsEnabled = remoteConnected;

    ICanvasHost* activeCanvas = getActiveCanvas();
    if (activeCanvas) {
        if (!remoteConnected && propagateLoss) {
            activeCanvas->handleRemoteConnectionLost();
        }
        activeCanvas->setOverlayActionsEnabled(remoteConnected);
    }

    if (m_uploadManager) emit m_uploadManager->uiStateChanged();
    emit presentationStateChanged();
}

ApplicationRuntime::ApplicationRuntime(const RuntimeProfileContext& runtimeProfile,
                                       QObject* parent)
    : QObject(parent),
      m_fileManager(new FileManager()),
      m_activityMonitor(new ApplicationActivityMonitor(this)),
      m_workspaceManager(new WorkspaceManager(this)),
      m_projectManager(new ProjectManager(projectTimingPolicyFromConfig(), this)),
      m_sceneActivityModel(new SceneActivityModel(this)),
      m_systemMonitor(new SystemMonitor(this)),
      m_systemTrayManager(new SystemTrayManager(this)),
      m_webSocketClient(new WebSocketClient(
          RuntimeProfile::identityLocation(), runtimeProfile.useNativeIdentityVault, this, {},
          runtimeProfile.ordinal)),
      m_connectionManager(new ConnectionManager(m_webSocketClient, this)),
      m_settingsManager(new SettingsManager(this)),
      m_profileCache(new ClientProfileCache(this)),
      m_webSocketMessageHandler(new WebSocketMessageHandler(this, this)),
      m_screenEventHandler(new ScreenEventHandler(this, this)),
      m_uploadEventHandler(new UploadEventHandler(this, this)),
      m_clientListEventHandler(new ClientListEventHandler(this, m_webSocketClient, this)),
      m_workspaceController(new ClientWorkspaceController(this, this)),
      m_uploadSignalConnector(new UploadSignalConnector(this)),
      m_incomingSessionOrphanWatchdog(
          new IncomingSessionOrphanWatchdog(this)),
      m_uploadManager(new UploadManager(m_fileManager, this)),
      m_fileWatcher(new FileWatcher(this)),
      m_navigationManager(new ScreenNavigationManager(this))
{
    connect(&MediaResidencyManager::instance(), &MediaResidencyManager::errorOccurred,
            this, [](const QString& owner, const QString& reason) {
        if (owner.startsWith(QLatin1String("remote:")))
            TOAST_ERROR(QStringLiteral("Received media could not be prepared: %1").arg(reason));
    });
    connect(m_connectionManager, &ConnectionManager::disconnectRequested,
            this, &ApplicationRuntime::beginControlledDisconnect);
    connect(m_connectionManager, &ConnectionManager::connectionEnabledChanged,
            this, [this](bool) { emit presentationStateChanged(); });
    connect(m_webSocketClient, &WebSocketClient::endpointDisableAcknowledged,
            this, [this](const QString& requestId, quint64 generation) {
        if (!m_controlledDisconnectInProgress
            || requestId != m_controlledDisconnectRequestId
            || generation != m_controlledDisconnectGeneration) return;
        m_controlledDisconnectAcknowledged = true;
        finishControlledDisconnectIfReady();
    });

    connect(m_activityMonitor, &ApplicationActivityMonitor::inactivityStarted,
            this, [this](qint64 atMs) {
        if (m_workspaceManager) {
            for (ClientWorkspace* workspace : m_workspaceManager->allWorkspaces()) {
                if (workspace && workspace->canvas && m_projectManager
                    && m_projectManager->hasProjectForTarget(
                        workspace->targetEndpointId)) {
                    persistProjectCanvas(workspace->targetEndpointId);
                }
            }
            m_workspaceManager->markAllWorkspacesHidden(atMs);
        }
        if (m_projectManager) m_projectManager->markAllHidden(atMs);
        refreshProjectClientList();
    });
    connect(m_activityMonitor, &ApplicationActivityMonitor::activityResumed,
            this, [this](qint64 atMs) {
        m_sessionRecovery.suspend();
        if (m_workspaceManager) m_workspaceManager->processDeadlines(atMs);
        if (m_projectManager) m_projectManager->processDeadlines(atMs);
        if (m_projectManager) {
            for (const ProjectRecord& project : m_projectManager->projects()) {
                m_projectManager->setVisible(project.targetEndpointId, atMs);
            }
        }
        if (m_workspaceManager) {
            for (const QString& target :
                 m_workspaceManager->allTargetEndpointIds()) {
                m_workspaceManager->setWorkspaceVisible(target, atMs);
            }
        }

        // Activity on the selected Canvas is ongoing session intent, whatever
        // caused the previous session to end. Recompute it instead of retaining
        // a one-shot flag that could outlive pointer presence or navigation.
        reconcileForegroundRemoteSession();
        refreshProjectClientList();
    });

    m_workspaceManager->setRemoteSessionHiddenTimeoutMs(
        AppConfig::instance().remoteSessionHiddenTimeoutMs());
    m_incomingSessionOrphanWatchdog->setConfiguredTimeoutMs(
        AppConfig::instance().incomingSessionOrphanTimeoutMs());
    connect(m_incomingSessionOrphanWatchdog,
            &IncomingSessionOrphanWatchdog::orphanedSessionsDue,
            this, [this](const QSet<QString>& dueSessionIds) {
        m_incomingOrphanSessionIds = dueSessionIds;
        m_terminalIncomingSessionFilter = dueSessionIds;
        beginTerminalIncomingCacheCleanup(
            QStringLiteral("incoming_session_orphan_timeout"));
        m_incomingOrphanSessionIds.clear();
    });
    connect(m_workspaceManager, &WorkspaceManager::remoteSessionCloseDue,
            this, [this](const QString& targetEndpointId) {
        terminateProjectRemoteSession(targetEndpointId, true);
    });

    m_settingsManager->loadSettings();
    m_screenPreviewStore = new ProjectScreenPreviewStore(
        QFileInfo(RuntimeProfile::projectsFilePath()).dir().filePath(
            QStringLiteral("screen-previews-v1")), this);
    connect(m_screenPreviewStore, &ProjectScreenPreviewStore::persistenceError,
            this, [](const QString& error) {
        qWarning().noquote() << "Screen preview persistence error:" << error;
    });
    connect(m_screenPreviewStore, &ProjectScreenPreviewStore::frameRestored, this,
            [this](const QString& projectId, int screenId, const QImage& image) {
        if (m_cleanShutdownPrepared || !m_settingsManager->getScreenContentVisible()) return;
        const auto* project = m_projectManager->projectById(projectId);
        if (!project) return;
        if (auto* canvas = canvasForEndpointId(project->targetEndpointId);
            canvas && canvas->document() && !canvas->document()->mediaResidencySuspended())
            canvas->restoreRemoteScreenFrame(screenId, image);
    });
    // A previous Hide is durable even if the process stopped during cleanup.
    if (!m_settingsManager->getScreenContentVisible())
        m_screenPreviewClearPending = !m_screenPreviewStore->clearAll();
    m_settingsManager->setScreenContentEnableGuard([this](QString* error) {
        if (!m_screenPreviewClearPending) return true;
        m_screenPreviewClearPending = !m_screenPreviewStore->clearAll();
        if (m_screenPreviewClearPending && error)
            *error = tr("The saved screen images could not be erased. Please try again.");
        return !m_screenPreviewClearPending;
    });
    m_screenSharing = new ScreenSharingService(m_webSocketClient, m_systemMonitor, this);
    m_audioSharing = new AudioSharingService(m_webSocketClient, this);
    connect(m_audioSharing, &AudioSharingService::statusChanged,
            this, &ApplicationRuntime::audioSharingStatusChanged);
    connect(m_audioSharing, &AudioSharingService::stateChanged,
            this, &ApplicationRuntime::presentationStateChanged);
    connect(m_audioSharing, &AudioSharingService::remoteStatusChanged,
            this, &ApplicationRuntime::presentationStateChanged);
    connect(m_settingsManager, &SettingsManager::audioSharingEnabledChanged,
            m_audioSharing, &AudioSharingService::setSharingEnabled);
    connect(m_settingsManager, &SettingsManager::systemAudioEnabledChanged,
            m_audioSharing, &AudioSharingService::setListeningEnabled);
    connect(m_screenSharing, &ScreenSharingService::sourceBudgetChanged,
            m_audioSharing, &AudioSharingService::setSourceBudget);
    connect(m_audioSharing, &AudioSharingService::sourceReservationChanged,
            m_screenSharing, &ScreenSharingService::setAudioReservationBps);
    connect(m_audioSharing, &AudioSharingService::playbackClock,
            m_screenSharing, &ScreenSharingService::setAudioPlaybackClock);
    connect(m_audioSharing, &AudioSharingService::outputQuantumChanged,
            m_screenSharing, &ScreenSharingService::setAudioOutputQuantumUs);
    connect(m_audioSharing, &AudioSharingService::playbackReset,
            m_screenSharing, &ScreenSharingService::clearAudioPlaybackClock);
    connect(m_screenSharing, &ScreenSharingService::sourceTimestampObserved,
            m_audioSharing, &AudioSharingService::observeVideoTimestamp);
    connect(m_audioSharing, &AudioSharingService::remoteIssue, this,
            [this](const QString& endpoint, const QString& message) {
        if (!m_toastSystem || m_cleanShutdownPrepared || isUserDisconnected()
            || !m_qmlWindowVisible || m_applicationPage != 1
            || endpoint != m_activeWorkspaceEndpointId
            || !m_settingsManager->getSystemAudioEnabled()) return;
        NotificationRequest notification;
        notification.severity = NotificationSeverity::Warning;
        notification.category = QStringLiteral("Audio sharing");
        notification.message = message;
        notification.remoteSessionId = m_webSocketClient->remoteSessionCoordinator()
                                          ->outgoingForPeer(endpoint).remoteSessionId;
        notification.peers = {{endpoint, m_selectedClient.getMachineName(),
                               m_selectedClient.instanceOrdinal(), QStringLiteral("From")}};
        m_toastSystem->publishNotification(notification);
    });
    connect(m_screenSharing, &ScreenSharingService::statusChanged,
            this, &ApplicationRuntime::screenSharingStatusChanged);
    connect(m_settingsManager, &SettingsManager::screenSharingEnabledChanged,
            m_screenSharing, &ScreenSharingService::setSharingEnabled);
    connect(m_settingsManager, &SettingsManager::screenContentVisibleChanged, this,
            [this](bool visible) {
        if (!visible) {
            // This preference is global: forget every project's last screen image.
            // Invalidate disk/queued reads before the stream is unsubscribed.
            m_screenPreviewClearPending = !m_screenPreviewStore->clearAll();
            for (auto* workspace : m_workspaceManager->allWorkspaces())
                if (workspace && workspace->canvas) workspace->canvas->clearRemoteScreenFrames();
        }
        refreshMediaSharing();
    });
    connect(m_screenSharing, &ScreenSharingService::frameReady, this,
            [this](const QString& endpoint, int screenId, const QVideoFrame& frame) {
        if (m_cleanShutdownPrepared || !frame.isValid()
            || !m_settingsManager->getScreenContentVisible()) return;
        const auto* project = m_projectManager->projectForTarget(endpoint);
        auto* canvas = canvasForEndpointId(endpoint);
        if (!project || !canvas || !canvas->document()
            || canvas->document()->mediaResidencySuspended()) return;
        const auto screens = canvas->document()->screens();
        if (std::none_of(screens.cbegin(), screens.cend(),
            [screenId](const ScreenInfo& screen) { return screen.id == screenId; })) return;
        canvas->setRemoteScreenFrame(screenId, frame);
        m_screenPreviewStore->retain(project->projectId, screenId, frame);
    });
    // These signals revoke decoder/stream state. Last project pixels remain
    // visible while the live status still reports the interruption truthfully.
    connect(m_screenSharing, &ScreenSharingService::frameCleared, this,
            [this](const QString&, int) { m_screenPreviewStore->flush(); });
    connect(m_screenSharing, &ScreenSharingService::framesCleared, this,
            [this](const QString&) { m_screenPreviewStore->flush(); });
    connect(m_screenSharing, &ScreenSharingService::remoteStateChanged,
            this, &ApplicationRuntime::presentationStateChanged);
    connect(m_screenSharing, &ScreenSharingService::remoteIssue, this,
            [this](const QString& endpoint, const QString& message) {
        if (!m_toastSystem || m_cleanShutdownPrepared || isUserDisconnected()
            || !m_qmlWindowVisible || m_applicationPage != 1
            || endpoint != m_activeWorkspaceEndpointId
            || !m_settingsManager->getScreenContentVisible()) return;
        NotificationRequest notification;
        notification.severity = NotificationSeverity::Warning;
        notification.category = QStringLiteral("Screen sharing");
        notification.message = message;
        notification.remoteSessionId = m_webSocketClient->remoteSessionCoordinator()
                                          ->outgoingForPeer(endpoint).remoteSessionId;
        notification.peers = {{endpoint, m_selectedClient.getMachineName(),
                               m_selectedClient.instanceOrdinal(), QStringLiteral("From")}};
        m_toastSystem->publishNotification(notification);
    });
    connect(this, &ApplicationRuntime::activeWorkspaceChanged, this, &ApplicationRuntime::refreshMediaSharing);
    connect(this, &ApplicationRuntime::applicationPageChanged, this, &ApplicationRuntime::refreshMediaSharing);
    m_screenSharing->setSharingEnabled(m_settingsManager->getScreenSharingEnabled());
    m_audioSharing->setListeningEnabled(m_settingsManager->getSystemAudioEnabled());
    m_audioSharing->setSharingEnabled(m_settingsManager->getAudioSharingEnabled());
    m_profileCache->setPictureRequester([this](const QString& endpoint, const QString& hash) {
        return m_webSocketClient->requestProfilePicture(endpoint, hash);
    });
    connect(m_webSocketClient, &WebSocketClient::clientListReceived,
            m_profileCache, &ClientProfileCache::observe);
    connect(m_webSocketClient, &WebSocketClient::profilePictureReceived,
            m_profileCache, &ClientProfileCache::acceptPicture);
    connect(m_webSocketClient, &WebSocketClient::disconnected,
            m_profileCache, &ClientProfileCache::transportReset);
    connect(m_webSocketClient, &WebSocketClient::connected,
            m_profileCache, &ClientProfileCache::transportReset);
    connect(m_settingsManager, &SettingsManager::serverUrlChanged,
            m_profileCache, &ClientProfileCache::clearPeers);
    if (m_projectManager) {
        connect(m_projectManager, &ProjectManager::projectCheckpointDue,
                this, &ApplicationRuntime::persistProjectCanvas);
        connect(m_projectManager, &ProjectManager::projectMediaReleaseDue,
                this, [this](const QString&, const QString& targetEndpointId) {
            reconcileProjectMediaResidency(targetEndpointId);
        });
        connect(m_projectManager, &ProjectManager::projectVisibilityChanged,
                this, [this](const QString&, const QString& targetEndpointId,
                             ProjectLifecycleState) {
            reconcileProjectMediaResidency(targetEndpointId);
        });
        connect(m_projectManager, &ProjectManager::projectRemoved,
                this, [this](const QString& projectId, const QString& targetEndpointId,
                             ProjectManager::RemovalReason reason) {
            m_screenPreviewStore->removeProject(projectId);
            const bool displayed = m_applicationPage == 1 && m_navigationManager
                && m_navigationManager->isOnScreenView()
                && m_navigationManager->currentClientId() == targetEndpointId;
            m_remoteSessionOpenDesiredTargets.remove(targetEndpointId);
            m_remoteSessionOpenSuppressedTargets.insert(targetEndpointId);
            m_remoteSessionAutoOpenBlockedTargets.remove(targetEndpointId);
            m_sessionRecovery.reset(targetEndpointId);
            cancelPendingRemoteSessionOpen(targetEndpointId);
            if (m_selectionEndpointId == targetEndpointId) {
                m_selectionEndpointId.clear();
                m_selectionClient = ClientInfo();
            }
            // projectRemoved follows the durable commit. Leave the displayed
            // page before clearing its graph, without waiting for wire cleanup.
            if (displayed) showClientListView();
            clearDeletedProjectFromWorkspace(targetEndpointId);
            // Closing remains tracked independently of the deleted workspace.
            terminateProjectRemoteSession(targetEndpointId, true,
                reason == ProjectManager::RemovalReason::RetentionExpired
                    ? QStringLiteral("project_retention_expired")
                    : QStringLiteral("user_project_deleted"));
        });
        connect(m_projectManager, &ProjectManager::projectsChanged,
                this, &ApplicationRuntime::refreshProjectClientList);
        connect(m_projectManager, &ProjectManager::persistenceError,
                this, [](const QString& message) {
            qWarning().noquote() << "Project persistence error:" << message;
        });
    }
    if (m_workspaceController) {
        QTimer::singleShot(0, this, [this]() {
            if (m_workspaceController) {
                m_workspaceController->prewarmQuickCanvasHost();
            }
        });
    }
    if (QCoreApplication::instance()) QCoreApplication::instance()->installEventFilter(this);
    connect(m_uploadManager, &UploadManager::incomingFileReadersChanged, this, [this]() {
        const auto sessions = m_readerTeardownPendingSessionIds;
        for (const auto& session : sessions) {
            if (!m_uploadManager->incomingFileReadersSettled({session})) continue;
            m_readerTeardownPendingSessionIds.remove(session);
            handleRemoteRendererTeardownSettled(session, true);
        }
        finishTerminalIncomingCacheCleanupIfReady();
        if (m_cleanShutdownPrepared) finishCleanShutdownIncomingCacheTeardownIfReady();
    });
    m_remoteSceneController = new RemoteSceneController(
        m_fileManager, m_webSocketClient, this);
    connect(m_remoteSceneController, &RemoteSceneController::teardownSettled,
            this, &ApplicationRuntime::handleRemoteRendererTeardownSettled,
            Qt::UniqueConnection);
    if (m_systemTrayManager) {
        m_systemTrayManager->setup();
        connect(m_systemTrayManager, &SystemTrayManager::activated, this,
                [this](SystemTrayManager::ActivationReason reason) {
            onTrayIconActivated(static_cast<int>(reason));
        });
    }
    
    if (m_systemMonitor) {
        // Publish native notifications immediately on the same control socket
        // as the remote cursor, without waiting for the inventory refresh.
        connect(m_systemMonitor, &SystemMonitor::volumeChanged, this, [this](int volume) {
            if (m_webSocketClient && m_webSocketClient->isConnected()
                && (!m_uploadManager || m_uploadManager->receiverReadyForAdvertisement())) {
                m_webSocketClient->updateSystemVolume(volume);
            }
        });
        m_systemMonitor->startVolumeMonitoring();
        connect(m_systemMonitor, &SystemMonitor::screenConfigurationChanged,
                this, [this](const QList<ScreenInfo>&) {
            if (m_webSocketClient && m_webSocketClient->isConnected()) {
                syncRegistration();
            }
        });
    }

    if (m_webSocketMessageHandler) {
        m_webSocketMessageHandler->setupConnections(m_webSocketClient);
    }
    
    if (m_screenEventHandler) {
        m_screenEventHandler->setupConnections(m_webSocketClient);
    }
    
    if (m_clientListEventHandler) {
        m_clientListEventHandler->setupConnections(m_webSocketClient);
    }
    // The handler above installs discovery first. Reconcile the foreground
    // session afterwards, using the same intent and OPEN/CLOSE guards as
    // activity resumption and terminal completion.
    connect(m_webSocketClient, &WebSocketClient::clientListReceived,
            this, &ApplicationRuntime::onClientListReceived);

    m_sessionRecovery.ready = [this](const QString& endpoint) {
        if (hasPendingOutgoingSessionClose(endpoint)) retryPendingOutgoingSessionClose(endpoint, QStringLiteral("retry_after_rejection"));
        if (endpoint == m_activeWorkspaceEndpointId) reconcileForegroundRemoteSession();
    };
    m_sessionRecovery.changed = [this] { emit presentationStateChanged(); };
    m_deviceSnapshotTimer = new QTimer(this);
    m_deviceSnapshotTimer->setInterval(5000);
    connect(m_deviceSnapshotTimer, &QTimer::timeout, this, &ApplicationRuntime::syncRegistration);
    connect(m_webSocketClient, &WebSocketClient::localDeviceSnapshotRequested,
            this, &ApplicationRuntime::syncRegistration);
    m_cursorClock.start();
    m_cursorPublishTimer = new QTimer(this);
    m_cursorPublishTimer->setTimerType(Qt::PreciseTimer);
    m_cursorPublishTimer->setInterval(16);
    connect(m_cursorPublishTimer, &QTimer::timeout,
            this, &ApplicationRuntime::publishLocalCursor);
    m_cursorExpiryTimer = new QTimer(this);
    m_cursorExpiryTimer->setInterval(500);
    connect(m_cursorExpiryTimer, &QTimer::timeout,
            this, &ApplicationRuntime::expireStaleRemoteCursors);
    if (auto* sessions = m_webSocketClient->remoteSessionCoordinator()) {
        connect(sessions, &RemoteSessionCoordinator::sessionChanged, this,
                [this](const QString& sessionId, quint64 generation, const QString&) {
            if (m_publishedCursors.contains(sessionId)
                && m_publishedCursors.value(sessionId).generation != generation) {
                m_publishedCursors[sessionId].lastSentAtMs = -1;
            }
            refreshRemoteCursorStreaming();
        });
        connect(sessions, &RemoteSessionCoordinator::sessionRemoved, this,
                [this](const QString& sessionId) {
            m_publishedCursors.remove(sessionId);
            refreshRemoteCursorStreaming();
        });
    }
    connect(m_webSocketClient, &WebSocketClient::connected,
            this, &ApplicationRuntime::refreshRemoteCursorStreaming);
    connect(m_webSocketClient, &WebSocketClient::disconnected,
            this, &ApplicationRuntime::refreshRemoteCursorStreaming);
    // The coordinator publishes identity before WebSocketClient installs the
    // authenticated deadline. Re-evaluate command capability after the complete
    // state has been applied, especially for a newly opened session.
    connect(m_webSocketClient, &WebSocketClient::remoteSessionOpened,
            this, &ApplicationRuntime::refreshRemoteCursorStreaming);
    connect(m_webSocketClient, &WebSocketClient::remoteSessionResumed,
            this, &ApplicationRuntime::refreshRemoteCursorStreaming);
    connect(m_webSocketClient, &WebSocketClient::remoteSessionLeaseStateChanged,
            this, &ApplicationRuntime::refreshRemoteCursorStreaming);
    connect(m_webSocketClient, &WebSocketClient::remoteCursorReceived, this,
            [this](const QString& sessionId, int screenId,
                   const QPointF& position, bool visible) {
        const auto binding = m_webSocketClient->remoteSessionCoordinator()->byId(sessionId);
        if (!isCommandReadyBinding(m_webSocketClient, binding)
            || binding.ownerEndpointId != m_webSocketClient->endpointId()) return;
        auto* workspace = m_workspaceManager->findWorkspace(binding.targetEndpointId);
        if (!workspace || !workspace->canvas) return;
        m_receivedCursorAtByEndpoint[binding.targetEndpointId] = m_cursorClock.elapsed();
        if (visible) workspace->canvas->updateRemoteCursor(screenId, position);
        else workspace->canvas->hideRemoteCursor();
    });

    // SceneActivityModel is the sole source for the Ongoing Scenes UI. A run
    // is inserted only after the protocol v4 coordinator reaches Live and is
    // removed as soon as it leaves Live.
    if (m_sceneActivityModel && m_webSocketClient) {
        m_sceneActivityModel->setLocalEndpointId(m_webSocketClient->endpointId());
        if (SceneRunCoordinator* sceneRuns = m_webSocketClient->sceneRunCoordinator()) {
            connect(sceneRuns, &SceneRunCoordinator::runChanged, this,
                    [this, sceneRuns](const QString& sceneRunId,
                                      SceneRunCoordinator::Phase phase) {
                if (!m_sceneActivityModel) return;
                if (phase != SceneRunCoordinator::Phase::Live) {
                    m_sceneActivityModel->remove(sceneRunId);
                    return;
                }
                const SceneRunCoordinator::Run run = sceneRuns->run(sceneRunId);
                if (run.sceneRunId.isEmpty()) return;
                qint64 startedAt = run.startEpochMs;
                if (startedAt <= 0) {
                    const SceneActivityModel::Activity existing =
                        m_sceneActivityModel->activity(sceneRunId);
                    startedAt = existing.startedAtEpochMs > 0
                        ? existing.startedAtEpochMs
                        : MouffetteClock::anchoredEpochMs();
                }
                m_sceneActivityModel->upsertLive(
                    run.sceneRunId, run.remoteSessionId, run.ownerEndpointId,
                    run.targetEndpointId, startedAt);
            });
        }
        connect(m_webSocketClient, &WebSocketClient::transportHealthChanged,
                this, [this](bool degraded) {
            if (degraded) {
                m_sceneActivityModel->setAllDegraded(true);
                return;
            }
            // Authentication alone does not recover a retained scene's session.
            for (const auto& binding : m_webSocketClient->remoteSessionCoordinator()->all())
                m_sceneActivityModel->setSessionDegraded(binding.remoteSessionId,
                    !isCommandReadyBinding(m_webSocketClient, binding));
        });
        connect(m_webSocketClient, &WebSocketClient::remoteSessionLeaseStateChanged,
                this, [this](const QJsonObject& envelope) {
            handleRemoteSessionLeaseState(envelope);
            if (!m_sceneActivityModel) return;
            const QString sessionId = envelope.value(QStringLiteral("remoteSessionId")).toString();
            const QString phase = envelope.value(QStringLiteral("phase"))
                                      .toString(envelope.value(QStringLiteral("state")).toString());
            if (phase.compare(QStringLiteral("Grace"), Qt::CaseInsensitive) == 0) {
                m_sceneActivityModel->setSessionDegraded(sessionId, true);
            } else if (phase.compare(QStringLiteral("Active"), Qt::CaseInsensitive) == 0) {
                m_sceneActivityModel->setSessionDegraded(sessionId,
                    !m_webSocketClient->canIssueSessionCommands(sessionId));
            } else if (phase.compare(QStringLiteral("Terminating"), Qt::CaseInsensitive) == 0
                       || phase.compare(QStringLiteral("CleanupPending"), Qt::CaseInsensitive) == 0
                       || phase.compare(QStringLiteral("Closed"), Qt::CaseInsensitive) == 0) {
                m_sceneActivityModel->removeForSession(sessionId);
            }
        });
        connect(m_webSocketClient, &WebSocketClient::remoteSessionTerminating,
                this, [this](const QJsonObject& envelope) {
            handleRemoteSessionTerminating(envelope);
            if (m_sceneActivityModel) {
                m_sceneActivityModel->removeForSession(
                    envelope.value(QStringLiteral("remoteSessionId")).toString());
            }
        });
        connect(m_webSocketClient, &WebSocketClient::remoteSessionClosed,
                this, [this](const QJsonObject& envelope) {
            handleRemoteSessionClosed(envelope);
            if (m_sceneActivityModel) {
                m_sceneActivityModel->removeForSession(
                    envelope.value(QStringLiteral("remoteSessionId")).toString());
            }
        });
        connect(m_webSocketClient, &WebSocketClient::serverRestarted,
                m_sceneActivityModel, [this](const QString&, const QString&) {
            if (m_sceneActivityModel) m_sceneActivityModel->clear();
            m_sessionRecovery.suspend();
            const QString selectedTarget = m_activeWorkspaceEndpointId;
            const bool preserveSelectedOpenIntent =
                !selectedTarget.isEmpty()
                && m_navigationManager
                && m_navigationManager->isOnScreenView()
                && m_remoteSessionOpenDesiredTargets.contains(selectedTarget);
            // RemoteSession identifiers are scoped to a server boot. No
            // terminal tombstone for the old boot can arrive afterwards.
            const QList<QString> abandonedCloseTargets =
                m_pendingOutgoingSessionCloses.keys();
            m_pendingOutgoingSessionCloses.clear();
            m_outgoingSessionCloseRequestById.clear();
            m_currentOutgoingSessionIdByTarget.clear();
            m_currentOutgoingSessionGenerationByTarget.clear();
            m_remoteSessionOpenDesiredTargets.clear();
            if (preserveSelectedOpenIntent) {
                m_remoteSessionOpenDesiredTargets.insert(selectedTarget);
            }
            m_cancelledInitialOpenTargetByRequestId.clear();
            m_cancelledInitialOpenSessionByTarget.clear();
            m_locallyTerminatingRemoteSessions.clear();
            // expireLease() ran synchronously before serverRestarted and
            // converted every incoming binding into the local terminal-cleanup
            // FIFO. Protocol acknowledgements, however, are scoped to the old
            // boot and must never be replayed to the replacement server.
            m_pendingTeardownAcks.clear();
            m_pendingRendererTeardowns.clear();
            if (m_workspaceManager) {
                for (const QString& target : abandonedCloseTargets) {
                    m_workspaceManager->setRemoteSessionState(
                        target, WorkspaceManager::RemoteSessionState::Absent);
                    updateWorkspaceCapabilities(target);
                }
            }
        });
        connect(m_webSocketClient, &WebSocketClient::remoteSessionOpened,
                this, [this](const QJsonObject& envelope) {
            handleRemoteSessionReady(envelope, false);
        });
        connect(m_webSocketClient, &WebSocketClient::remoteSessionResumed,
                this, [this](const QJsonObject& envelope) {
            cancelIncomingSessionOrphanWatchdogIfResumed(envelope);
            handleRemoteSessionReady(envelope, true);
        });
        connect(m_webSocketClient, &WebSocketClient::remoteSessionSnapshotReceived,
                this, &ApplicationRuntime::handleRemoteSessionSnapshot);
        connect(m_webSocketClient, &WebSocketClient::remoteSessionError,
                this, &ApplicationRuntime::handleRemoteSessionError);
    }
    
    // ConnectionManager is the sole owner of retries and transport status.
    connect(m_connectionManager, &ConnectionManager::connectionError,
            this, &ApplicationRuntime::onConnectionError);
    connect(m_webSocketClient, &WebSocketClient::connected,
            this, [this]() {
        if (isUserDisconnected() || m_controlledDisconnectInProgress) return;
        m_transportOutageNotified = false;
        m_intentionalTransportClose = false;
        retryPendingTeardownAcks();
        // A CLOSE acknowledged only by silence on the former socket is not a
        // commit. Retry its immutable session identity once on this newly
        // authenticated transport, even though lease expiry may have cleared
        // the coordinator binding.
        const QList<QString> pendingCloseTargets =
            m_pendingOutgoingSessionCloses.keys();
        for (const QString& target : pendingCloseTargets) {
            retryPendingOutgoingSessionClose(
                target, QStringLiteral("transport_reauthenticated"));
        }
        // An OPEN may have reached the server without its Opening/Ready reply
        // reaching this socket. Reuse its idempotency key after authentication;
        // otherwise a pending flag with no resumable binding can stick forever.
        // Known bindings are recovered by WebSocketClient's signed RESUME.
        const auto pendingOpens = m_remoteSessionOpenTargetByRequestId;
        const RemoteSessionCoordinator* coordinator =
            m_webSocketClient->remoteSessionCoordinator();
        for (auto it = pendingOpens.cbegin(); it != pendingOpens.cend(); ++it) {
            if (!m_remoteSessionOpenPendingTargets.contains(it.value())
                || hasPendingOutgoingSessionClose(it.value())
                || hasCancelledInitialOpenForTarget(it.value())) continue;
            const auto binding = coordinator
                ? coordinator->outgoingForPeer(it.value())
                : RemoteSessionCoordinator::Binding();
            if (binding.remoteSessionId.isEmpty()) {
                m_webSocketClient->replayRemoteSessionOpen(it.value(), it.key());
            }
        }
        // A user can leave the initial loading page before Opening returns a
        // session identity. If that socket then disappears, a target-only
        // cancellation must not fence the device forever. Replay the exact
        // idempotency key on the authenticated replacement transport; the
        // resulting Ready/terminal/error retires only that cancelled attempt.
        const auto cancelledWithoutIdentity =
            m_cancelledInitialOpenTargetByRequestId;
        for (auto it = cancelledWithoutIdentity.cbegin();
             it != cancelledWithoutIdentity.cend(); ++it) {
            if (hasPendingOutgoingSessionClose(it.value())) continue;
            m_webSocketClient->replayRemoteSessionOpen(it.value(), it.key());
        }
    });
    connect(m_webSocketClient, &WebSocketClient::transportConnected,
            m_incomingSessionOrphanWatchdog,
            &IncomingSessionOrphanWatchdog::transportConnected);
    connect(m_webSocketClient, &WebSocketClient::disconnected,
            this, [this]() {
        if (m_controlledDisconnectInProgress) {
            finishControlledDisconnect();
        }
        if (!m_intentionalTransportClose && !isUserDisconnected()) {
            armIncomingSessionOrphanWatchdog();
        }
        if (m_cleanShutdownPrepared && !m_cleanShutdownFinished) {
            finishCleanShutdown();
            return;
        }

        const QString selectedTarget = m_activeWorkspaceEndpointId;
        const bool selectedHasProject = m_projectManager
            && m_projectManager->hasProjectForTarget(selectedTarget);
        const bool initialOpenInterrupted = !selectedTarget.isEmpty()
            && m_navigationManager && m_navigationManager->isOnScreenView()
            && !selectedHasProject;

        m_discoveredClients.clear();
        markAllWorkspacesDisconnected();
        refreshProjectClientList();

        if (initialOpenInterrupted) {
            // Retain the selected endpoint even before its first snapshot.
            refreshRemoteConnectionPresentation(false);
            if (m_navigationManager) m_navigationManager->revealCanvas();
        } else if (selectedHasProject) {
            refreshProjectClientList();
            refreshRemoteConnectionPresentation(false);
            if (m_navigationManager) m_navigationManager->revealCanvas();
        }
        if (!m_intentionalTransportClose && !isUserDisconnected()
            && !m_transportOutageNotified && m_toastSystem) {
            m_transportOutageNotified = true;
            NotificationRequest notification;
            notification.severity = NotificationSeverity::Warning;
            notification.category = QStringLiteral("Connection");
            notification.message = initialOpenInterrupted
                ? QStringLiteral("The server connection was lost before the remote client could be opened.")
                : QStringLiteral("The server connection was lost. Reconnection will continue in the background.");
            notification.correlationId = QStringLiteral("server-outage");
            m_toastSystem->publishNotification(notification);
        }
    });
    connect(m_connectionManager, &ConnectionManager::retryStateChanged, this, &ApplicationRuntime::presentationStateChanged);
    connect(m_connectionManager, &ConnectionManager::statusChanged,
            this, [this](const QString& status) {
        setLocalNetworkStatus(status);
        if (!m_connectionManager->isReady()) {
            if (m_activeCanvas) {
                // Grace disables new remote commands but does not tear down the
                // running graph or discard resumable upload state.
                m_activeCanvas->setOverlayActionsEnabled(false);
            }
        }
        // Reproject on every state transition, including degradation and
        // recovery without a new presence revision. Keep raw discovery intact.
        refreshProjectClientList();
        if (m_connectionManager->isReady()) {
            const auto clients = m_discoveredClients;
            onClientListReceived(clients);
            reconcileForegroundRemoteSession();
        } else if (!m_activeWorkspaceEndpointId.isEmpty()) {
            refreshRemoteConnectionPresentation(false);
        }
    });
    connect(m_connectionManager, &ConnectionManager::leaseExpired,
            this, [this](const QString& boot, quint64 generation) {
        handleTerminalTransportLoss(QStringLiteral("lease_expired"), boot,
                                    generation, !isUserDisconnected());
    });
    connect(m_webSocketClient, &WebSocketClient::sessionsInvalidated,
            this, [this](const QString& reason, const QString& boot, quint64 generation) {
        handleTerminalTransportLoss(reason, boot, generation, false);
    });
    connect(m_webSocketClient, &WebSocketClient::remoteSessionRecoveryExpired,
            this, [this](const QString& sessionId, quint64 generation) {
        const auto binding = m_webSocketClient->remoteSessionCoordinator()->byId(sessionId);
        if (binding.remoteSessionId.isEmpty() || binding.generation != generation) return;
        if (m_sceneActivityModel) m_sceneActivityModel->removeForSession(sessionId);
        if (binding.targetEndpointId == m_webSocketClient->endpointId()) {
            m_terminalIncomingSessionFilter.insert(sessionId);
            beginTerminalIncomingCacheCleanup(QStringLiteral("session_recovery_expired"));
        } else if (binding.ownerEndpointId == m_webSocketClient->endpointId()) {
            if (!matchesCurrentOutgoingSession(binding.targetEndpointId, sessionId, generation)) return;
            terminateProjectRemoteSession(binding.targetEndpointId, true,
                                          QStringLiteral("session_recovery_expired"));
        }
    });
    connect(m_webSocketClient, &WebSocketClient::remoteSessionAbsent,
            this, [this](const QString& sessionId, quint64 generation) {
        const auto binding = m_webSocketClient->remoteSessionCoordinator()->byId(sessionId);
        if (binding.remoteSessionId.isEmpty() || binding.generation != generation) return;
        if (binding.targetEndpointId == m_webSocketClient->endpointId()) {
            m_terminalIncomingSessionFilter.insert(sessionId);
            beginTerminalIncomingCacheCleanup(QStringLiteral("session_authoritatively_absent"));
        } else if (binding.ownerEndpointId == m_webSocketClient->endpointId()) {
            const QString target = binding.targetEndpointId;
            if (!matchesCurrentOutgoingSession(target, sessionId, generation)) return;
            if (!finalizePendingOutgoingSessionClose(target, sessionId, generation, false))
                clearRemoteSessionRuntimeState(target, false, sessionId);
            QTimer::singleShot(0, this, &ApplicationRuntime::reconcileForegroundRemoteSession);
        }
    });
    connect(m_webSocketClient, &WebSocketClient::remoteSessionLogicallyClosed,
            this, [this](const QJsonObject& envelope) {
        // Logical closure does not erase renderer/cache transactions or ACKs.
        if (envelope.value(QStringLiteral("ownerEndpointId")).toString()
            == m_webSocketClient->endpointId()) {
            clearRemoteSessionRuntimeState(envelope.value(QStringLiteral("targetEndpointId")).toString(), true,
                envelope.value(QStringLiteral("remoteSessionId")).toString());
        }
    });
    connect(m_webSocketClient, &WebSocketClient::registrationConfirmed, this, &ApplicationRuntime::onRegistrationConfirmed);
    // UploadManager subscribes to the canonical upload envelope.
    m_uploadManager->setWebSocketClient(m_webSocketClient);
    m_connectionManager->setReceiverReady(m_uploadManager->receiverReadyForAdvertisement());
    connect(m_uploadManager, &UploadManager::terminalIncomingCleanupRequired,
            this, &ApplicationRuntime::beginTerminalIncomingCacheCleanup);
    connect(m_uploadManager,
            &UploadManager::receiverAdvertisementReadinessChanged,
            this, [this](bool ready, const QString&) {
        // A welcome may have completed while renderer destruction was still
        // pending. Publish the device only after logical quarantine commits.
        if (m_connectionManager) m_connectionManager->setReceiverReady(ready);
        if (!ready && m_webSocketClient) m_webSocketClient->invalidateLocalDeviceSnapshot();
        if (ready && m_webSocketClient && m_webSocketClient->isConnected()) {
            syncRegistration();
        }
    });

    connect(m_uploadManager, &UploadManager::assetRemovalCommitted,
            this, [this](const QString& targetEndpointId,
                         const QStringList& localFileIds) {
        ClientWorkspace* session = m_workspaceManager
            ? m_workspaceManager->findWorkspace(targetEndpointId) : nullptr;
        if (!session) return;
        for (const QString& fileId : localFileIds) {
            session->knownRemoteFileIds.remove(fileId);
            session->expectedProjectFileIds.remove(fileId);
            if (!session->canvas) continue;
            for (CanvasMedia* media : session->canvas->enumerateMediaItems()) {
                if (media && media->fileId() == fileId) {
                    media->setUploadNotUploaded();
                }
            }
        }
        session->upload.remoteFilesPresent =
            !session->knownRemoteFileIds.isEmpty();
    });
    connect(m_uploadManager, &UploadManager::assetRemovalFailed,
            this, [this](const QString& targetEndpointId,
                         const QString& remoteSessionId,
                         const QStringList& localFileIds,
                         const QString& reason) {
        Q_UNUSED(localFileIds);
        TOAST_ERROR(QStringLiteral(
            "Remote asset cleanup failed: %1")
                        .arg(reason),
                    AppConfig::instance().toastErrorDurationMs());
        // Project deletion is local and terminal even if the background
        // physical cleanup needs a later retry. It must never close an
        // otherwise healthy session-only workspace.
        if (!m_projectManager
            || !m_projectManager->hasProjectForTarget(targetEndpointId)) {
            return;
        }
        RemoteSessionCoordinator* coordinator = m_webSocketClient
            ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
        const RemoteSessionCoordinator::Binding binding = coordinator
            ? coordinator->byId(remoteSessionId)
            : RemoteSessionCoordinator::Binding();
        if (!binding.remoteSessionId.isEmpty()
            && binding.ownerEndpointId == m_webSocketClient->endpointId()) {
            terminateProjectRemoteSession(targetEndpointId, true);
        } else {
            clearRemoteSessionRuntimeState(targetEndpointId);
        }
    });
    
    // A file is invalidated only after its last local media reference is gone.
    // Remote copies are addressed through the exact authenticated session
    // inventory; no target name or historical canvas identifier is accepted.
    FileManager::setFileRemovalNotifier([uploads = QPointer<UploadManager>(m_uploadManager)](
        const QString& fileId, const QList<QString>& clientIds, const QList<QString>& projectIds) {
        Q_UNUSED(projectIds);
        if (!uploads) return;
        QSet<QString> uniqueTargets(clientIds.cbegin(), clientIds.cend());
        for (const QString& targetEndpointId : uniqueTargets) {
            uploads->requestAssetRemoval(
                targetEndpointId, fileId, QStringLiteral("source_removed"));
        }
    });
    
    // FileWatcher: remove media items when their source files are deleted
    connect(m_fileWatcher, &FileWatcher::filesDeleted, this,
            [this](const QList<CanvasMedia*>& mediaItems) {
        removeInvalidMediaItems(mediaItems);
    });
    
    connect(m_uploadManager, &UploadManager::uploadProgress, this, [this](int percent, int filesCompleted, int totalFiles){
        updateIndividualProgressFromServer(percent, filesCompleted, totalFiles);
    });
    // Initialize toast notification system
    m_toastSystem = new ToastNotificationSystem(this);
    ToastNotificationSystem::setInstance(m_toastSystem);
    updateHistoryVisibilityState();

    if (m_projectManager && !m_projectManager->load()) {
        TOAST_ERROR(QStringLiteral("Projects could not be loaded: %1")
                        .arg(m_projectManager->lastError()),
                    AppConfig::instance().toastErrorDurationMs());
    } else {
        QStringList projectIds;
        for (const auto& project : m_projectManager->projects()) projectIds.append(project.projectId);
        m_screenPreviewStore->pruneProjects(projectIds);
        validateAllProjectSources();
        refreshProjectClientList();
    }

    connectToServer();
}


void ApplicationRuntime::handleTerminalTransportLoss(
    const QString& reason, const QString& serverBootId, quint64 generation, bool notify)
{
    if (m_incomingSessionOrphanWatchdog) {
        m_incomingSessionOrphanWatchdog->cancelAll();
        m_incomingOrphanSessionIds.clear();
    }
    if (m_cleanShutdownPrepared) {
        // Requested shutdown invalidates in-memory sessions as cleanup;
        // it must not create a misleading outage notification.
        if (m_sceneActivityModel) m_sceneActivityModel->clear();
        return;
    }
    // This signal is delivered synchronously from WebSocketClient before
    // SceneRunCoordinator::clearSessions(). Capture every incoming binding
    // now; a queued cleanup would lose the immutable cache correlation.
    beginTerminalIncomingCacheCleanup(reason);
    if (notify && m_toastSystem) {
        NotificationRequest notification;
        notification.severity = NotificationSeverity::Warning;
        notification.category = QStringLiteral("Remote session");
        notification.message = QStringLiteral(
            "The recovery deadline expired. Remote scenes were stopped and session uploads were invalidated.");
        notification.correlationId = QStringLiteral("lease-expired:%1:%2")
                                         .arg(serverBootId)
                                         .arg(generation);
        notification.terminal = true;
        m_toastSystem->publishNotification(notification);
    }
    if (m_sceneActivityModel) m_sceneActivityModel->clear();
    if (m_workspaceManager) {
        const QList<QString> targets =
            m_workspaceManager->allTargetEndpointIds();
        for (const QString& target : targets) {
            m_remoteSessionOpenSuppressedTargets.insert(target);
            if (ClientWorkspace* session =
                    m_workspaceManager->findWorkspace(target)) {
                terminateProjectRemoteSession(target, false);
            }
        }
        markAllWorkspacesDisconnected();
    }
    m_remoteSessionOpenPendingTargets.clear();
    m_remoteSessionOpenTargetByRequestId.clear();
    m_automaticRemoteSessionOpenRequestIds.clear();
    // Keep pending outgoing close intents across a same-boot transport
    // outage. The server will replay/resume that exact session and the
    // close is retried before any replacement OPEN is allowed.
    for (auto it = m_pendingOutgoingSessionCloses.begin();
         it != m_pendingOutgoingSessionCloses.end(); ++it) {
        it->closeDispatchedOnConnectionGeneration = 0;
    }
    m_locallyTerminatingRemoteSessions.clear();
    m_pendingTeardownAcks.clear();
    m_discoveredClients.clear();
    refreshProjectClientList();
}

void ApplicationRuntime::setLocalNetworkStatus(const QString& status) {
    const QString normalized = status.trimmed().toUpper();
    if (m_localStatusText == normalized) return;
    m_localStatusText = normalized;
    emit presentationStateChanged();
}

ApplicationRuntime::ClientWorkspace* ApplicationRuntime::findWorkspace(
    const QString& targetEndpointId) {
    return m_workspaceController
        ? m_workspaceController->findWorkspace(targetEndpointId) : nullptr;
}

const ApplicationRuntime::ClientWorkspace* ApplicationRuntime::findWorkspace(
    const QString& targetEndpointId) const {
    return m_workspaceController
        ? m_workspaceController->findWorkspace(targetEndpointId) : nullptr;
}

ApplicationRuntime::ClientWorkspace* ApplicationRuntime::ensureWorkspace(
    const ClientInfo& client) {
    return m_workspaceController ? m_workspaceController->ensureWorkspace(client)
                                 : nullptr;
}

void ApplicationRuntime::configureWorkspace(ClientWorkspace& workspace) {
    if (m_workspaceController) {
        m_workspaceController->configureWorkspace(&workspace);
    }
}

void ApplicationRuntime::switchToWorkspace(const QString& targetEndpointId) {
    if (m_workspaceController) {
        m_workspaceController->switchToWorkspace(targetEndpointId);
    }
}

void ApplicationRuntime::updateUploadButtonForWorkspace(ClientWorkspace& workspace) {
    if (m_workspaceController) {
        m_workspaceController->updateUploadButtonForWorkspace(&workspace);
    }
}

bool ApplicationRuntime::getAutoUploadImportedMedia() const {
    return m_settingsManager ? m_settingsManager->getAutoUploadImportedMedia() : false;
}


void ApplicationRuntime::reconcileRemoteFilesForWorkspace(
    ClientWorkspace& workspace,
    const QSet<QString>& currentFileIds) {
    if (workspace.expectedProjectFileIds != currentFileIds) {
        workspace.expectedProjectFileIds = currentFileIds;
        m_fileManager->replaceProjectFileSet(workspace.projectId, currentFileIds);
    }

    if (!workspace.targetEndpointId.isEmpty()) {
        QSet<QString> targetSources = workspace.knownRemoteFileIds;
        targetSources.unite(workspace.upload.fileIds);
        const QSet<QString> toRemove = targetSources - currentFileIds;
        for (const QString& fileId : toRemove) {
            if (m_uploadManager) {
                m_uploadManager->requestAssetRemoval(
                    workspace.targetEndpointId, fileId,
                    QStringLiteral("no_longer_referenced"));
            }
        }
    }
}

void ApplicationRuntime::connectUploadSignals() {
    if (m_uploadSignalConnector) {
        m_uploadSignalConnector->connectAllSignals(this, m_uploadManager, m_webSocketClient, m_uploadSignalsConnected);
    }
}

void ApplicationRuntime::setUploadWorkspaceByUploadId(
    const QString& uploadId, const QString& workspaceEndpointId) {
    if (uploadId.isEmpty() || workspaceEndpointId.isEmpty()) return;
    m_uploadWorkspaceByUploadId.insert(uploadId, workspaceEndpointId);
}

void ApplicationRuntime::markAllWorkspacesDisconnected() {
    for (ClientWorkspace* session : m_workspaceManager->allWorkspaces()) {
        session->lastClientInfo.setEndpointId(session->targetEndpointId);
        session->lastClientInfo.setFromMemory(true);
        session->lastClientInfo.setOnline(false);
    }
}

QList<ClientInfo> ApplicationRuntime::buildDisplayClientList(const QList<ClientInfo>& connectedClients) {
    for (const ClientInfo& incoming : connectedClients) {
        bool previouslyKnown = false;
        for (const ClientInfo& previous : std::as_const(m_discoveredClients)) {
            if (incoming.endpointId() == previous.endpointId()) previouslyKnown = true;
            if (incoming.endpointId() == previous.endpointId()
                && (incoming.runtimeId() != previous.runtimeId()
                    || incoming.isOnline() != previous.isOnline()
                    || incoming.canAcceptSession() != previous.canAcceptSession())) {
                m_sessionRecovery.expedite(incoming.endpointId());
                m_remoteSessionAutoOpenBlockedTargets.remove(incoming.endpointId());
            }
        }
        if (!previouslyKnown && incoming.canAcceptSession()) {
            m_sessionRecovery.expedite(incoming.endpointId());
            m_remoteSessionAutoOpenBlockedTargets.remove(incoming.endpointId());
        }
    }
    m_discoveredClients = connectedClients;
    const bool localDiscoveryUsable = m_connectionManager
        && m_connectionManager->state() == ConnectionManager::State::Connected;
    if (!m_projectManager) {
        m_displayClients = ClientListBuilder::buildDisplayClientList(
            this, connectedClients, localDiscoveryUsable);
        for (ClientInfo& client : m_displayClients) client = m_profileCache->apply(client);
        emit displayClientsChanged(m_displayClients);
        refreshRemoteConnectionPresentation(false);
        return m_displayClients;
    }

    const QList<ProjectClientEntry> entries =
        m_projectManager->mergeDiscoveredClients(connectedClients, -1, localDiscoveryUsable);
    QList<ClientInfo> result;
    result.reserve(entries.size());
    for (const ProjectClientEntry& entry : entries) {
        ClientInfo client = m_profileCache->apply(entry.client);
        client.setEndpointId(entry.endpointId);
        client.setOnline(entry.online);
        client.setFromMemory(entry.hasProject);
        client.setProjectId(entry.projectId);
        client.setHasProject(entry.hasProject);
        client.setRemoteSessionCloseAtMs(m_workspaceManager
            ? m_workspaceManager->remoteSessionCloseAtMs(entry.endpointId) : -1);
        client.setProjectDeleteAtMs(entry.projectDeleteAtMs);
        client.setProjectMediaReleaseAtMs(
            m_projectManager->projectMediaReleaseAtMs(entry.endpointId));
        const ClientInfo* presence = nullptr;
        for (const ClientInfo& discovered : m_discoveredClients) {
            if (discovered.endpointId() == entry.endpointId) {
                presence = &discovered;
                break;
            }
        }
        const QString status = remoteConnectionStatus(entry.endpointId, presence,
                                                       localDiscoveryUsable);
        client.setStatus(status);
        client.setAvailabilityStatus(status);

        if (ClientWorkspace* session = m_workspaceManager->findWorkspace(entry.endpointId)) {
            session->lastClientInfo = client;
            session->remoteContentClearedOnDisconnect = !entry.online
                ? session->remoteContentClearedOnDisconnect : false;
            if (session->canvas) {
                session->canvas->setRemoteSceneTarget(
                    entry.endpointId, client.getInstanceDisplayName());
                session->canvas->updateRemoteSceneTargetFromClientList({client});
            }
        }
        result.append(client);
    }
    m_displayClients = result;
    for (const ClientInfo& client : result) {
        if (client.endpointId() == m_activeWorkspaceEndpointId) {
            m_selectedClient = client;
            m_remoteDisplayName = client.getInstanceDisplayName();
            break;
        }
    }
    emit displayClientsChanged(m_displayClients);
    refreshRemoteConnectionPresentation(false);
    return m_displayClients;
}

void ApplicationRuntime::refreshProjectClientList() {
    buildDisplayClientList(m_discoveredClients);
}

QList<ProjectMediaReference> ApplicationRuntime::collectProjectMediaReferences(
    const QString& targetEndpointId, ICanvasHost* canvas) const {
    QList<ProjectMediaReference> references;
    if (!canvas) {
        return references;
    }

    QHash<QString, ProjectMediaReference> previousByMediaId;
    if (m_projectManager) {
        if (const ProjectRecord* project =
                m_projectManager->projectForTarget(targetEndpointId)) {
            for (const ProjectMediaReference& previous : project->mediaReferences) {
                previousByMediaId.insert(previous.mediaId, previous);
            }
        }
    }

    for (CanvasMedia* media : canvas->enumerateMediaItems()) {
        if (!media || media->isText() || media->sourcePath().isEmpty()) {
            continue;
        }
        const QString canonicalPath = canonicalExistingPath(media->sourcePath());
        if (canonicalPath.isEmpty()) {
            continue;
        }
        const QString identity = sourceIdentityForPath(canonicalPath);
        ProjectMediaReference reference = previousByMediaId.value(media->mediaId());
        reference.mediaId = media->mediaId();
        reference.canonicalSourcePath = canonicalPath;
        reference.sourceIdentity = identity;
        // Canvas import owns asynchronous content verification. Autosave must
        // never reread a whole movie on the GUI thread.
        reference.sha256 = media->fileId();
        reference.assetId = reference.sha256;
        reference.pendingImport = reference.sha256.isEmpty();
        reference.mediaType = media->isVideo()
            ? QStringLiteral("video") : QStringLiteral("image");
        references.append(reference);
    }
    return references;
}

void ApplicationRuntime::persistProjectCanvas(const QString& targetEndpointId) {
    if (!m_projectManager || targetEndpointId.isEmpty()) {
        return;
    }
    ClientWorkspace* session = m_workspaceManager->findWorkspace(targetEndpointId);
    if (!session || !session->canvas
        || !m_projectManager->hasProjectForTarget(targetEndpointId)) {
        return;
    }
    m_projectManager->updateCanvasState(
        targetEndpointId,
        session->canvas->serializeProjectState(),
        collectProjectMediaReferences(targetEndpointId, session->canvas),
        session->canvas->document()
            ? session->canvas->document()->screens() : QList<ScreenInfo>());
}

void ApplicationRuntime::reconcileProjectMediaResidency(const QString& targetEndpointId)
{
    ClientWorkspace* workspace = findWorkspace(targetEndpointId);
    if (!workspace || !workspace->canvas || !m_projectManager) return;
    CanvasDocument* document = workspace->canvas->document();
    if (!document) return;

    const bool expired = m_projectManager->projectMediaReleaseExpired(targetEndpointId);
    // PREPARE and playback own scene leases. Keep them intact until the host
    // restores its draft and unlocks the document, then apply the expired
    // deadline immediately. Returning to the application cancels the request.
    if (expired && document->editsLocked()) return;
    const bool wasSuspended = document->mediaResidencySuspended();
    document->setMediaResidencySuspended(expired);
    if (expired) {
        m_screenPreviewStore->flush();
        workspace->canvas->clearRemoteScreenFrames(); // Unload RAM; the durable image remains.
    } else if (wasSuspended) {
        restoreProjectScreenPreviews(targetEndpointId);
    }
}

void ApplicationRuntime::restoreProjectScreenPreviews(const QString& targetEndpointId)
{
    if (!m_screenPreviewStore || !m_settingsManager->getScreenContentVisible()) return;
    const auto* project = m_projectManager->projectForTarget(targetEndpointId);
    auto* canvas = canvasForEndpointId(targetEndpointId);
    if (!project || !canvas || !canvas->document()
        || canvas->document()->mediaResidencySuspended()) return;
    QList<int> missingScreens;
    for (const auto& screen : canvas->document()->screens())
        if (!canvas->hasRemoteScreenFrame(screen.id)) missingScreens.append(screen.id);
    if (!missingScreens.isEmpty()) m_screenPreviewStore->restore(project->projectId, missingScreens);
}

void ApplicationRuntime::restoreProjectCanvas(ClientWorkspace& session) {
    if (!m_projectManager || !session.canvas
        || m_restoredProjectIds.contains(session.targetEndpointId)) {
        return;
    }
    const ProjectRecord* stored =
        m_projectManager->projectForTarget(session.targetEndpointId);
    if (!stored) {
        return;
    }

    QHash<QString, QString> validSources;
    QList<ProjectMediaReference> validReferences;
    QSet<QString> invalidMediaIds;
    for (const ProjectMediaReference& reference : stored->mediaReferences) {
        const QString canonicalPath = canonicalExistingPath(reference.canonicalSourcePath);
        const QString identity = sourceIdentityForPath(canonicalPath);
        if (canonicalPath.isEmpty()
            || canonicalPath != reference.canonicalSourcePath
            || identity.isEmpty()
            || identity != reference.sourceIdentity) {
            invalidMediaIds.insert(reference.mediaId);
            continue;
        }
        validSources.insert(reference.mediaId, canonicalPath);
        validReferences.append(reference);
    }

    QJsonObject durableState = stored->canvasStateForRestore();
    QJsonArray filteredMedia;
    for (const QJsonValue& value : durableState.value(QStringLiteral("media")).toArray()) {
        const QJsonObject media = value.toObject();
        const QString type = media.value(QStringLiteral("type")).toString().toLower();
        const QString mediaId = media.value(QStringLiteral("mediaId")).toString();
        if (type == QLatin1String("text") || validSources.contains(mediaId)) {
            filteredMedia.append(media);
        } else {
            invalidMediaIds.insert(mediaId);
        }
    }
    durableState.insert(QStringLiteral("media"), filteredMedia);

    reconcileProjectMediaResidency(session.targetEndpointId);
    QStringList skipped;
    if (!session.canvas->restoreProjectState(durableState, validSources, &skipped)) {
        qWarning() << "Project canvas restoration failed for device"
                   << session.targetEndpointId.left(12);
        return;
    }
    for (const QString& mediaId : skipped) {
        invalidMediaIds.insert(mediaId);
    }
    m_restoredProjectIds.insert(session.targetEndpointId);
    restoreProjectScreenPreviews(session.targetEndpointId);

    if (!invalidMediaIds.isEmpty() || !skipped.isEmpty()) {
        QJsonArray finalMedia;
        for (const QJsonValue& value : filteredMedia) {
            const QString mediaId = value.toObject()
                                        .value(QStringLiteral("mediaId")).toString();
            if (!invalidMediaIds.contains(mediaId)) {
                finalMedia.append(value);
            }
        }
        durableState.insert(QStringLiteral("media"), finalMedia);
        QJsonArray finalPendingImports;
        for (const QJsonValue& value : durableState.value(QStringLiteral("pendingImports")).toArray()) {
            if (!invalidMediaIds.contains(value.toObject().value(QStringLiteral("mediaId")).toString()))
                finalPendingImports.append(value);
        }
        if (finalPendingImports.isEmpty()) durableState.remove(QStringLiteral("pendingImports"));
        else durableState.insert(QStringLiteral("pendingImports"), finalPendingImports);
        m_projectManager->updateCanvasState(
            session.targetEndpointId, durableState, validReferences,
            stored->savedScreens);
        TOAST_WARNING(QStringLiteral("%1 missing or modified media item%2 removed from the project")
                          .arg(invalidMediaIds.size())
                          .arg(invalidMediaIds.size() == 1 ? QString() : QStringLiteral("s")),
                      5000);
    }
}

void ApplicationRuntime::removeInvalidMediaItems(
    const QList<CanvasMedia*>& mediaItems) {
    if (!m_workspaceManager || mediaItems.isEmpty()) {
        return;
    }

    QSet<CanvasMedia*> requested;
    QSet<QString> affectedCanonicalPaths;
    QSet<QString> affectedMediaIds;
    for (CanvasMedia* media : mediaItems) {
        if (!media || media->mediaId().isEmpty()
            || media->mediaId().contains(QChar::Null)) {
            continue;
        }
        requested.insert(media);
        affectedMediaIds.insert(media->mediaId());
        QString sourcePath = m_fileWatcher
            ? m_fileWatcher->watchedFilePath(media) : QString();
        if (sourcePath.isEmpty()) {
            const QFileInfo info(media->sourcePath());
            sourcePath = info.canonicalFilePath();
            if (sourcePath.isEmpty()) {
                sourcePath = QDir::cleanPath(info.absoluteFilePath());
            }
        }
        if (!sourcePath.isEmpty()) {
            affectedCanonicalPaths.insert(sourcePath);
        }
    }
    if (requested.isEmpty()) {
        return;
    }

    QHash<QString, QList<CanvasMedia*>> itemsByProject;
    for (ClientWorkspace* session : m_workspaceManager->allWorkspaces()) {
        if (!session || !session->canvas) {
            continue;
        }
        for (CanvasMedia* candidate : session->canvas->enumerateMediaItems()) {
            if (!candidate || candidate->isText()) {
                continue;
            }
            QString candidatePath = m_fileWatcher
                ? m_fileWatcher->watchedFilePath(candidate) : QString();
            if (candidatePath.isEmpty()) {
                const QFileInfo candidateInfo(candidate->sourcePath());
                candidatePath = candidateInfo.canonicalFilePath();
                if (candidatePath.isEmpty()) {
                    candidatePath = QDir::cleanPath(candidateInfo.absoluteFilePath());
                }
            }
            if (requested.contains(candidate)
                || affectedCanonicalPaths.contains(candidatePath)) {
                itemsByProject[session->targetEndpointId].append(candidate);
            }
        }
    }

    // Remote graphs are immutable, so stop them before removing a source.
    // Local preview can retire only the invalid occurrences and keep its clock.
    for (auto it = itemsByProject.cbegin(); it != itemsByProject.cend(); ++it) {
        if (ClientWorkspace* session = m_workspaceManager->findWorkspace(it.key());
            session && session->canvas && !session->canvas->testSceneLaunched()) {
            session->canvas->stopScenesForSourceInvalidation();
        }
    }

    int removedCount = 0;
    for (auto it = itemsByProject.cbegin(); it != itemsByProject.cend(); ++it) {
        ClientWorkspace* session = m_workspaceManager->findWorkspace(it.key());
        if (!session || !session->canvas) {
            continue;
        }
        for (CanvasMedia* media : it.value()) {
            if (m_fileWatcher) {
                m_fileWatcher->unwatchMediaItem(media);
            }
            session->canvas->deleteMediaItemCanonical(media);
            ++removedCount;
        }
        persistProjectCanvas(it.key());
    }

    // A source can occur in projects whose canvas has not been instantiated
    // in this process. Remove those durable occurrences as part of the same
    // canonical operation; no placeholder survives.
    if (m_projectManager) {
        const QList<ProjectRecord> projects = m_projectManager->projects();
        for (const ProjectRecord& project : projects) {
            QSet<QString> removeIds;
            QList<ProjectMediaReference> retainedReferences;
            for (const ProjectMediaReference& reference : project.mediaReferences) {
                if (affectedMediaIds.contains(reference.mediaId)
                    || affectedCanonicalPaths.contains(reference.canonicalSourcePath)) {
                    removeIds.insert(reference.mediaId);
                } else {
                    retainedReferences.append(reference);
                }
            }
            if (removeIds.isEmpty()) {
                continue;
            }
            QJsonObject state = project.canvasState;
            QJsonArray retainedMedia;
            for (const QJsonValue& value : state.value(QStringLiteral("media")).toArray()) {
                if (!removeIds.contains(
                        value.toObject().value(QStringLiteral("mediaId")).toString())) {
                    retainedMedia.append(value);
                }
            }
            state.insert(QStringLiteral("media"), retainedMedia);
            m_projectManager->updateCanvasState(
                project.targetEndpointId, state, retainedReferences,
                project.savedScreens);
        }
        m_projectManager->flush();
    }

    if (removedCount > 0) {
        TOAST_WARNING(
            QStringLiteral("%1 unavailable media item%2 removed from all projects")
                .arg(removedCount)
                .arg(removedCount == 1 ? QString() : QStringLiteral("s")),
            5000);
    }
}

void ApplicationRuntime::validateAllProjectSources() {
    if (!m_projectManager) {
        return;
    }

    int removedCount = 0;
    bool changed = false;
    const QList<ProjectRecord> projects = m_projectManager->projects();
    for (const ProjectRecord& project : projects) {
        QSet<QString> validMediaIds;
        QSet<QString> invalidMediaIds;
        QList<ProjectMediaReference> retainedReferences;
        for (const ProjectMediaReference& reference : project.mediaReferences) {
            const QString canonicalPath =
                canonicalExistingPath(reference.canonicalSourcePath);
            const bool valid = !canonicalPath.isEmpty()
                && canonicalPath == reference.canonicalSourcePath
                && sourceIdentityForPath(canonicalPath) == reference.sourceIdentity;
            if (valid) {
                validMediaIds.insert(reference.mediaId);
                retainedReferences.append(reference);
            } else {
                invalidMediaIds.insert(reference.mediaId);
            }
        }

        QJsonObject state = project.canvasState;
        QJsonArray retainedMedia;
        for (const QJsonValue& value : state.value(QStringLiteral("media")).toArray()) {
            const QJsonObject media = value.toObject();
            const QString type = media.value(QStringLiteral("type")).toString().toLower();
            const QString mediaId = media.value(QStringLiteral("mediaId")).toString();
            if (type == QLatin1String("text") || validMediaIds.contains(mediaId)) {
                retainedMedia.append(media);
            } else {
                invalidMediaIds.insert(mediaId);
            }
        }

        if (invalidMediaIds.isEmpty()) {
            continue;
        }
        removedCount += invalidMediaIds.size();
        state.insert(QStringLiteral("media"), retainedMedia);
        m_projectManager->updateCanvasState(
            project.targetEndpointId, state, retainedReferences,
            project.savedScreens);
        changed = true;
    }

    if (changed) {
        m_projectManager->flush();
        TOAST_WARNING(
            QStringLiteral("%1 missing or modified media item%2 removed while restoring projects")
                .arg(removedCount)
                .arg(removedCount == 1 ? QString() : QStringLiteral("s")),
            5000);
    }
}

ICanvasHost* ApplicationRuntime::canvasForEndpointId(
    const QString& targetEndpointId) const {
    if (targetEndpointId.isEmpty()) {
        const ClientWorkspace* active = findWorkspace(m_activeWorkspaceEndpointId);
        return active ? active->canvas : nullptr;
    }
    const ClientWorkspace* workspace = findWorkspace(targetEndpointId);
    return workspace ? workspace->canvas : nullptr;
}


ApplicationRuntime::ClientWorkspace* ApplicationRuntime::workspaceForActiveUpload() {
    return m_workspaceController
        ? m_workspaceController->workspaceForActiveUpload() : nullptr;
}

ApplicationRuntime::ClientWorkspace* ApplicationRuntime::workspaceForUploadId(const QString& uploadId) {
    return m_workspaceController
        ? m_workspaceController->workspaceForUploadId(uploadId) : nullptr;
}

void ApplicationRuntime::clearUploadTracking(ClientWorkspace& workspace) {
    if (m_workspaceController) {
        m_workspaceController->clearUploadTracking(&workspace);
    }
}


void ApplicationRuntime::updateApplicationSuspendedState(bool suspended) {
    setApplicationSuspended(suspended);
}



void ApplicationRuntime::handleApplicationStateChanged(Qt::ApplicationState state) {
    setApplicationSuspended(state == Qt::ApplicationHidden
                            || state == Qt::ApplicationSuspended
                            || m_nativeSystemSuspended);
    refreshMediaSharing();
}

void ApplicationRuntime::handleNativeSystemSuspendedChanged(bool suspended) {
    m_nativeSystemSuspended = suspended;
    if (!suspended && m_webSocketClient && m_webSocketClient->isConnected()) syncRegistration();
    setApplicationSuspended(suspended
                            || QGuiApplication::applicationState() == Qt::ApplicationHidden
                            || QGuiApplication::applicationState() == Qt::ApplicationSuspended);
    refreshMediaSharing();
}

void ApplicationRuntime::showScreenView(const ClientInfo& client) {
    if (!m_navigationManager) return;
    ClientInfo selectedClient = client;
    const QString targetEndpointId = selectedClient.endpointId().trimmed();
    if (targetEndpointId.isEmpty()) {
        TOAST_ERROR(QStringLiteral("This client has no authenticated device identity"),
                    AppConfig::instance().toastErrorDurationMs());
        return;
    }
    if (selectedClient.getMachineName().trimmed().isEmpty()) {
        for (const ClientInfo& discovered : m_discoveredClients) {
            if (discovered.endpointId() == targetEndpointId
                && !discovered.getMachineName().trimmed().isEmpty()) {
                selectedClient.setMachineName(discovered.getMachineName());
                selectedClient.setPlatform(discovered.getPlatform());
                selectedClient.setInstallationId(discovered.installationId());
                selectedClient.setInstanceId(discovered.instanceId());
                selectedClient.setInstanceOrdinal(discovered.instanceOrdinal());
                selectedClient.setRuntimeId(discovered.runtimeId());
                break;
            }
        }
    }
    if (selectedClient.getMachineName().trimmed().isEmpty()) {
        TOAST_ERROR(QStringLiteral("This client's identity is incomplete. Refresh the client list and try again."),
                    AppConfig::instance().toastErrorDurationMs());
        return;
    }

    if (!m_activeWorkspaceEndpointId.isEmpty()
        && m_activeWorkspaceEndpointId != targetEndpointId
        && m_navigationManager->isOnScreenView()) {
        persistProjectCanvas(m_activeWorkspaceEndpointId);
        m_remoteSessionOpenDesiredTargets.remove(m_activeWorkspaceEndpointId);
    }

    // Selection can establish the first session before a Project exists.
    // Preserve it across cleanup of a previous session; subsequent automatic
    // recovery derives its intent from activity on this Canvas.
    m_selectionEndpointId = targetEndpointId;
    m_selectionClient = selectedClient;
    m_sessionRecovery.reset(targetEndpointId);
    m_remoteSessionAutoOpenBlockedTargets.remove(targetEndpointId);
    m_remoteSessionOpenDesiredTargets.insert(targetEndpointId);

    ClientWorkspace* workspacePointer =
        m_workspaceManager->getOrCreateWorkspace(targetEndpointId, selectedClient);
    if (!workspacePointer) return;
    ClientWorkspace& workspace = *workspacePointer;
    workspace.lastClientInfo = selectedClient;
    workspace.lastClientInfo.setEndpointId(targetEndpointId);
    workspace.workspaceVisible = true;
    workspace.sessionHiddenAtMs = -1;
    m_activeWorkspaceEndpointId = targetEndpointId;
    m_selectedClient = workspace.lastClientInfo;

    bool hasProject = m_projectManager
        && m_projectManager->hasProjectForTarget(targetEndpointId);
    if (hasProject) {
        m_projectManager->updateTargetReference(
            ProjectTargetReference::fromClientInfo(selectedClient));
        if (const ProjectRecord* project =
                m_projectManager->projectForTarget(targetEndpointId)) {
            m_workspaceManager->updateWorkspaceProjectId(targetEndpointId,
                                                  project->projectId);
        }
    }

    RemoteSessionCoordinator* remoteSessionCoordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    const RemoteSessionCoordinator::Binding retainedBinding = remoteSessionCoordinator
        ? remoteSessionCoordinator->outgoingForPeer(targetEndpointId)
        : RemoteSessionCoordinator::Binding();
    const bool terminalBinding = retainedBinding.phase == QLatin1String("Terminating")
        || retainedBinding.phase == QLatin1String("CleanupPending")
        || retainedBinding.phase == QLatin1String("Closed");
    if (terminalBinding && !retainedBinding.remoteSessionId.isEmpty()
        && retainedBinding.ownerEndpointId == m_webSocketClient->endpointId()) {
        rememberPendingOutgoingSessionClose(
            targetEndpointId, retainedBinding.remoteSessionId,
            retainedBinding.generation);
        m_locallyTerminatingRemoteSessions.insert(
            retainedBinding.remoteSessionId);
    }
    const bool closePending = hasPendingOutgoingSessionClose(
        targetEndpointId)
        || hasCancelledInitialOpenForTarget(targetEndpointId)
        || (!retainedBinding.remoteSessionId.isEmpty()
            && m_locallyTerminatingRemoteSessions.contains(
                retainedBinding.remoteSessionId));
    const bool active = !closePending
        && isCommandReadyBinding(m_webSocketClient, retainedBinding);
    const bool grace = !closePending && !active
        && (retainedBinding.phase == QLatin1String("Active")
            || retainedBinding.phase == QLatin1String("Grace"));
    if (active || grace) {
        m_currentOutgoingSessionIdByTarget.insert(
            targetEndpointId, retainedBinding.remoteSessionId);
        m_currentOutgoingSessionGenerationByTarget.insert(
            targetEndpointId, retainedBinding.generation);
    }
    if (active) {
        m_workspaceManager->setRemoteSessionState(
            targetEndpointId, WorkspaceManager::RemoteSessionState::Active);
        m_remoteSessionOpenDesiredTargets.remove(targetEndpointId);
    } else if (grace) {
        m_workspaceManager->setRemoteSessionState(
            targetEndpointId, WorkspaceManager::RemoteSessionState::Grace);
        m_remoteSessionOpenDesiredTargets.remove(targetEndpointId);
    } else if (closePending) {
        m_workspaceManager->setRemoteSessionState(
            targetEndpointId, WorkspaceManager::RemoteSessionState::Closing);
    } else if (!m_remoteSessionOpenPendingTargets.contains(targetEndpointId)) {
        m_workspaceManager->setRemoteSessionState(
            targetEndpointId, WorkspaceManager::RemoteSessionState::Absent);
    }

    const bool hasRemoteSession = active || grace
        || m_remoteSessionOpenPendingTargets.contains(targetEndpointId);
    if ((hasProject || hasRemoteSession) && !workspace.canvas) {
        ensureWorkspace(workspace.lastClientInfo);
    }
    ClientWorkspace* currentWorkspace = m_workspaceManager->findWorkspace(targetEndpointId);
    if (!currentWorkspace) return;
    if (hasProject && currentWorkspace->canvas) restoreProjectCanvas(*currentWorkspace);
    if (hasRemoteSession && currentWorkspace->canvas) {
        currentWorkspace->canvas->setScreens(
            currentWorkspace->lastClientInfo.getScreens());
    }
    updateWorkspaceCapabilities(targetEndpointId);

    if (currentWorkspace->canvas) switchToWorkspace(targetEndpointId);
    else {
        m_activeCanvas = nullptr;
        if (m_navigationManager) m_navigationManager->setActiveCanvas(nullptr);
        if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
    }

    m_canvasRevealedForCurrentClient = currentWorkspace->canvas != nullptr;
    m_canvasContentEverLoaded = currentWorkspace->canvas != nullptr;
    m_preserveViewportOnReconnect = hasProject;
    updateClientNameDisplay(currentWorkspace->lastClientInfo);
    m_remoteClientConnected = active;
    refreshRemoteConnectionPresentation(false);

    m_navigationManager->showScreenView(currentWorkspace->lastClientInfo,
                                        currentWorkspace->canvas != nullptr);
    if (currentWorkspace->canvas) m_navigationManager->revealCanvas();
    m_applicationPage = 1;
    updateHistoryVisibilityState();
    emit activeWorkspaceChanged(m_activeWorkspaceEndpointId);
    emit applicationPageChanged(1);
    emit presentationStateChanged();

    if (!active && !grace && selectedClient.isOnline()
        && m_webSocketClient && m_webSocketClient->isConnected()) {
        m_remoteSessionOpenSuppressedTargets.remove(targetEndpointId);
        ensureRemoteSessionForClient(currentWorkspace->lastClientInfo);
    }
}

void ApplicationRuntime::updateClientNameDisplay(const ClientInfo& client) {
    m_remoteDisplayName = client.endpointId().isEmpty() ? QString() : client.getInstanceDisplayName();
    emit presentationStateChanged();
}

void ApplicationRuntime::cancelPendingRemoteSessionOpen(const QString& targetEndpointId)
{
    if (!m_remoteSessionOpenPendingTargets.contains(targetEndpointId)) return;
    RemoteSessionCoordinator* coordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    const RemoteSessionCoordinator::Binding opening = coordinator
        ? coordinator->outgoingForPeer(targetEndpointId)
        : RemoteSessionCoordinator::Binding();
    if (!opening.remoteSessionId.isEmpty() && m_webSocketClient) {
        m_cancelledInitialOpenSessionByTarget.insert(
            targetEndpointId, opening.remoteSessionId);
        rememberPendingOutgoingSessionClose(
            targetEndpointId, opening.remoteSessionId,
            opening.generation);
        m_locallyTerminatingRemoteSessions.insert(
            opening.remoteSessionId);
        retryPendingOutgoingSessionClose(
            targetEndpointId, QStringLiteral("initial_open_cancelled"));
    }
    m_remoteSessionOpenPendingTargets.remove(targetEndpointId);
    for (auto it = m_remoteSessionOpenTargetByRequestId.begin();
         it != m_remoteSessionOpenTargetByRequestId.end();) {
        if (it.value() == targetEndpointId) {
            m_automaticRemoteSessionOpenRequestIds.remove(it.key());
            m_cancelledInitialOpenTargetByRequestId.insert(
                it.key(), targetEndpointId);
            if (m_webSocketClient) m_webSocketClient->cancelRemoteSessionOpen(it.key());
            it = m_remoteSessionOpenTargetByRequestId.erase(it);
        } else {
            ++it;
        }
    }
}

void ApplicationRuntime::showClientListView() {
    m_selectionEndpointId.clear();
    m_selectionClient = ClientInfo();
    // Do NOT unload when navigating back to client list - uploads persist per session
    // Each client maintains its own upload state that should survive navigation
    
    const QString leavingTarget = m_activeWorkspaceEndpointId;
    if (!leavingTarget.isEmpty()) {
        m_remoteSessionOpenDesiredTargets.remove(leavingTarget);
    }
    if (!leavingTarget.isEmpty() && m_projectManager
        && !m_projectManager->hasProjectForTarget(leavingTarget)) {
        cancelPendingRemoteSessionOpen(leavingTarget);
    }
    if (!leavingTarget.isEmpty() && m_projectManager
        && m_navigationManager && m_navigationManager->isOnScreenView()) {
        persistProjectCanvas(leavingTarget);
    }
    if (m_navigationManager) m_navigationManager->showClientList();
    m_uploadManager->setTargetClientId(QString());
    // Navigation hides the canvas but does not terminate its RemoteSession.
    // Preserve the authenticated binding and its presentation state until the
    // hidden-project deadline or an explicit user action closes it.
    
    m_applicationPage = 0;
    updateHistoryVisibilityState();
    emit applicationPageChanged(0);
    emit presentationStateChanged();
}


bool ApplicationRuntime::hasPendingOutgoingSessionClose(
    const QString& targetEndpointId,
    const QString& remoteSessionId) const
{
    const auto it = m_pendingOutgoingSessionCloses.constFind(targetEndpointId);
    if (it == m_pendingOutgoingSessionCloses.cend()) return false;
    return remoteSessionId.isEmpty() || it->remoteSessionId == remoteSessionId;
}

bool ApplicationRuntime::hasCancelledInitialOpenForTarget(
    const QString& targetEndpointId) const
{
    if (targetEndpointId.isEmpty()) return false;
    if (m_cancelledInitialOpenSessionByTarget.contains(targetEndpointId)) {
        return true;
    }
    for (auto it = m_cancelledInitialOpenTargetByRequestId.cbegin();
         it != m_cancelledInitialOpenTargetByRequestId.cend(); ++it) {
        if (it.value() == targetEndpointId) return true;
    }
    return false;
}

void ApplicationRuntime::rememberPendingOutgoingSessionClose(
    const QString& targetEndpointId,
    const QString& remoteSessionId,
    quint64 generation)
{
    if (targetEndpointId.isEmpty() || remoteSessionId.isEmpty()) return;
    auto it = m_pendingOutgoingSessionCloses.find(targetEndpointId);
    if (it != m_pendingOutgoingSessionCloses.end()
        && it->remoteSessionId != remoteSessionId) {
        // The server must not create a replacement while the former session
        // still owns the directional index. Keep the older close fence; the
        // unexpected session is fail-closed by its Ready handler.
        qWarning() << "Ignored overlapping outgoing RemoteSession close intent"
                   << targetEndpointId << it->remoteSessionId
                   << remoteSessionId;
        return;
    }
    if (it == m_pendingOutgoingSessionCloses.end()) {
        PendingOutgoingSessionClose pending;
        pending.remoteSessionId = remoteSessionId;
        pending.generation = generation;
        m_pendingOutgoingSessionCloses.insert(targetEndpointId, pending);
        return;
    }
    if (generation > it->generation) {
        it->generation = generation;
        it->closeDispatchedOnConnectionGeneration = 0;
    }
}

bool ApplicationRuntime::retryPendingOutgoingSessionClose(
    const QString& targetEndpointId,
    const QString& reason)
{
    auto it = m_pendingOutgoingSessionCloses.find(targetEndpointId);
    if (it == m_pendingOutgoingSessionCloses.end()
        || it->remoteSessionId.isEmpty() || !m_webSocketClient) {
        return false;
    }

    RemoteSessionCoordinator* coordinator =
        m_webSocketClient->remoteSessionCoordinator();
    const RemoteSessionCoordinator::Binding binding = coordinator
        ? coordinator->outgoingForPeer(targetEndpointId)
        : RemoteSessionCoordinator::Binding();
    if (!binding.remoteSessionId.isEmpty()
        && binding.remoteSessionId != it->remoteSessionId) {
        return false;
    }
    if (binding.remoteSessionId == it->remoteSessionId
        && binding.generation > it->generation) {
        it->generation = binding.generation;
        it->closeDispatchedOnConnectionGeneration = 0;
    }
    const quint64 connectionGeneration =
        m_webSocketClient->connectionGeneration();
    if (!binding.remoteSessionId.isEmpty()
        && (binding.phase == QLatin1String("Terminating")
        || binding.phase == QLatin1String("CleanupPending")
        || binding.phase == QLatin1String("Closed"))) {
        it->closeDispatchedOnConnectionGeneration = connectionGeneration;
        return true;
    }
    if (connectionGeneration > 0
        && it->closeDispatchedOnConnectionGeneration
            == connectionGeneration) {
        return true;
    }
    QString requestId;
    const bool sent = m_webSocketClient->closeRemoteSessionByIdentity(
        it->remoteSessionId, it->generation, &requestId, reason);
    if (sent) {
        it->closeDispatchedOnConnectionGeneration = connectionGeneration;
        OutgoingSessionCloseRequest request;
        request.targetEndpointId = targetEndpointId;
        request.remoteSessionId = it->remoteSessionId;
        request.generation = it->generation;
        m_outgoingSessionCloseRequestById.insert(requestId, request);
    }
    return sent;
}

bool ApplicationRuntime::finalizePendingOutgoingSessionClose(
    const QString& targetEndpointId,
    const QString& remoteSessionId,
    quint64 generation,
    bool allowReplacementOpen)
{
    auto pending = m_pendingOutgoingSessionCloses.find(targetEndpointId);
    if (pending == m_pendingOutgoingSessionCloses.end()
        || pending->remoteSessionId != remoteSessionId
        || (generation > 0 && generation < pending->generation)) {
        return false;
    }

    RemoteSessionCoordinator* coordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    const RemoteSessionCoordinator::Binding current = coordinator
        ? coordinator->outgoingForPeer(targetEndpointId)
        : RemoteSessionCoordinator::Binding();
    if (!current.remoteSessionId.isEmpty()
        && current.remoteSessionId != remoteSessionId) {
        return false;
    }
    if (!current.remoteSessionId.isEmpty() && m_webSocketClient) {
        m_webSocketClient->discardRemoteSessionAfterAuthoritativeRejection(
            remoteSessionId);
    }
    if (m_sceneActivityModel) {
        m_sceneActivityModel->removeForSession(remoteSessionId);
    }

    m_pendingOutgoingSessionCloses.erase(pending);
    m_locallyTerminatingRemoteSessions.remove(remoteSessionId);
    if (m_currentOutgoingSessionIdByTarget.value(targetEndpointId)
        == remoteSessionId) {
        m_currentOutgoingSessionIdByTarget.remove(targetEndpointId);
        m_currentOutgoingSessionGenerationByTarget.remove(targetEndpointId);
    }
    if (m_cancelledInitialOpenSessionByTarget.value(targetEndpointId)
        == remoteSessionId) {
        m_cancelledInitialOpenSessionByTarget.remove(targetEndpointId);
    }
    for (auto it = m_cancelledInitialOpenTargetByRequestId.begin();
         it != m_cancelledInitialOpenTargetByRequestId.end();) {
        if (it.value() == targetEndpointId) {
            it = m_cancelledInitialOpenTargetByRequestId.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_outgoingSessionCloseRequestById.begin();
         it != m_outgoingSessionCloseRequestById.end();) {
        if (it->targetEndpointId == targetEndpointId
            && it->remoteSessionId == remoteSessionId) {
            it = m_outgoingSessionCloseRequestById.erase(it);
        } else {
            ++it;
        }
    }
    m_remoteSessionOpenPendingTargets.remove(targetEndpointId);
    for (auto it = m_remoteSessionOpenTargetByRequestId.begin();
         it != m_remoteSessionOpenTargetByRequestId.end();) {
        if (it.value() == targetEndpointId) {
            m_automaticRemoteSessionOpenRequestIds.remove(it.key());
            if (m_webSocketClient) m_webSocketClient->cancelRemoteSessionOpen(it.key());
            it = m_remoteSessionOpenTargetByRequestId.erase(it);
        } else {
            ++it;
        }
    }

    clearRemoteSessionRuntimeState(targetEndpointId, false);
    const bool stillSelected = m_activeWorkspaceEndpointId == targetEndpointId
        && m_navigationManager && m_navigationManager->isOnScreenView();
    const bool replacementRequested = allowReplacementOpen && stillSelected
        && (m_remoteSessionOpenDesiredTargets.contains(targetEndpointId)
            || wantsForegroundRemoteSession(targetEndpointId));
    if (!replacementRequested) {
        m_remoteSessionOpenDesiredTargets.remove(targetEndpointId);
        m_remoteSessionOpenSuppressedTargets.insert(targetEndpointId);
        return true;
    }

    m_remoteSessionOpenSuppressedTargets.remove(targetEndpointId);
    for (const ClientInfo& discovered : std::as_const(m_discoveredClients)) {
        if (discovered.endpointId() != targetEndpointId
            || !discovered.isOnline()) {
            continue;
        }
        ClientInfo available = discovered;
        // Admission is server-authoritative; preserve its presence capability.
        ensureRemoteSessionForClient(available);
        break;
    }
    return true;
}

bool ApplicationRuntime::wantsForegroundRemoteSession(
    const QString& targetEndpointId) const
{
    return !m_cleanShutdownPrepared && !isUserDisconnected()
        && !m_controlledDisconnectInProgress
        && !targetEndpointId.isEmpty()
        && targetEndpointId == m_activeWorkspaceEndpointId
        && m_navigationManager && m_navigationManager->isOnScreenView()
        && m_activityMonitor && m_activityMonitor->isActive()
        && m_selectionEndpointId == targetEndpointId
        && !m_remoteSessionAutoOpenBlockedTargets.contains(targetEndpointId);
}

void ApplicationRuntime::reconcileForegroundRemoteSession()
{
    const QString targetEndpointId = m_activeWorkspaceEndpointId;
    if (!wantsForegroundRemoteSession(targetEndpointId)) return;
    if (!m_webSocketClient || !m_webSocketClient->isConnected()) {
        m_sessionRecovery.pause(targetEndpointId, QStringLiteral("server_unavailable"));
        return;
    }

    for (const ClientInfo& discovered : std::as_const(m_discoveredClients)) {
        if (discovered.endpointId() != targetEndpointId
            || !discovered.isOnline()) continue;
        // ensureRemoteSessionForClient republishes discovery/presentation and
        // can invalidate references into m_discoveredClients.
        const ClientInfo available = discovered;
        if (m_workspaceManager)
            m_workspaceManager->getOrCreateWorkspace(targetEndpointId, available);
        m_remoteSessionOpenSuppressedTargets.remove(targetEndpointId);
        ensureRemoteSessionForClient(available);
        return;
    }
    // CLOSE does not depend on the target being discoverable: its cleanup
    // obligation must keep progressing while the peer is absent.
    if (!hasPendingOutgoingSessionClose(targetEndpointId))
        m_sessionRecovery.pause(targetEndpointId, QStringLiteral("waiting_for_presence"));
}

void ApplicationRuntime::ensureRemoteSessionForClient(const ClientInfo& client) {
    if (m_cleanShutdownPrepared || isUserDisconnected() || !m_webSocketClient) {
        return;
    }
    const QString targetEndpointId = client.endpointId().trimmed();
    if (targetEndpointId.isEmpty() || targetEndpointId == m_webSocketClient->endpointId()) {
        return;
    }

    RemoteSessionCoordinator* coordinator =
        m_webSocketClient->remoteSessionCoordinator();
    const RemoteSessionCoordinator::Binding binding = coordinator
        ? coordinator->outgoingForPeer(targetEndpointId)
        : RemoteSessionCoordinator::Binding();
    const bool terminalBinding = binding.phase == QLatin1String("Terminating")
        || binding.phase == QLatin1String("CleanupPending")
        || binding.phase == QLatin1String("Closed");
    if (terminalBinding && !binding.remoteSessionId.isEmpty()
        && binding.ownerEndpointId == m_webSocketClient->endpointId()) {
        rememberPendingOutgoingSessionClose(
            targetEndpointId, binding.remoteSessionId, binding.generation);
        m_locallyTerminatingRemoteSessions.insert(binding.remoteSessionId);
    }
    const bool closePending = hasPendingOutgoingSessionClose(targetEndpointId)
        || hasCancelledInitialOpenForTarget(targetEndpointId)
        || (!binding.remoteSessionId.isEmpty()
            && m_locallyTerminatingRemoteSessions.contains(
                binding.remoteSessionId));
    if (closePending || terminalBinding) {
        if (!terminalBinding) {
            retryPendingOutgoingSessionClose(
                targetEndpointId, QStringLiteral("local_close_retry"));
        }
        if (m_workspaceManager) {
            m_workspaceManager->setRemoteSessionState(
                targetEndpointId, WorkspaceManager::RemoteSessionState::Closing);
            updateWorkspaceCapabilities(targetEndpointId);
        }
        refreshProjectClientList();
        if (m_activeWorkspaceEndpointId == targetEndpointId) {
            m_remoteClientConnected = false;
            refreshRemoteConnectionPresentation(false);
            if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
        }
        return;
    }
    if (!binding.remoteSessionId.isEmpty()) {
        m_currentOutgoingSessionIdByTarget.insert(
            targetEndpointId, binding.remoteSessionId);
        m_currentOutgoingSessionGenerationByTarget.insert(
            targetEndpointId, binding.generation);
        const bool commandReady = isCommandReadyBinding(
            m_webSocketClient, binding);
        refreshProjectClientList();
        if (m_workspaceManager) {
            WorkspaceManager::RemoteSessionState state =
                WorkspaceManager::RemoteSessionState::Closing;
            if (commandReady) {
                state = WorkspaceManager::RemoteSessionState::Active;
            } else if (binding.phase == QLatin1String("Opening")) {
                state = WorkspaceManager::RemoteSessionState::Opening;
            } else if (binding.phase == QLatin1String("Active")
                       || binding.phase == QLatin1String("Grace")) {
                state = WorkspaceManager::RemoteSessionState::Grace;
            } else if (binding.phase == QLatin1String("Closed")) {
                state = WorkspaceManager::RemoteSessionState::Absent;
            }
            m_workspaceManager->setRemoteSessionState(targetEndpointId, state);
            updateWorkspaceCapabilities(targetEndpointId);
        }
        const bool locallyCommandReady = commandReady
            && !m_locallyTerminatingRemoteSessions.contains(binding.remoteSessionId);
        if (m_activeWorkspaceEndpointId == targetEndpointId) {
            m_remoteClientConnected = locallyCommandReady;
            refreshRemoteConnectionPresentation(false);
            updateWorkspaceCapabilities(targetEndpointId);
        }
        m_remoteSessionOpenDesiredTargets.remove(targetEndpointId);
        return;
    }

    if (m_remoteSessionOpenPendingTargets.contains(targetEndpointId)) {
        m_remoteSessionOpenDesiredTargets.remove(targetEndpointId);
        refreshProjectClientList();
        if (m_activeWorkspaceEndpointId == targetEndpointId && m_uploadManager) {
            m_uploadManager->setTargetClientId(QString());
        }
        return;
    }
    if (m_remoteSessionOpenSuppressedTargets.contains(targetEndpointId)
        && !m_remoteSessionOpenDesiredTargets.contains(targetEndpointId)) {
        refreshProjectClientList();
        if (m_activeWorkspaceEndpointId == targetEndpointId) {
            refreshRemoteConnectionPresentation(false);
            if (m_activeCanvas) m_activeCanvas->setOverlayActionsEnabled(false);
            if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
        }
        return;
    }
    if (m_remoteSessionOpenDesiredTargets.contains(targetEndpointId)) {
        m_remoteSessionOpenSuppressedTargets.remove(targetEndpointId);
    }

    const bool canOpen = client.canAcceptSession()
        && m_connectionManager && m_connectionManager->isReady();
    if (!canOpen) {
        m_sessionRecovery.pause(targetEndpointId, client.canAcceptSession()
            ? QStringLiteral("waiting_for_server_readiness") : QStringLiteral("waiting_for_presence"));
        refreshProjectClientList();
        if (m_activeWorkspaceEndpointId == targetEndpointId) {
            refreshRemoteConnectionPresentation(false);
            if (m_activeCanvas) m_activeCanvas->setOverlayActionsEnabled(false);
            if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
        }
        return;
    }

    if (m_sessionRecovery.waiting(targetEndpointId)) return;
    if (m_workspaceManager) m_workspaceManager->getOrCreateWorkspace(targetEndpointId, client);
    QString requestId;
    if (!m_webSocketClient->openRemoteSession(targetEndpointId, &requestId)) {
        m_sessionRecovery.retry(targetEndpointId, QStringLiteral("control_send_unavailable"));
        if (m_workspaceManager) {
            m_workspaceManager->setRemoteSessionState(
                targetEndpointId, WorkspaceManager::RemoteSessionState::Absent);
        }
        refreshProjectClientList();
        if (m_activeWorkspaceEndpointId == targetEndpointId && m_uploadManager) {
            m_uploadManager->setTargetClientId(QString());
        }
        return;
    }
    m_remoteSessionOpenPendingTargets.insert(targetEndpointId);
    m_sessionRecovery.running(targetEndpointId);
    m_remoteSessionOpenTargetByRequestId.insert(requestId, targetEndpointId);
    if (!m_remoteSessionOpenDesiredTargets.contains(targetEndpointId)) {
        m_automaticRemoteSessionOpenRequestIds.insert(requestId);
    }
    m_remoteSessionOpenDesiredTargets.remove(targetEndpointId);
    refreshProjectClientList();
    if (m_workspaceManager) {
        m_workspaceManager->setRemoteSessionState(
            targetEndpointId, WorkspaceManager::RemoteSessionState::Opening);
        updateWorkspaceCapabilities(targetEndpointId);
    }
    if (m_activeWorkspaceEndpointId == targetEndpointId) {
        m_remoteClientConnected = false;
        refreshRemoteConnectionPresentation(false);
        if (m_activeCanvas) m_activeCanvas->setOverlayActionsEnabled(false);
        if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
    }
}

void ApplicationRuntime::handleRemoteSessionReady(const QJsonObject& envelope,
                                          bool resumed) {
    if (!m_webSocketClient) return;
    const QString ownerEndpointId = envelope.value(QStringLiteral("ownerEndpointId")).toString();
    const QString targetEndpointId = envelope.value(QStringLiteral("targetEndpointId")).toString();
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    const QString phase = envelope.value(QStringLiteral("phase")).toString();
    const bool activePhase = phase == QLatin1String("Active");
    const bool resumableGrace = resumed && phase == QLatin1String("Grace");
    // Active admits the authenticated snapshot, but commands still await both
    // applied-state receipts and a usable local session proof.
    if (!activePhase && !resumableGrace) return;
    const bool commandActive = activePhase && isCommandReadyBinding(
        m_webSocketClient,
        m_webSocketClient->remoteSessionCoordinator()->byId(remoteSessionId));
    quint64 generation = 0;
    readSafePositiveJsonInteger(
        envelope.value(QStringLiteral("generation")), &generation);
    const QString localEndpointId = m_webSocketClient->endpointId();
    const QString peerEndpointId = ownerEndpointId == localEndpointId
        ? targetEndpointId
        : (targetEndpointId == localEndpointId ? ownerEndpointId : QString());
    if (peerEndpointId.isEmpty()) return;

    // A response to an in-flight open (or a new incoming session) may cross
    // the quit request. Never let it resurrect command-capable state while the
    // transport is being drained: include it in the same idempotent close set.
    if (m_cleanShutdownPrepared) {
        if (!remoteSessionId.isEmpty()) {
            m_locallyTerminatingRemoteSessions.insert(remoteSessionId);
            m_webSocketClient->closeRemoteSession(
                remoteSessionId, nullptr, QStringLiteral("clean_shutdown"));
            if (targetEndpointId == localEndpointId && m_remoteSceneController) {
                m_cleanShutdownRendererPendingSessionIds.insert(remoteSessionId);
                enqueueRendererTeardown(remoteSessionId);
            }
        }
        return;
    }

    const QString requestId = envelope.value(QStringLiteral("requestId")).toString();
    bool cancelledRequest = false;
    if (ownerEndpointId == localEndpointId && !requestId.isEmpty()) {
        const auto cancelled =
            m_cancelledInitialOpenTargetByRequestId.find(requestId);
        if (cancelled != m_cancelledInitialOpenTargetByRequestId.end()
            && cancelled.value() == peerEndpointId) {
            cancelledRequest = true;
            m_cancelledInitialOpenTargetByRequestId.erase(cancelled);
        }
    }
    bool cancelledSession = false;
    const auto cancelledBySession =
        m_cancelledInitialOpenSessionByTarget.find(peerEndpointId);
    if (ownerEndpointId == localEndpointId
        && cancelledBySession != m_cancelledInitialOpenSessionByTarget.end()
        && cancelledBySession.value() == remoteSessionId) {
        cancelledSession = true;
        m_cancelledInitialOpenSessionByTarget.erase(cancelledBySession);
    }
    if (ownerEndpointId == localEndpointId && !requestId.isEmpty()) {
        if (m_webSocketClient) m_webSocketClient->cancelRemoteSessionOpen(requestId);
        m_remoteSessionOpenTargetByRequestId.remove(requestId);
        m_automaticRemoteSessionOpenRequestIds.remove(requestId);
    }

    const bool cancelledInitial = ownerEndpointId == localEndpointId
        && (cancelledRequest || cancelledSession);
    const bool pendingCloseForPeer = ownerEndpointId == localEndpointId
        && hasPendingOutgoingSessionClose(peerEndpointId);
    const bool matchingPendingClose = ownerEndpointId == localEndpointId
        && hasPendingOutgoingSessionClose(peerEndpointId, remoteSessionId);
    const bool locallyClosing = cancelledInitial || pendingCloseForPeer
        || m_locallyTerminatingRemoteSessions.contains(remoteSessionId);
    if (locallyClosing) {
        m_remoteSessionOpenPendingTargets.remove(peerEndpointId);
        m_remoteSessionOpenSuppressedTargets.insert(peerEndpointId);
        m_locallyTerminatingRemoteSessions.insert(remoteSessionId);
        if (ownerEndpointId == localEndpointId) {
            if (!pendingCloseForPeer || matchingPendingClose) {
                rememberPendingOutgoingSessionClose(
                    peerEndpointId, remoteSessionId, generation);
                retryPendingOutgoingSessionClose(
                    peerEndpointId,
                    cancelledInitial
                        ? QStringLiteral("initial_open_cancelled")
                        : QStringLiteral("local_close_retry"));
            } else {
                // A second session for the same peer before the first Closed
                // violates the one-directional-session invariant. Refuse it
                // without weakening the original close fence.
                m_webSocketClient->closeRemoteSession(
                    remoteSessionId, nullptr,
                    QStringLiteral("previous_session_cleanup_pending"));
            }
            if (m_workspaceManager) {
                m_workspaceManager->setRemoteSessionState(
                    peerEndpointId,
                    WorkspaceManager::RemoteSessionState::Closing);
            }
            clearRemoteSessionRuntimeState(peerEndpointId, true);
            refreshProjectClientList();
            if (m_activeWorkspaceEndpointId == peerEndpointId) {
                m_remoteClientConnected = false;
                refreshRemoteConnectionPresentation(false);
            }
        } else {
            m_webSocketClient->closeRemoteSession(
                remoteSessionId, nullptr,
                QStringLiteral("local_close_pending"));
        }
        return;
    }

    if (ownerEndpointId != localEndpointId) {
        // Incoming sessions do not create a local Project and must never
        // consume or rewrite the independent outgoing intent for this peer.
        return;
    }
    m_remoteSessionOpenPendingTargets.remove(peerEndpointId);
    m_remoteSessionOpenSuppressedTargets.remove(peerEndpointId);
    m_remoteSessionOpenDesiredTargets.remove(peerEndpointId);
    m_locallyTerminatingRemoteSessions.remove(remoteSessionId);
    // RESUME can complete a pending OPEN without echoing its requestId. Retire
    // that transaction too so a later transport cannot replay a settled OPEN.
    for (auto it = m_remoteSessionOpenTargetByRequestId.begin();
         it != m_remoteSessionOpenTargetByRequestId.end();) {
        if (it.value() == peerEndpointId) {
            m_automaticRemoteSessionOpenRequestIds.remove(it.key());
            if (m_webSocketClient) m_webSocketClient->cancelRemoteSessionOpen(it.key());
            it = m_remoteSessionOpenTargetByRequestId.erase(it);
        } else {
            ++it;
        }
    }

    m_currentOutgoingSessionIdByTarget.insert(
        peerEndpointId, remoteSessionId);
    m_currentOutgoingSessionGenerationByTarget.insert(
        peerEndpointId, generation);

    const auto failCloseOutgoing = [this, &peerEndpointId, &remoteSessionId,
                                    generation](const QString& reason) {
        rememberPendingOutgoingSessionClose(
            peerEndpointId, remoteSessionId, generation);
        m_locallyTerminatingRemoteSessions.insert(remoteSessionId);
        m_remoteSessionOpenSuppressedTargets.insert(peerEndpointId);
        m_remoteSessionAutoOpenBlockedTargets.insert(peerEndpointId);
        retryPendingOutgoingSessionClose(peerEndpointId, reason);
        if (m_workspaceManager) {
            m_workspaceManager->setRemoteSessionState(
                peerEndpointId,
                WorkspaceManager::RemoteSessionState::Closing);
            updateWorkspaceCapabilities(peerEndpointId);
        }
    };

    ClientWorkspace* session = m_workspaceManager
        ? m_workspaceManager->findWorkspace(peerEndpointId) : nullptr;
    if (!session) {
        failCloseOutgoing(QStringLiteral("workspace_gone"));
        return;
    }

    if (activePhase) {
        // The OPEN/RESUME has succeeded even if its command barrier is pending.
        m_sessionRecovery.reset(peerEndpointId);
    }
    const bool hadProject = m_projectManager
        && m_projectManager->hasProjectForTarget(peerEndpointId);
    // Active RESUME snapshots have passed the same protocol validation as
    // initial snapshots. Install their latest volume/topology for retained
    // projects as well; the value may have changed during the outage.
    const bool hasResumedSnapshot = resumed && envelope.contains(QStringLiteral("snapshot"))
        && m_webSocketClient->remoteSessionCoordinator()->byId(remoteSessionId).active;
    if (!resumed || !hadProject || hasResumedSnapshot) {
        const QJsonObject snapshot = hasResumedSnapshot
            ? m_webSocketClient->remoteSessionCoordinator()->latestSnapshot(remoteSessionId)
            : envelope.value(QStringLiteral("snapshot")).toObject();
        const QJsonArray screenValues =
            snapshot.value(QStringLiteral("screens")).toArray();
        QList<ScreenInfo> screens;
        screens.reserve(screenValues.size());
        for (const QJsonValue& value : screenValues) {
            screens.append(ScreenInfo::fromJson(value.toObject()));
        }
        quint64 snapshotRevision = 0;
        const bool validRevision = readSafePositiveJsonInteger(
            snapshot.value(QStringLiteral("revision")), &snapshotRevision);
        const qint64 capturedAtMs = static_cast<qint64>(
            snapshot.value(QStringLiteral("capturedAtEpochMs")).toDouble(-1));
        const int volumePercent = snapshot.value(QStringLiteral("volumePercent"))
                                      .isNull()
            ? -1 : snapshot.value(QStringLiteral("volumePercent")).toInt(-1);
        if (!validRevision || capturedAtMs < 1
            || volumePercent < -1 || volumePercent > 100) {
            failCloseOutgoing(QStringLiteral("invalid_initial_snapshot"));
            handleRemoteSessionError(QJsonObject{
                {QStringLiteral("code"), QStringLiteral("invalid_initial_snapshot")},
                {QStringLiteral("targetEndpointId"), peerEndpointId},
                {QStringLiteral("message"),
                 QStringLiteral("The remote client returned an invalid initial snapshot.")}
            });
            return;
        }
        session->lastClientInfo.setScreens(screens);
        session->lastClientInfo.setVolumePercent(volumePercent);
        if (!hadProject) {
            const QString projectId = m_projectManager
                ? m_projectManager->createProjectFromSnapshot(
                    ProjectTargetReference::fromClientInfo(
                        session->lastClientInfo),
                    screens, volumePercent, snapshotRevision,
                    capturedAtMs)
                : QString();
            if (projectId.isEmpty()) {
                failCloseOutgoing(
                    QStringLiteral("project_persistence_failed"));
                if (m_workspaceManager) {
                    m_workspaceManager->setRemoteSessionState(
                        peerEndpointId,
                        WorkspaceManager::RemoteSessionState::Closing);
                }
                TOAST_ERROR(QStringLiteral(
                    "The project could not be saved. The remote session was closed."),
                    4000);
                if (m_activeWorkspaceEndpointId == peerEndpointId) {
                    showClientListView();
                }
                return;
            }
            m_workspaceManager->updateWorkspaceProjectId(peerEndpointId, projectId);
            session = m_workspaceManager->findWorkspace(peerEndpointId);
            if (!session) return;
        } else if (m_projectManager) {
            m_projectManager->updateRemoteSnapshot(
                peerEndpointId, screens, volumePercent, snapshotRevision,
                capturedAtMs);
        }
    }

    if (m_workspaceManager) {
        m_workspaceManager->setRemoteSessionState(
            peerEndpointId,
            commandActive ? WorkspaceManager::RemoteSessionState::Active
                          : WorkspaceManager::RemoteSessionState::Grace);
    }
    refreshProjectClientList();
    if (m_activeWorkspaceEndpointId == peerEndpointId) {
        if (!session->canvas) ensureWorkspace(session->lastClientInfo);
        session = m_workspaceManager->findWorkspace(peerEndpointId);
        if (!session) return;
        if (!hadProject) {
            // This canvas is the initial in-memory state of the Project just
            // created from the snapshot; there is no older authoring graph to
            // restore. Mark it initialized so later navigation cannot attempt
            // to restore into the already-live document.
            m_restoredProjectIds.insert(peerEndpointId);
        }
        if (session->canvas) {
            session->canvas->setScreens(session->lastClientInfo.getScreens());
            switchToWorkspace(peerEndpointId);
        }
        // The authenticated snapshot is the first authoritative presentation
        // state for a new Project. Publish only after the canvas, topology and
        // volume have all been installed; the initial page transition happened
        // earlier while the workspace intentionally had no canvas.
        m_selectedClient = session->lastClientInfo;
        m_remoteClientConnected = commandActive;
        refreshRemoteConnectionPresentation(false);
        updateWorkspaceCapabilities(peerEndpointId);
        if (session->canvas && !m_canvasRevealedForCurrentClient) {
            if (m_navigationManager) m_navigationManager->revealCanvas();
            if (!hadProject) {
                session->canvas->requestDeferredInitialRecenter(53);
            }
            m_preserveViewportOnReconnect = false;
            m_canvasRevealedForCurrentClient = true;
            m_canvasContentEverLoaded = true;
        }
        emit activeWorkspaceChanged(peerEndpointId);
    }
}

void ApplicationRuntime::handleRemoteSessionSnapshot(const QJsonObject& envelope)
{
    if (!m_webSocketClient || !m_projectManager || !m_workspaceManager) return;
    const QString targetEndpointId =
        envelope.value(QStringLiteral("targetEndpointId")).toString();
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    if (envelope.value(QStringLiteral("ownerEndpointId")).toString()
            != m_webSocketClient->endpointId()
        || targetEndpointId.isEmpty()
        || hasPendingOutgoingSessionClose(targetEndpointId)
        || m_locallyTerminatingRemoteSessions.contains(remoteSessionId)
        || !m_projectManager->hasProjectForTarget(targetEndpointId)) {
        return;
    }
    const QJsonObject snapshot =
        envelope.value(QStringLiteral("snapshot")).toObject();
    QList<ScreenInfo> screens;
    for (const QJsonValue& value :
         snapshot.value(QStringLiteral("screens")).toArray()) {
        screens.append(ScreenInfo::fromJson(value.toObject()));
    }
    quint64 revision = 0;
    if (!readSafePositiveJsonInteger(
            snapshot.value(QStringLiteral("revision")), &revision)) {
        return;
    }
    const qint64 capturedAtMs = static_cast<qint64>(
        snapshot.value(QStringLiteral("capturedAtEpochMs")).toDouble(-1));
    const int volumePercent = snapshot.value(QStringLiteral("volumePercent"))
                                  .isNull()
        ? -1 : snapshot.value(QStringLiteral("volumePercent")).toInt(-1);
    if (capturedAtMs < 1 || volumePercent < -1 || volumePercent > 100) return;

    ClientWorkspace* workspace =
        m_workspaceManager->findWorkspace(targetEndpointId);
    if (!workspace) return;
    const bool topologyChanged = workspace->lastClientInfo.getScreens() != screens;
    if (topologyChanged && workspace->canvas) workspace->canvas->hideRemoteCursor();
    workspace->lastClientInfo.setScreens(screens);
    workspace->lastClientInfo.setVolumePercent(volumePercent);
    m_projectManager->updateRemoteSnapshot(
        targetEndpointId, screens, volumePercent, revision, capturedAtMs);
    if (workspace->canvas) workspace->canvas->setScreens(screens);
    if (m_activeWorkspaceEndpointId == targetEndpointId) {
        m_selectedClient = workspace->lastClientInfo;
        // An accepted snapshot updates retained presentation even while
        // remote commands are unavailable. Only an explicit unknown clears it.
        if (topologyChanged || m_remoteVolumePercent != volumePercent) {
            m_remoteVolumePercent = volumePercent;
            emit presentationStateChanged();
        }
    }
}

void ApplicationRuntime::handleRemoteSessionLeaseState(const QJsonObject& envelope) {
    if (!m_webSocketClient) return;
    const QString ownerEndpointId = envelope.value(QStringLiteral("ownerEndpointId")).toString();
    if (ownerEndpointId != m_webSocketClient->endpointId()) return;
    const QString targetEndpointId = envelope.value(QStringLiteral("targetEndpointId")).toString();
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    const QString phase = envelope.value(QStringLiteral("phase")).toString();
    const QString state = envelope.value(QStringLiteral("state")).toString(phase);
    quint64 generation = 0;
    readSafePositiveJsonInteger(
        envelope.value(QStringLiteral("generation")), &generation);
    if (!matchesCurrentOutgoingSession(targetEndpointId, remoteSessionId, generation)) return;
    const bool terminalPhase = phase == QLatin1String("Terminating")
        || phase == QLatin1String("CleanupPending")
        || phase == QLatin1String("Closed");
    if (terminalPhase && !remoteSessionId.isEmpty()) {
        rememberPendingOutgoingSessionClose(
            targetEndpointId, remoteSessionId, generation);
        m_locallyTerminatingRemoteSessions.insert(remoteSessionId);
    }
    const bool pendingClose = hasPendingOutgoingSessionClose(targetEndpointId)
        || m_locallyTerminatingRemoteSessions.contains(remoteSessionId);
    if (pendingClose || terminalPhase) {
        if (!terminalPhase
            && hasPendingOutgoingSessionClose(
                targetEndpointId, remoteSessionId)) {
            rememberPendingOutgoingSessionClose(
                targetEndpointId, remoteSessionId, generation);
            retryPendingOutgoingSessionClose(
                targetEndpointId, QStringLiteral("late_session_state"));
        }
        clearRemoteSessionRuntimeState(targetEndpointId, true, remoteSessionId);
        refreshProjectClientList();
        if (m_activeWorkspaceEndpointId == targetEndpointId) {
            m_remoteClientConnected = false;
            refreshRemoteConnectionPresentation(false);
        }
        return;
    }

    if (phase != QLatin1String("Grace") && state != QLatin1String("Grace")
        && state != QLatin1String("Degraded")
        && !envelope.value(QStringLiteral("degraded")).toBool()
        && !(phase == QLatin1String("Active") && state == QLatin1String("Active"))) return;
    m_currentOutgoingSessionIdByTarget.insert(
        targetEndpointId, remoteSessionId);
    m_currentOutgoingSessionGenerationByTarget.insert(
        targetEndpointId, generation);
    refreshProjectClientList();
    const auto currentBinding = m_webSocketClient->remoteSessionCoordinator()->byId(remoteSessionId);
    const bool commandReady = isCommandReadyBinding(m_webSocketClient, currentBinding)
        && !m_locallyTerminatingRemoteSessions.contains(remoteSessionId);
    if (m_workspaceManager) {
        WorkspaceManager::RemoteSessionState workspaceState =
            WorkspaceManager::RemoteSessionState::Closing;
        if (commandReady) {
            workspaceState = WorkspaceManager::RemoteSessionState::Active;
        } else if (currentBinding.phase == QLatin1String("Grace") || currentBinding.phase == QLatin1String("Active")) {
            workspaceState = WorkspaceManager::RemoteSessionState::Grace;
        }
        m_workspaceManager->setRemoteSessionState(targetEndpointId,
                                                workspaceState);
        updateWorkspaceCapabilities(targetEndpointId);
    }
    if (m_activeWorkspaceEndpointId == targetEndpointId) {
        m_remoteClientConnected = commandReady;
        refreshRemoteConnectionPresentation(false);
    }
}

void ApplicationRuntime::handleRemoteSessionTerminating(const QJsonObject& envelope) {
    if (!m_webSocketClient) return;
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    const QString teardownId = envelope.value(QStringLiteral("teardownId")).toString();
    const QString ownerEndpointId = envelope.value(QStringLiteral("ownerEndpointId")).toString();
    const QString targetEndpointId = envelope.value(QStringLiteral("targetEndpointId")).toString();
    quint64 generation = 0;
    if (remoteSessionId.isEmpty() || teardownId.isEmpty()
        || !readSafePositiveJsonInteger(
            envelope.value(QStringLiteral("generation")), &generation)) {
        return;
    }
    if (ownerEndpointId == m_webSocketClient->endpointId()
        && !matchesCurrentOutgoingSession(targetEndpointId, remoteSessionId, generation)) return;
    m_locallyTerminatingRemoteSessions.insert(remoteSessionId);

    if (ownerEndpointId == m_webSocketClient->endpointId()) {
        rememberPendingOutgoingSessionClose(
            targetEndpointId, remoteSessionId, generation);
        if (m_activeWorkspaceEndpointId == targetEndpointId) m_remoteClientConnected = false;
        clearRemoteSessionRuntimeState(targetEndpointId, true, remoteSessionId);
        if (m_remoteSessionOpenDesiredTargets.contains(targetEndpointId)
            || wantsForegroundRemoteSession(targetEndpointId)) {
            refreshProjectClientList();
            if (m_activeWorkspaceEndpointId == targetEndpointId) {
                refreshRemoteConnectionPresentation(false);
            }
        }
        return;
    }
    if (targetEndpointId != m_webSocketClient->endpointId()) return;

    // Only a fully committed result is retained in this replay table. Failed
    // attempts remain in the renderer queue and are retried from an external
    // progress edge (a settled teardown or a bounded server retry).
    const auto acknowledged = m_pendingTeardownAcks.constFind(remoteSessionId);
    if (acknowledged != m_pendingTeardownAcks.cend()) {
        if (acknowledged->teardownId == teardownId) retryPendingTeardownAcks();
        return;
    }
    const auto pending = m_pendingRendererTeardowns.constFind(remoteSessionId);
    if (pending != m_pendingRendererTeardowns.cend()) {
        if (pending->ownerEndpointId == ownerEndpointId
            && pending->teardownId == teardownId
            && pending->generation == generation) {
            startNextPendingRendererTeardown();
        }
        return;
    }

    // Target-side commit order is enforced asynchronously: renderer objects
    // are stopped/detached now, but cache quarantine is forbidden until their
    // actual QObject/native-window destruction completes.
    PendingRendererTeardown rendererTeardown;
    rendererTeardown.ownerEndpointId = ownerEndpointId;
    rendererTeardown.teardownId = teardownId;
    rendererTeardown.generation = generation;
    m_pendingRendererTeardowns.insert(remoteSessionId, rendererTeardown);
    enqueueRendererTeardown(remoteSessionId);
}

void ApplicationRuntime::enqueueRendererTeardown(
    const QString& remoteSessionId)
{
    if (remoteSessionId.isEmpty()) return;
    if (!m_pendingRendererTeardownOrder.contains(remoteSessionId)) {
        m_pendingRendererTeardownOrder.append(remoteSessionId);
    }
    startNextPendingRendererTeardown();
}

void ApplicationRuntime::startNextPendingRendererTeardown(
    const QString& excludedRemoteSessionId)
{
    if (!m_remoteSceneController
        || !m_activeRendererTeardownSessionId.isEmpty()) {
        return;
    }

    // Exactly one request owns the asynchronous renderer barrier at a time.
    // RemoteSceneController can also be busy with a normal STOP or transport
    // cleanup that is not represented in this map; in that case admission
    // returns false and its eventual teardownSettled signal re-enters here.
    for (const QString& remoteSessionId :
         std::as_const(m_pendingRendererTeardownOrder)) {
        if (remoteSessionId == excludedRemoteSessionId
            || m_readerTeardownPendingSessionIds.contains(remoteSessionId)) continue;
        const bool requested =
            m_pendingRendererTeardowns.contains(remoteSessionId)
            || m_terminalRendererPendingSessionIds.contains(remoteSessionId)
            || m_cleanShutdownRendererPendingSessionIds.contains(
                remoteSessionId);
        if (!requested) continue;
        auto pending = m_pendingRendererTeardowns.find(remoteSessionId);

        // A CLOSED/cleanup_error echo is not a new cleanup opportunity. Keep
        // failures bounded even when talking to an older server that echoes
        // every negative ACK immediately.
        if (pending != m_pendingRendererTeardowns.end()
            && pending->nextRetryAtMs > MouffetteClock::nowMs()) continue;

        // Mark before calling into the controller so even a future synchronous
        // implementation of the acceptance path cannot race its settlement.
        m_activeRendererTeardownSessionId = remoteSessionId;
        if (pending != m_pendingRendererTeardowns.end()) {
            pending->rendererTeardownStarted = true;
        }
        if (m_remoteSceneController->teardownRemoteSession(remoteSessionId)) {
            return;
        }

        // A refusal is admission backpressure, not a terminal cleanup result.
        // Leave the immutable transaction queued. Do not spin through it (or
        // manufacture a replayable failure ACK) on the same event-loop edge.
        m_activeRendererTeardownSessionId.clear();
        pending = m_pendingRendererTeardowns.find(remoteSessionId);
        if (pending != m_pendingRendererTeardowns.end()) {
            pending->rendererTeardownStarted = false;
        }
        return;
    }
}

void ApplicationRuntime::beginTerminalIncomingCacheCleanup(const QString& reasonCode)
{
    if (m_cleanShutdownPrepared || !m_uploadManager || !m_webSocketClient) {
        return;
    }

    // Stop the receiver writer/timer and fail-close advertisement before any
    // asynchronous renderer destruction starts. This call does not touch the
    // cache namespace or FileManager mappings.
    if (reasonCode != QLatin1String("incoming_session_orphan_timeout")
        && reasonCode != QLatin1String("session_recovery_expired")
        && reasonCode != QLatin1String("session_authoritatively_absent")) {
        // Lease/server terminal events supersede a narrower local watchdog
        // cleanup and must quarantine every incoming scope.
        m_terminalIncomingSessionFilter.clear();
        m_terminalIncomingCleanupAll = true;
    }
    if (m_terminalIncomingCleanupAll) m_terminalIncomingSessionFilter.clear();
    m_uploadManager->beginTerminalIncomingCleanup(
        reasonCode, m_terminalIncomingSessionFilter);

    if (!m_terminalIncomingCleanupActive) {
        m_terminalIncomingCleanupActive = true;
        m_terminalIncomingCacheTeardownStarted = false;
    }
    if (!reasonCode.trimmed().isEmpty()) {
        m_terminalIncomingCleanupReason = reasonCode.trimmed();
    }

    const QString localEndpointId = m_webSocketClient->endpointId();
    RemoteSessionCoordinator* coordinator =
        m_webSocketClient->remoteSessionCoordinator();
    const QList<RemoteSessionCoordinator::Binding> bindings = coordinator
        ? coordinator->all() : QList<RemoteSessionCoordinator::Binding>();

    // This slot is reached synchronously from leaseExpired/serverRestarted.
    // WebSocketClient deliberately clears the coordinator only after signal
    // delivery, so capture every incoming id now; a queued scan would be too
    // late and could quarantine media still held by the renderer.
    for (const RemoteSessionCoordinator::Binding& binding : bindings) {
        if (binding.targetEndpointId != localEndpointId
            || binding.remoteSessionId.isEmpty()
            || (!m_terminalIncomingSessionFilter.isEmpty()
                && !m_terminalIncomingSessionFilter.contains(
                    binding.remoteSessionId))
            || m_terminalRendererPendingSessionIds.contains(
                binding.remoteSessionId)) {
            continue;
        }
        m_terminalRendererPendingSessionIds.insert(binding.remoteSessionId);
        enqueueRendererTeardown(binding.remoteSessionId);
    }
    // The coordinator can be cleared by the lease timer at the same event-loop
    // edge as the independent watchdog. Its immutable capture still owns the
    // renderer barrier even when no current binding remains enumerable.
    for (const QString& remoteSessionId :
         std::as_const(m_terminalIncomingSessionFilter)) {
        if (m_terminalRendererPendingSessionIds.contains(remoteSessionId)) {
            continue;
        }
        m_terminalRendererPendingSessionIds.insert(remoteSessionId);
        enqueueRendererTeardown(remoteSessionId);
    }
    finishTerminalIncomingCacheCleanupIfReady();
}

void ApplicationRuntime::finishTerminalIncomingCacheCleanupIfReady()
{
    if (!m_terminalIncomingCleanupActive
        || m_terminalIncomingCacheTeardownStarted
        || !m_terminalRendererPendingSessionIds.isEmpty()
        || !m_uploadManager
        || !m_uploadManager->incomingFileReadersSettled(m_terminalIncomingSessionFilter)) {
        return;
    }

    m_terminalIncomingCacheTeardownStarted = true;
    const UploadManager::BulkTeardownResult cleanup =
        m_uploadManager->completeTerminalIncomingCleanup(
            m_terminalIncomingCleanupReason,
            m_terminalIncomingSessionFilter);
    if (cleanup.pendingScopes > 0) {
        m_terminalIncomingCacheTeardownStarted = false;
        return;
    }
    if (!cleanup.allLogicallyCommitted()) {
        qCritical() << "Terminal incoming cache cleanup remains pending:"
                    << cleanup.errorCode
                    << "failed scopes" << cleanup.cleanupErrorScopes;
    }
    m_terminalIncomingSessionFilter.clear();
    m_terminalIncomingCleanupActive = false;
    m_terminalIncomingCleanupAll = false;
}

void ApplicationRuntime::armIncomingSessionOrphanWatchdog()
{
    if (m_cleanShutdownPrepared || !m_incomingSessionOrphanWatchdog
        || m_incomingSessionOrphanWatchdog->active() || !m_webSocketClient) {
        return;
    }
    RemoteSessionCoordinator* coordinator =
        m_webSocketClient->remoteSessionCoordinator();
    if (!coordinator) return;
    const QString localEndpointId = m_webSocketClient->endpointId();
    QSet<QString> incoming;
    // Versioned sessions already have a suspend-inclusive deadline owned by
    // WebSocketClient. Only legacy unversioned bindings use this fallback.
    for (const RemoteSessionCoordinator::Binding& binding : coordinator->all()) {
        if (binding.stateRevision == 0 && binding.targetEndpointId == localEndpointId
            && binding.ownerEndpointId != localEndpointId
            && !binding.remoteSessionId.isEmpty()
            && binding.phase != QLatin1String("Closed")) {
            incoming.insert(binding.remoteSessionId);
        }
    }
    if (incoming.isEmpty()) return;

    m_incomingOrphanSessionIds = incoming;
    const qint64 advertisedLeaseMs = m_webSocketClient->serverPolicy()
        .value(QStringLiteral("leaseTimeoutMs")).toInteger(0);
    m_incomingSessionOrphanWatchdog->arm(incoming, advertisedLeaseMs);
}

void ApplicationRuntime::cancelIncomingSessionOrphanWatchdogIfResumed(
    const QJsonObject& envelope)
{
    if (!m_incomingSessionOrphanWatchdog || !m_webSocketClient) return;
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    const QString targetEndpointId =
        envelope.value(QStringLiteral("targetEndpointId")).toString();
    const QString ownerEndpointId =
        envelope.value(QStringLiteral("ownerEndpointId")).toString();
    if (targetEndpointId != m_webSocketClient->endpointId()
        || ownerEndpointId == targetEndpointId
        || !m_incomingSessionOrphanWatchdog
                ->authenticatedSessionResumed(remoteSessionId)) {
        return;
    }
    m_incomingOrphanSessionIds.remove(remoteSessionId);
}

void ApplicationRuntime::handleRemoteRendererTeardownSettled(
    const QString& remoteSessionId,
    bool sceneStopped)
{
    if (remoteSessionId.isEmpty()) {
        // A normal SceneRun STOP can be the barrier that caused target-side
        // RemoteSession teardown admission to return false.
        startNextPendingRendererTeardown();
        return;
    }
    if (m_activeRendererTeardownSessionId == remoteSessionId) {
        m_activeRendererTeardownSessionId.clear();
    }
    const bool terminalTransportBarrier =
        m_terminalRendererPendingSessionIds.contains(remoteSessionId);
    const bool cleanShutdownBarrier =
        m_cleanShutdownRendererPendingSessionIds.contains(remoteSessionId);
    if (sceneStopped) {
        m_terminalRendererPendingSessionIds.remove(remoteSessionId);
        m_cleanShutdownRendererPendingSessionIds.remove(remoteSessionId);
    }
    const auto pending = m_pendingRendererTeardowns.find(remoteSessionId);
    if (pending == m_pendingRendererTeardowns.end()) {
        if (terminalTransportBarrier && sceneStopped) {
            finishTerminalIncomingCacheCleanupIfReady();
        }
        if (cleanShutdownBarrier && sceneStopped) {
            finishCleanShutdownIncomingCacheTeardownIfReady();
        }
        if (!m_terminalRendererPendingSessionIds.contains(remoteSessionId)
            && !m_cleanShutdownRendererPendingSessionIds.contains(
                remoteSessionId)) {
            m_pendingRendererTeardownOrder.removeAll(remoteSessionId);
        }
        // A reported renderer failure must not synchronously retry the same
        // transaction forever. Drain other queued sessions; this one remains
        // fail-closed for a later external recovery edge/startup cleanup.
        startNextPendingRendererTeardown(
            sceneStopped ? QString() : remoteSessionId);
        return;
    }
    const PendingRendererTeardown teardown = pending.value();
    if (sceneStopped && m_uploadManager) {
        m_uploadManager->beginIncomingFileReaderTeardown({remoteSessionId});
        if (!m_uploadManager->incomingFileReadersSettled({remoteSessionId})) {
            m_readerTeardownPendingSessionIds.insert(remoteSessionId);
            startNextPendingRendererTeardown(remoteSessionId);
            return;
        }
    }

    RemoteCacheStore::CommitResult cacheResult;
    bool uploadsAborted = false;
    int removedFileCount = 0;
    if (sceneStopped && m_uploadManager) {
        cacheResult = m_uploadManager->teardownRemoteSession(
            teardown.ownerEndpointId, remoteSessionId, teardown.generation,
            teardown.teardownId);
        uploadsAborted = true;
        removedFileCount = m_uploadManager->lastTeardownRemovedFileCount();
    } else {
        cacheResult.outcome = RemoteCacheStore::CommitOutcome::CleanupError;
        cacheResult.teardownId = teardown.teardownId;
        cacheResult.errorCode = sceneStopped
            ? QStringLiteral("upload_manager_unavailable")
            : QStringLiteral("renderer_teardown_failed");
    }

    if (cacheResult.outcome == RemoteCacheStore::CommitOutcome::Pending) {
        m_readerTeardownPendingSessionIds.insert(remoteSessionId);
        startNextPendingRendererTeardown(remoteSessionId);
        return;
    }

    PendingTeardownAck ack;
    ack.teardownId = teardown.teardownId;
    ack.sceneStopped = sceneStopped;
    ack.uploadsAborted = uploadsAborted;
    ack.cacheQuarantined = cacheResult.acknowledgementSafe();
    ack.removedFileCount = removedFileCount;
    ack.errorCode = cacheResult.errorCode;
    ack.quarantinedBytes = cacheResult.quarantinedBytes;
    const bool committed = ack.sceneStopped && ack.uploadsAborted
        && ack.cacheQuarantined;
    if (committed) {
        m_pendingRendererTeardowns.erase(pending);
        m_pendingRendererTeardownOrder.removeAll(remoteSessionId);
        m_pendingTeardownAcks.insert(remoteSessionId, ack);
        retryPendingTeardownAcks();
    } else {
        // A cleanup_error is useful to the server for observability and retry
        // scheduling, but it is not an idempotent terminal result. Keep the
        // exact transaction queued and never place it in the replay table.
        pending->rendererTeardownStarted = false;
        pending->nextRetryAtMs = MouffetteClock::nowMs()
            + AppConfig::instance().deferredCleanupRetryMs();
        qWarning() << "remote_session_cleanup_failed" << remoteSessionId
                   << "teardown" << teardown.teardownId << "cause" << ack.errorCode;
        if (m_webSocketClient && m_webSocketClient->isConnected()) {
            m_webSocketClient->acknowledgeRemoteSessionTeardown(
                remoteSessionId, ack.teardownId, ack.sceneStopped,
                ack.uploadsAborted, ack.cacheQuarantined,
                ack.removedFileCount, ack.errorCode,
                ack.quarantinedBytes);
        }
        if (m_toastSystem) {
            NotificationRequest notification;
            notification.severity = NotificationSeverity::Error;
            notification.category = QStringLiteral("Remote session cleanup");
            notification.message = QStringLiteral(
                "Remote session cleanup could not be committed; this device remains unavailable until cleanup succeeds.");
            notification.remoteSessionId = remoteSessionId;
            notification.correlationId =
                NotificationCorrelation::teardown(teardown.teardownId);
            notification.terminal = true;
            m_toastSystem->publishNotification(notification);
        }
    }
    if (cleanShutdownBarrier && sceneStopped) {
        finishCleanShutdownIncomingCacheTeardownIfReady();
    }
    if (terminalTransportBarrier && sceneStopped) {
        finishTerminalIncomingCacheCleanupIfReady();
    }
    if (!m_pendingRendererTeardowns.contains(remoteSessionId)
        && !m_terminalRendererPendingSessionIds.contains(remoteSessionId)
        && !m_cleanShutdownRendererPendingSessionIds.contains(
            remoteSessionId)) {
        m_pendingRendererTeardownOrder.removeAll(remoteSessionId);
    }
    // An actual failure is excluded from this immediate drain to avoid a local
    // hot loop. Other queued sessions may progress; this transaction retries
    // on the next server delivery or after another teardown settles.
    startNextPendingRendererTeardown(
        committed ? QString() : remoteSessionId);
}

void ApplicationRuntime::retryPendingTeardownAcks() {
    if (!m_webSocketClient || !m_webSocketClient->isConnected()) return;
    for (auto it = m_pendingTeardownAcks.cbegin();
         it != m_pendingTeardownAcks.cend(); ++it) {
        const PendingTeardownAck& ack = it.value();
        if (!ack.sceneStopped || !ack.uploadsAborted
            || !ack.cacheQuarantined) {
            qCritical() << "Refusing to replay an uncommitted teardown ACK"
                        << it.key();
            continue;
        }
        m_webSocketClient->acknowledgeRemoteSessionTeardown(
            it.key(), ack.teardownId, ack.sceneStopped, ack.uploadsAborted,
            ack.cacheQuarantined, ack.removedFileCount, ack.errorCode,
            ack.quarantinedBytes);
    }
}

bool ApplicationRuntime::matchesCurrentOutgoingSession(
    const QString& targetEndpointId, const QString& remoteSessionId,
    quint64 generation) const
{
    if (targetEndpointId.isEmpty() || remoteSessionId.isEmpty()) return false;
    const QString knownId = m_currentOutgoingSessionIdByTarget.value(targetEndpointId);
    if ((!knownId.isEmpty() && knownId != remoteSessionId)
        || (generation > 0 && knownId == remoteSessionId
            && m_currentOutgoingSessionGenerationByTarget.value(targetEndpointId) > generation)) return false;
    const auto binding = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator()->outgoingForPeer(targetEndpointId)
        : RemoteSessionCoordinator::Binding();
    return binding.remoteSessionId.isEmpty()
        || (binding.remoteSessionId == remoteSessionId
            && (generation == 0 || binding.generation <= generation));
}

void ApplicationRuntime::clearRemoteSessionRuntimeState(
    const QString& targetEndpointId,
    bool teardownPending,
    const QString& expectedSessionId) {
    if (!expectedSessionId.isEmpty()
        && !matchesCurrentOutgoingSession(targetEndpointId, expectedSessionId)) return;
    // Retire the backend before publishing cleared workspace state. The local
    // inactivity deadline is terminal even while the server CLOSE is pending;
    // late transfer/RAM callbacks must not restore its inventory or progress.
    // Resolve the session by target, never through the selected upload in the UI.
    if (m_uploadManager) {
        const auto binding = m_webSocketClient
            ? m_webSocketClient->remoteSessionCoordinator()->outgoingForPeer(targetEndpointId)
            : RemoteSessionCoordinator::Binding();
        const QString remoteSessionId = !expectedSessionId.isEmpty() ? expectedSessionId
            : !binding.remoteSessionId.isEmpty() ? binding.remoteSessionId
                                               : m_currentOutgoingSessionIdByTarget.value(targetEndpointId);
        if (!remoteSessionId.isEmpty()) {
            m_uploadManager->terminateRemoteSessionUpload(
                remoteSessionId, QStringLiteral("session_terminal"));
        }
    }
    ClientWorkspace* session = m_workspaceManager
        ? m_workspaceManager->findWorkspace(targetEndpointId) : nullptr;
    if (session) {
        session->upload.remoteFilesPresent = false;
        if (session->canvas) {
            // Every terminal remote-session path clears only remote playback.
            // Local sources and a running test remain valid after peer loss.
            session->canvas->handleRemoteConnectionLost();
        }
        session->knownRemoteFileIds.clear();
        session->expectedProjectFileIds.clear();
        session->remoteContentClearedOnDisconnect = true;
        if (session->canvas) {
            for (CanvasMedia* media : session->canvas->enumerateMediaItems()) {
                if (media) media->setUploadNotUploaded();
            }
        }
        clearUploadTracking(*session);
        if (m_workspaceManager) {
            m_workspaceManager->setRemoteSessionState(
                targetEndpointId,
                teardownPending
                    ? WorkspaceManager::RemoteSessionState::Closing
                    : WorkspaceManager::RemoteSessionState::Absent);
        }
        if (m_activeWorkspaceEndpointId == targetEndpointId && m_uploadManager) {
            // RemoteSession terminal envelopes invalidate only their own upload.
            // onConnectionLost() is transport-wide and would suspend unrelated
            // concurrent uploads to other devices indefinitely.
            m_uploadManager->setTargetClientId(QString());
        }
        updateWorkspaceCapabilities(targetEndpointId);
        destroyWorkspaceCanvasIfUnused(targetEndpointId);
        if (m_activeWorkspaceEndpointId == targetEndpointId) {
            emit activeWorkspaceChanged(targetEndpointId);
        }
    }

    if (m_activeWorkspaceEndpointId == targetEndpointId) {
        m_remoteClientConnected = false;
        refreshRemoteConnectionPresentation(false);
    }
    // This final projection also updates the list when the Project already
    // removed its runtime workspace before the terminal envelope arrived.
    refreshProjectClientList();
}

void ApplicationRuntime::handleRemoteSessionClosed(const QJsonObject& envelope) {
    if (!m_webSocketClient) return;
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    quint64 generation = 0;
    readSafePositiveJsonInteger(
        envelope.value(QStringLiteral("generation")), &generation);
    if (m_controlledDisconnectInProgress) {
        m_controlledDisconnectPendingSessionIds.remove(remoteSessionId);
        QTimer::singleShot(
            0, this, &ApplicationRuntime::finishControlledDisconnectIfReady);
    }
    const QString ownerEndpointId = envelope.value(QStringLiteral("ownerEndpointId")).toString();
    const QString targetEndpointId = envelope.value(QStringLiteral("targetEndpointId")).toString();
    m_pendingRendererTeardowns.remove(remoteSessionId);
    m_pendingRendererTeardownOrder.removeAll(remoteSessionId);
    m_pendingTeardownAcks.remove(remoteSessionId);
    m_locallyTerminatingRemoteSessions.remove(remoteSessionId);
    if (m_incomingSessionOrphanWatchdog
        && m_incomingSessionOrphanWatchdog->officiallyClosed(
            remoteSessionId)) {
        m_incomingOrphanSessionIds.remove(remoteSessionId);
    }
    if (ownerEndpointId != m_webSocketClient->endpointId()) return;

    const QString knownSessionId =
        m_currentOutgoingSessionIdByTarget.value(targetEndpointId);
    const quint64 knownGeneration =
        m_currentOutgoingSessionGenerationByTarget.value(targetEndpointId, 0);
    if ((!knownSessionId.isEmpty() && knownSessionId != remoteSessionId)
        || (knownSessionId == remoteSessionId
            && knownGeneration > 0 && generation < knownGeneration)) {
        return;
    }

    bool matchedPendingClose = false;
    const auto pending =
        m_pendingOutgoingSessionCloses.constFind(targetEndpointId);
    if (pending != m_pendingOutgoingSessionCloses.cend()
        && pending->remoteSessionId == remoteSessionId
        && generation >= pending->generation) {
        matchedPendingClose = true;
    }

    // A delayed Closed from an older session must never erase a replacement
    // that is already Opening/Active for the same peer.
    RemoteSessionCoordinator* coordinator =
        m_webSocketClient->remoteSessionCoordinator();
    const RemoteSessionCoordinator::Binding currentBinding = coordinator
        ? coordinator->outgoingForPeer(targetEndpointId)
        : RemoteSessionCoordinator::Binding();
    if (!currentBinding.remoteSessionId.isEmpty()
        && currentBinding.remoteSessionId != remoteSessionId) {
        return;
    }
    if (matchedPendingClose) {
        finalizePendingOutgoingSessionClose(
            targetEndpointId, remoteSessionId, generation, true);
        return;
    }
    if (knownSessionId == remoteSessionId) {
        m_currentOutgoingSessionIdByTarget.remove(targetEndpointId);
        m_currentOutgoingSessionGenerationByTarget.remove(targetEndpointId);
    }
    for (auto it = m_outgoingSessionCloseRequestById.begin();
         it != m_outgoingSessionCloseRequestById.end();) {
        if (it->targetEndpointId == targetEndpointId
            && it->remoteSessionId == remoteSessionId) {
            it = m_outgoingSessionCloseRequestById.erase(it);
        } else {
            ++it;
        }
    }
    if (m_cancelledInitialOpenSessionByTarget.value(targetEndpointId)
        == remoteSessionId) {
        m_cancelledInitialOpenSessionByTarget.remove(targetEndpointId);
    }
    for (auto it = m_cancelledInitialOpenTargetByRequestId.begin();
         it != m_cancelledInitialOpenTargetByRequestId.end();) {
        if (it.value() == targetEndpointId) {
            it = m_cancelledInitialOpenTargetByRequestId.erase(it);
        } else {
            ++it;
        }
    }

    m_remoteSessionOpenPendingTargets.remove(targetEndpointId);
    for (auto it = m_remoteSessionOpenTargetByRequestId.begin();
         it != m_remoteSessionOpenTargetByRequestId.end();) {
        if (it.value() == targetEndpointId) {
            m_automaticRemoteSessionOpenRequestIds.remove(it.key());
            if (m_webSocketClient) m_webSocketClient->cancelRemoteSessionOpen(it.key());
            it = m_remoteSessionOpenTargetByRequestId.erase(it);
        } else {
            ++it;
        }
    }
    clearRemoteSessionRuntimeState(targetEndpointId, false);
    // A terminal session cannot be resumed. Reconcile the selected Canvas
    // immediately as well as on discovery: either event may arrive first.
    m_remoteSessionOpenDesiredTargets.remove(targetEndpointId);
    m_remoteSessionOpenSuppressedTargets.insert(targetEndpointId);
    if (targetEndpointId == m_activeWorkspaceEndpointId) {
        reconcileForegroundRemoteSession();
    }
}

void ApplicationRuntime::handleRemoteSessionError(const QJsonObject& envelope) {
    if (m_cleanShutdownPrepared) {
        // A duplicate/late close rejection cannot change a terminal local
        // shutdown. Keep draining any other correlated sessions until timeout.
        return;
    }
    const QString code = envelope.value(QStringLiteral("code")).toString();
    const QString requestId = envelope.value(QStringLiteral("requestId")).toString();
    const QString envelopeSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    if (code == QLatin1String("cleanup_not_committed")
        && (m_pendingRendererTeardowns.contains(envelopeSessionId)
            || m_pendingTeardownAcks.contains(envelopeSessionId))) {
        // The local cleanup path already publishes one descriptive, correlated
        // notification. Older servers echo our own failure as a raw error.
        return;
    }
    const auto publishError = [this, &envelope]() {
        if (!m_toastSystem) return;
        NotificationRequest notification;
        notification.severity = NotificationSeverity::Error;
        notification.category = QStringLiteral("Remote session");
        notification.message = envelope.value(QStringLiteral("message"))
            .toString(QStringLiteral(
                "The remote session command was rejected."));
        const QString code = envelope.value(QStringLiteral("code")).toString();
        if (notification.message.trimmed().isEmpty() || notification.message == code
            || QRegularExpression(QStringLiteral("^[a-z][a-z0-9_]*$")).match(notification.message).hasMatch()) {
            if (code == QLatin1String("cleanup_not_committed") || code == QLatin1String("session_cleanup_pending"))
                notification.message = QStringLiteral("The previous remote session is still being cleaned up. Please wait before trying again.");
            else if (code == QLatin1String("target_offline") || code == QLatin1String("target_unavailable"))
                notification.message = QStringLiteral("The remote computer is unavailable. Check its connection and try again.");
            else if (code == QLatin1String("remote_session_reconnecting") || code == QLatin1String("target_reconnecting")
                     || code == QLatin1String("resume_required") || code == QLatin1String("session_requires_resume"))
                notification.message = QStringLiteral("The remote connection is recovering. Please wait before trying again.");
            else if (code == QLatin1String("lease_expired") || code == QLatin1String("session_terminal")
                     || code == QLatin1String("unknown_remote_session"))
                notification.message = QStringLiteral("The remote session has ended. Reconnect to the remote computer and try again.");
            else
                notification.message = QStringLiteral("The remote session could not complete the request. Please reconnect and try again.");
        }
        notification.remoteSessionId = envelope.value(QStringLiteral("remoteSessionId")).toString();
        QString identity = envelope.value(QStringLiteral("requestId")).toString();
        if (identity.isEmpty() || code == QLatin1String("cleanup_not_committed"))
            identity = notification.remoteSessionId;
        if (!identity.isEmpty() && !code.isEmpty()) {
            notification.correlationId = QStringLiteral("remote-session-error:%1:%2").arg(identity, code);
            notification.terminal = true;
        }
        m_toastSystem->publishNotification(notification);
    };

    // WebSocketClient installs an authenticated Active binding before it
    // validates the initial snapshot.  If that second step fails, it emits
    // this local error after sending a fail-closed CLOSE.  Fence the exact
    // bound session here: otherwise a click before Closed can reuse the still
    // Active coordinator binding and expose an empty, command-capable canvas.
    //
    // Do not infer the target from the error alone.  The OPEN request,
    // session identity, generation and both authenticated parties must all
    // describe the same currently-bound transaction.
    if (code == QLatin1String("invalid_initial_snapshot")
        && !requestId.isEmpty() && !envelopeSessionId.isEmpty()
        && m_webSocketClient) {
        const auto openRequest =
            m_remoteSessionOpenTargetByRequestId.constFind(requestId);
        quint64 errorGeneration = 0;
        const QString errorOwnerEndpointId =
            envelope.value(QStringLiteral("ownerEndpointId")).toString();
        const QString errorTargetEndpointId =
            envelope.value(QStringLiteral("targetEndpointId")).toString();
        RemoteSessionCoordinator* coordinator =
            m_webSocketClient->remoteSessionCoordinator();
        const RemoteSessionCoordinator::Binding binding = coordinator
            ? coordinator->byId(envelopeSessionId)
            : RemoteSessionCoordinator::Binding();
        const bool exactInvalidSnapshot =
            openRequest != m_remoteSessionOpenTargetByRequestId.cend()
            && readSafePositiveJsonInteger(
                envelope.value(QStringLiteral("generation")),
                &errorGeneration)
            && errorOwnerEndpointId == m_webSocketClient->endpointId()
            && errorTargetEndpointId == openRequest.value()
            && (envelope.value(QStringLiteral("identityValid")).toBool()
                || (binding.remoteSessionId == envelopeSessionId
                    && binding.ownerEndpointId == errorOwnerEndpointId
                    && binding.targetEndpointId == errorTargetEndpointId
                    && binding.generation == errorGeneration
                    && binding.phase == QLatin1String("Active")));
        if (exactInvalidSnapshot) {
            rememberPendingOutgoingSessionClose(
                errorTargetEndpointId, envelopeSessionId, errorGeneration);
            m_locallyTerminatingRemoteSessions.insert(envelopeSessionId);
            m_remoteSessionOpenSuppressedTargets.insert(errorTargetEndpointId);
            m_remoteSessionAutoOpenBlockedTargets.insert(errorTargetEndpointId);
            retryPendingOutgoingSessionClose(
                errorTargetEndpointId,
                QStringLiteral("invalid_initial_snapshot"));
        }
    }

    // CLOSE and OPEN share the remote_session error channel. Correlate CLOSE
    // first so its rejection can never be misapplied to an unrelated device's
    // sole in-flight OPEN.
    auto closeRequest = m_outgoingSessionCloseRequestById.find(requestId);
    if (!requestId.isEmpty()
        && closeRequest != m_outgoingSessionCloseRequestById.end()) {
        const OutgoingSessionCloseRequest request = closeRequest.value();
        m_outgoingSessionCloseRequestById.erase(closeRequest);
        if (!envelopeSessionId.isEmpty()
            && envelopeSessionId != request.remoteSessionId) {
            qWarning() << "Ignored mismatched RemoteSession CLOSE error"
                       << requestId << envelopeSessionId;
            publishError();
            return;
        }
        const auto pending =
            m_pendingOutgoingSessionCloses.constFind(request.targetEndpointId);
        if (pending == m_pendingOutgoingSessionCloses.cend()
            || pending->remoteSessionId != request.remoteSessionId
            || pending->generation != request.generation) {
            return; // Delayed rejection for an already finalized close.
        }

        const bool exactIdentityNoLongerControllable =
            code == QLatin1String("unknown_remote_session")
            || code == QLatin1String("not_a_session_party")
            || code == QLatin1String("invalid_resume_proof");
        if (exactIdentityNoLongerControllable) {
            // The authenticated server has proven that this exact identity is
            // no longer controllable by this runtime. Drop the stale local
            // binding and honor any queued explicit replacement selection. If
            // the old server-side pair is still converging, the correlated
            // OPEN error below retains that intent until a later client-list
            // update provides the next retry boundary.
            finalizePendingOutgoingSessionClose(
                request.targetEndpointId, request.remoteSessionId,
                request.generation, true);
            return;
        }

        // A generation/transport race is not proof of absence. Keep Closing
        // monotonic; a resumed lease or a later authenticated transport will
        // supply a new retry boundary.
        clearRemoteSessionRuntimeState(request.targetEndpointId, true);
        refreshProjectClientList();
        auto pendingClose = m_pendingOutgoingSessionCloses.find(request.targetEndpointId);
        if (pendingClose != m_pendingOutgoingSessionCloses.end()) pendingClose->closeDispatchedOnConnectionGeneration = 0;
        m_sessionRecovery.retry(request.targetEndpointId, code);
        qWarning() << "RemoteSession CLOSE remains pending after rejection"
                   << code << request.remoteSessionId;
        return;
    }

    // Errors for an OPEN which the user explicitly cancelled are terminal for
    // that exact attempt only. They must not cancel a later OPEN for the same
    // endpoint.
    auto cancelledOpen =
        m_cancelledInitialOpenTargetByRequestId.find(requestId);
    if (!requestId.isEmpty()
        && cancelledOpen != m_cancelledInitialOpenTargetByRequestId.end()) {
        const QString cancelledTarget = cancelledOpen.value();
        m_cancelledInitialOpenTargetByRequestId.erase(cancelledOpen);
        if (!envelopeSessionId.isEmpty()
            && m_cancelledInitialOpenSessionByTarget.value(cancelledTarget)
                == envelopeSessionId) {
            m_cancelledInitialOpenSessionByTarget.remove(cancelledTarget);
        }
        RemoteSessionCoordinator* coordinator = m_webSocketClient
            ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
        const RemoteSessionCoordinator::Binding cancelledBinding = coordinator
            ? coordinator->outgoingForPeer(cancelledTarget)
            : RemoteSessionCoordinator::Binding();
        if (!cancelledBinding.remoteSessionId.isEmpty()
            && (envelopeSessionId.isEmpty()
                || cancelledBinding.remoteSessionId == envelopeSessionId)) {
            // The server rejected the OPEN after its Opening binding had
            // already reached us. Keep that exact session fenced until the
            // correlated terminal result; otherwise a queued user click could
            // attach itself to the cancelled Opening and be lost.
            rememberPendingOutgoingSessionClose(
                cancelledTarget, cancelledBinding.remoteSessionId,
                cancelledBinding.generation);
            m_locallyTerminatingRemoteSessions.insert(
                cancelledBinding.remoteSessionId);
            retryPendingOutgoingSessionClose(
                cancelledTarget, QStringLiteral("cancelled_open_rejected"));
        }
        if (!hasCancelledInitialOpenForTarget(cancelledTarget)
            && !hasPendingOutgoingSessionClose(cancelledTarget)
            && m_remoteSessionOpenDesiredTargets.contains(cancelledTarget)) {
            for (const ClientInfo& discovered :
                 std::as_const(m_discoveredClients)) {
                if (discovered.endpointId() == cancelledTarget
                    && discovered.isOnline()) {
                    ensureRemoteSessionForClient(discovered);
                    break;
                }
            }
        }
        return;
    }

    // All remaining remote-session errors (accept, resume, scene, upload, ...)
    // are presentation-only unless they correlate to an exact pending OPEN.
    // Target-only or "sole pending target" guesses are deliberately forbidden.
    auto openRequest = m_remoteSessionOpenTargetByRequestId.find(requestId);
    if (requestId.isEmpty()
        || openRequest == m_remoteSessionOpenTargetByRequestId.end()) {
        publishError();
        return;
    }
    const QString targetEndpointId = openRequest.value();
    m_remoteSessionOpenTargetByRequestId.erase(openRequest);
    const bool automaticAttempt =
        m_automaticRemoteSessionOpenRequestIds.remove(requestId);
    const bool retryAfterServerConvergence =
        code == QLatin1String("session_cleanup_pending")
        || code == QLatin1String("session_requires_resume")
        || code == QLatin1String("session_runtime_conflict")
        || code == QLatin1String("target_offline")
        || code == QLatin1String("target_unavailable")
        || code == QLatin1String("target_reconnecting")
        || code == QLatin1String("remote_session_reconnecting")
        || code == QLatin1String("remote_session_open_timeout");
    if (retryAfterServerConvergence) {
        m_sessionRecovery.retry(targetEndpointId, code);
    }
    const bool retainExplicitOpenIntent = !automaticAttempt
        && retryAfterServerConvergence
        && !targetEndpointId.isEmpty()
        && m_activeWorkspaceEndpointId == targetEndpointId
        && m_navigationManager
        && m_navigationManager->isOnScreenView();
    if (retainExplicitOpenIntent) {
        // OPEN consumes the one-shot desired flag at dispatch time. These
        // errors describe an older server-side session which has not reached
        // its terminal commit yet, not a rejection of the user's selection.
        // Retain the intent; the foreground scheduler retries with bounded
        // backoff, while changed authenticated presence can retry sooner.
        m_remoteSessionOpenDesiredTargets.insert(targetEndpointId);
        m_remoteSessionOpenSuppressedTargets.remove(targetEndpointId);
    }
    // Automatic retries remain derived from current activity; never turn them
    // into a sticky explicit selection. A permanent/validation rejection must
    // also not produce an OPEN/CLOSE loop on each discovery or terminal event.
    if (!retryAfterServerConvergence
        && code != QLatin1String("target_offline")
        && code != QLatin1String("remote_session_open_timeout")) {
        m_remoteSessionAutoOpenBlockedTargets.insert(targetEndpointId);
        m_sessionRecovery.block(targetEndpointId, code);
    }
    const bool waitingForConvergence = retryAfterServerConvergence
        && (retainExplicitOpenIntent
            || wantsForegroundRemoteSession(targetEndpointId));
    const bool initialAttempt = !targetEndpointId.isEmpty()
        && (!m_projectManager
            || !m_projectManager->hasProjectForTarget(targetEndpointId));
    const bool closePending = !targetEndpointId.isEmpty()
        && hasPendingOutgoingSessionClose(targetEndpointId);
    if (!targetEndpointId.isEmpty()) {
        m_remoteSessionOpenPendingTargets.remove(targetEndpointId);
        if (m_workspaceManager) {
            m_workspaceManager->setRemoteSessionState(
                targetEndpointId,
                closePending
                    ? WorkspaceManager::RemoteSessionState::Closing
                    : WorkspaceManager::RemoteSessionState::Absent);
            updateWorkspaceCapabilities(targetEndpointId);
            destroyWorkspaceCanvasIfUnused(targetEndpointId);
        }
    }

    if (!targetEndpointId.isEmpty()) {
        refreshProjectClientList();
        if (m_activeWorkspaceEndpointId == targetEndpointId) {
            refreshRemoteConnectionPresentation(false);
            if (m_activeCanvas) m_activeCanvas->setOverlayActionsEnabled(false);

            if (waitingForConvergence) {
                qInfo() << "RemoteSession OPEN waits for server convergence"
                        << code << targetEndpointId;
                return;
            }

            // A rejected open is terminal for this attempt.  Do not leave an
            // indeterminate spinner running forever; reveal the known local
            // project/topology with remote actions disabled.
            if (!initialAttempt && m_navigationManager) {
                m_navigationManager->revealCanvas();
            }
        }
    }
    if (waitingForConvergence) return;
    publishError();
    if (initialAttempt && m_navigationManager
        && m_navigationManager->isOnScreenView()) {
        showClientListView();
        if (!targetEndpointId.isEmpty()) removeRuntimeWorkspace(targetEndpointId);
    }
}

void ApplicationRuntime::terminateProjectRemoteSession(const QString& targetEndpointId,
                                               bool attemptRemote, const QString& reason) {
    ClientWorkspace* session = m_workspaceManager
        ? m_workspaceManager->findWorkspace(targetEndpointId) : nullptr;
    if (session && session->canvas) {
        // Closing the wire session below also stops its remote SceneRun.
        // An inactivity timeout does not invalidate the local test's sources.
        session->canvas->handleRemoteConnectionLost();
    }

    RemoteSessionCoordinator* coordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    const RemoteSessionCoordinator::Binding binding = coordinator
        ? coordinator->outgoingForPeer(targetEndpointId)
        : RemoteSessionCoordinator::Binding();
    if (attemptRemote && m_webSocketClient
        && !binding.remoteSessionId.isEmpty()
        && binding.ownerEndpointId == m_webSocketClient->endpointId()
        && binding.phase != QLatin1String("Closed")) {
        rememberPendingOutgoingSessionClose(
            targetEndpointId, binding.remoteSessionId, binding.generation);
        m_locallyTerminatingRemoteSessions.insert(binding.remoteSessionId);
        retryPendingOutgoingSessionClose(
            targetEndpointId, reason);
    }

    // Runtime content becomes unavailable synchronously, while Closing stays
    // monotonic until the correlated Closed envelope removes the wire fence.
    clearRemoteSessionRuntimeState(targetEndpointId, hasPendingOutgoingSessionClose(targetEndpointId));
}

void ApplicationRuntime::removeRuntimeWorkspace(const QString& targetEndpointId) {
    if (!m_workspaceManager) {
        return;
    }
    ClientWorkspace* session = m_workspaceManager->findWorkspace(targetEndpointId);
    if (!session) {
        m_restoredProjectIds.remove(targetEndpointId);
        return;
    }

    ICanvasHost* canvas = session->canvas;
    const QString projectId = session->projectId;
    if (canvas) {
        for (CanvasMedia* media : canvas->enumerateMediaItems()) {
            if (m_fileWatcher && media) {
                m_fileWatcher->unwatchMediaItem(media);
            }
        }
    }
    clearUploadTracking(*session);
    if (m_fileManager && !projectId.isEmpty()) {
        m_fileManager->removeProjectAssociations(projectId);
    }
    if (m_activeWorkspaceEndpointId == targetEndpointId) {
        m_activeWorkspaceEndpointId.clear();
        m_activeCanvas = nullptr;
        m_selectedClient = ClientInfo();
        m_remoteDisplayName.clear();
        m_remoteClientConnected = false;
        m_remoteOverlayActionsEnabled = false;
        m_remoteVolumePercent = -1;
        m_canvasRevealedForCurrentClient = false;
        m_canvasContentEverLoaded = false;
        if (m_navigationManager) m_navigationManager->setActiveCanvas(nullptr);
        if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
    }
    m_workspaceManager->deleteWorkspace(targetEndpointId);
    if (canvas) canvas->deleteLater();
    m_restoredProjectIds.remove(targetEndpointId);
}

void ApplicationRuntime::onDeleteProjectRequested() {
    deleteActiveProjectConfirmed();
}

void ApplicationRuntime::deleteActiveProjectConfirmed() {
    const QString targetEndpointId = m_activeWorkspaceEndpointId;
    if (!m_projectManager || targetEndpointId.isEmpty()
        || !activeProjectExists()) {
        return;
    }
    persistProjectCanvas(targetEndpointId);
    // ProjectManager emits projectDeleted only after its durable store commit.
    // That signal owns session teardown, so a failed delete cannot
    // irreversibly close an otherwise intact project/session first.
    if (!m_projectManager->deleteProject(targetEndpointId)) {
        TOAST_ERROR(QStringLiteral("The project could not be deleted"),
                    AppConfig::instance().toastErrorDurationMs());
        return;
    }
}

void ApplicationRuntime::clearDeletedProjectFromWorkspace(
    const QString& targetEndpointId)
{
    ClientWorkspace* workspace = m_workspaceManager
        ? m_workspaceManager->findWorkspace(targetEndpointId) : nullptr;
    if (!workspace) {
        m_restoredProjectIds.remove(targetEndpointId);
        return;
    }

    const QString projectId = workspace->projectId;
    if (workspace->canvas) {
        workspace->canvas->stopScenesForSourceInvalidation();
        const QList<CanvasMedia*> media = workspace->canvas->enumerateMediaItems();
        for (CanvasMedia* item : media) {
            if (m_fileWatcher && item) m_fileWatcher->unwatchMediaItem(item);
        }
        if (workspace->canvas->document()) workspace->canvas->document()->clear();
    }
    clearUploadTracking(*workspace);
    if (m_fileManager && !projectId.isEmpty()) {
        m_fileManager->removeProjectAssociations(projectId);
    }

    m_restoredProjectIds.remove(targetEndpointId);
    removeRuntimeWorkspace(targetEndpointId);
    emit activeWorkspaceChanged(QString());
    emit presentationStateChanged();
}

void ApplicationRuntime::destroyWorkspaceCanvasIfUnused(
    const QString& targetEndpointId)
{
    if (!m_workspaceManager) return;
    ClientWorkspace* workspace = m_workspaceManager->findWorkspace(targetEndpointId);
    if (!workspace || !workspace->canvas) return;
    const bool hasProject = m_projectManager
        && m_projectManager->hasProjectForTarget(targetEndpointId);
    const WorkspaceManager::RemoteSessionState state =
        m_workspaceManager->remoteSessionState(targetEndpointId);
    const bool hasUsableRemoteSession =
        state == WorkspaceManager::RemoteSessionState::Opening
        || state == WorkspaceManager::RemoteSessionState::Active
        || state == WorkspaceManager::RemoteSessionState::Grace;
    if (hasProject || hasUsableRemoteSession) return;

    ICanvasHost* canvas = workspace->canvas;
    workspace->canvas = nullptr;
    workspace->connectionsInitialized = false;
    if (m_activeWorkspaceEndpointId == targetEndpointId) {
        m_activeCanvas = nullptr;
        if (m_navigationManager) m_navigationManager->setActiveCanvas(nullptr);
        if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
    }
    canvas->deleteLater();
}

void ApplicationRuntime::updateWorkspaceCapabilities(
    const QString& targetEndpointId)
{
    ClientWorkspace* workspace = m_workspaceManager
        ? m_workspaceManager->findWorkspace(targetEndpointId) : nullptr;
    if (!workspace || !workspace->canvas) return;
    const bool hasProject = m_projectManager
        && m_projectManager->hasProjectForTarget(targetEndpointId);
    const auto binding = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator()->outgoingForPeer(targetEndpointId)
        : RemoteSessionCoordinator::Binding();
    const bool remoteActive = workspace->remoteSessionState
        == WorkspaceManager::RemoteSessionState::Active
        && isCommandReadyBinding(m_webSocketClient, binding);
    workspace->canvas->setProjectEditingEnabled(hasProject);
    workspace->canvas->setOverlayActionsEnabled(remoteActive);
    if (m_activeWorkspaceEndpointId == targetEndpointId && m_uploadManager) {
        m_uploadManager->setTargetClientId(
            hasProject && remoteActive ? targetEndpointId : QString());
        emit m_uploadManager->uiStateChanged();
    }
}

void ApplicationRuntime::setApplicationSuspended(bool suspended) {
    if (m_applicationSuspended == suspended) {
        updateHistoryVisibilityState();
        return;
    }
    m_applicationSuspended = suspended;
    refreshMediaSharing();
    if (m_activityMonitor) m_activityMonitor->setSystemSuspended(suspended);
    updateHistoryVisibilityState();
}

void ApplicationRuntime::setSelectedClient(const ClientInfo& client)
{
    m_selectedClient = client;
    if (client.endpointId() == m_selectionEndpointId) m_selectionClient = client;
    updateClientNameDisplay(client);
}

void ApplicationRuntime::updateVolumeIndicator()
{
    m_remoteVolumePercent = m_selectedClient.getVolumePercent();
    emit presentationStateChanged();
}


void ApplicationRuntime::onUploadButtonClicked()
{
    if (!activeProjectExists() || !m_workspaceManager
        || m_workspaceManager->remoteSessionState(m_activeWorkspaceEndpointId)
            != WorkspaceManager::RemoteSessionState::Active) {
        return;
    }
    if (m_uploadEventHandler) m_uploadEventHandler->onUploadButtonClicked();
    if (m_uploadManager) emit m_uploadManager->uiStateChanged();
}

void ApplicationRuntime::onBackToClientListClicked()
{
    showClientListView();
}

void ApplicationRuntime::showHistoryPage()
{
    if (m_navigationManager && m_navigationManager->isOnScreenView()) {
        showClientListView();
    }
    m_applicationPage = 2;
    updateHistoryVisibilityState();
    emit applicationPageChanged(2);
    emit presentationStateChanged();
}

void ApplicationRuntime::updateHistoryVisibilityState()
{
    if (!m_toastSystem || !m_toastSystem->notificationCenter()) return;
    m_toastSystem->notificationCenter()->setHistoryVisible(
        m_applicationPage == 2 && m_qmlWindowVisible
        && !m_applicationSuspended);
}

void ApplicationRuntime::onClientSelected(const ClientInfo& client,
                                          int clientIndex)
{
    Q_UNUSED(clientIndex);
    showScreenView(client);
}

void ApplicationRuntime::onOngoingSceneSelected(const QString& sceneRunId)
{
    if (!m_sceneActivityModel) return;
    const SceneActivityModel::Activity activity =
        m_sceneActivityModel->activity(sceneRunId);
    if (activity.sceneRunId.isEmpty()
        || activity.direction != SceneActivityModel::Direction::Outgoing) {
        return;
    }
    if (ClientWorkspace* session = findWorkspace(activity.peerEndpointId)) {
        showScreenView(session->lastClientInfo);
        return;
    }
    for (const ClientInfo& client : std::as_const(m_discoveredClients)) {
        if (client.endpointId() == activity.peerEndpointId) {
            showScreenView(client);
            return;
        }
    }
    if (m_projectManager) {
        if (const ProjectRecord* project =
                m_projectManager->projectForTarget(activity.peerEndpointId)) {
            ClientInfo stored = project->target.toClientInfo(false);
            stored.setScreens(project->savedScreens);
            showScreenView(stored);
        }
    }
}

NotificationCenter* ApplicationRuntime::getNotificationCenter() const
{
    return m_toastSystem ? m_toastSystem->notificationCenter() : nullptr;
}

QString ApplicationRuntime::remoteDisplayName() const
{
    if (m_selectedClient.endpointId().isEmpty() && m_remoteDisplayName.isEmpty()) return {};
    return m_remoteDisplayName.isEmpty()
        ? m_selectedClient.getInstanceDisplayName() : m_remoteDisplayName;
}

bool ApplicationRuntime::canDeleteActiveProject() const
{
    return m_projectManager && !m_activeWorkspaceEndpointId.isEmpty()
        && m_projectManager->hasProjectForTarget(m_activeWorkspaceEndpointId);
}

bool ApplicationRuntime::activeProjectExists() const
{
    return m_projectManager && !m_activeWorkspaceEndpointId.isEmpty()
        && m_projectManager->hasProjectForTarget(m_activeWorkspaceEndpointId);
}

bool ApplicationRuntime::activeRemoteSessionExists() const
{
    if (!m_workspaceManager || m_activeWorkspaceEndpointId.isEmpty()) return false;
    const WorkspaceManager::RemoteSessionState state =
        m_workspaceManager->remoteSessionState(m_activeWorkspaceEndpointId);
    return state == WorkspaceManager::RemoteSessionState::Opening
        || state == WorkspaceManager::RemoteSessionState::Active
        || state == WorkspaceManager::RemoteSessionState::Grace;
}

void ApplicationRuntime::setQmlWindowVisible(bool visible)
{
    if (m_qmlWindowVisible == visible) return;
    m_qmlWindowVisible = visible;
    refreshMediaSharing();
    if (m_activityMonitor) m_activityMonitor->setControlWindowVisible(visible);
    updateHistoryVisibilityState();
}

void ApplicationRuntime::setPointerInsideControlWindow(bool inside)
{
    if (m_activityMonitor) m_activityMonitor->setPointerInside(inside);
}

void ApplicationRuntime::activateClient(const QString& endpointId)
{
    for (const ClientInfo& client : std::as_const(m_displayClients)) {
        const QString id = client.endpointId().isEmpty()
            ? client.getId() : client.endpointId();
        if (id == endpointId) {
            showScreenView(client);
            return;
        }
    }
}

void ApplicationRuntime::activateOngoingScene(const QString& sceneRunId)
{
    onOngoingSceneSelected(sceneRunId);
}

void ApplicationRuntime::navigateToClients()
{
    showClientListView();
}

void ApplicationRuntime::navigateToHistory()
{
    showHistoryPage();
}

void ApplicationRuntime::toggleConnectionEnabled()
{
    onEnableDisableClicked();
}

ApplicationRuntime::~ApplicationRuntime()
{
    prepareCleanShutdown();
    finishCleanShutdown();
}

void ApplicationRuntime::prepareCleanShutdown()
{
    if (m_cleanShutdownPrepared) return;
    m_cleanShutdownPrepared = true;
    if (m_screenSharing) m_screenSharing->stop();
    if (m_screenPreviewStore) m_screenPreviewStore->waitForDone();
    if (m_audioSharing) m_audioSharing->stop();

    if (m_webSocketClient && m_webSocketMessageHandler) {
        QObject::disconnect(m_webSocketClient, nullptr,
                            m_webSocketMessageHandler, nullptr);
    }
    if (m_workspaceManager && m_projectManager) {
        for (ClientWorkspace* session : m_workspaceManager->allWorkspaces()) {
            if (!session) continue;
            if (session->canvas) session->canvas->stopScenesForSourceInvalidation();
            if (m_projectManager->hasProjectForTarget(session->targetEndpointId)) {
                persistProjectCanvas(session->targetEndpointId);
            }
        }
        m_projectManager->markAllHidden(MouffetteClock::anchoredEpochMs());
        m_projectManager->flush();
    }

    const QString localEndpointId = m_webSocketClient
        ? m_webSocketClient->endpointId() : QString();
    RemoteSessionCoordinator* coordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    const auto bindings = coordinator ? coordinator->all()
                                      : QList<RemoteSessionCoordinator::Binding>();
    if (m_uploadManager) {
        // Stop incoming writers and fail-close advertisement immediately.
        // Cache deletion remains behind renderer destruction. If the event
        // loop ends first, startup recovery owns the quarantined namespace.
        m_uploadManager->beginTerminalIncomingCleanup(
            QStringLiteral("clean_shutdown"), {});
    }
    for (const auto& binding : bindings) {
        if (binding.remoteSessionId.isEmpty()) continue;
        if (m_webSocketClient && m_webSocketClient->isConnected()) {
            m_webSocketClient->closeRemoteSession(
                binding.remoteSessionId, nullptr,
                QStringLiteral("clean_shutdown"));
        }
        if (binding.targetEndpointId == localEndpointId
            && m_remoteSceneController) {
            m_cleanShutdownRendererPendingSessionIds.insert(
                binding.remoteSessionId);
            enqueueRendererTeardown(binding.remoteSessionId);
        }
    }
    finishCleanShutdownIncomingCacheTeardownIfReady();
    if (m_sceneActivityModel) m_sceneActivityModel->clear();
}

void ApplicationRuntime::finishCleanShutdownIncomingCacheTeardownIfReady()
{
    if (m_cleanShutdownIncomingCacheTeardownStarted
        || !m_cleanShutdownRendererPendingSessionIds.isEmpty()
        || !m_uploadManager
        || !m_uploadManager->incomingFileReadersSettled()) {
        return;
    }
    m_cleanShutdownIncomingCacheTeardownStarted = true;
    const UploadManager::BulkTeardownResult cleanup =
        m_uploadManager->completeTerminalIncomingCleanup(
            QStringLiteral("clean_shutdown"), {});
    if (cleanup.pendingScopes > 0) {
        m_cleanShutdownIncomingCacheTeardownStarted = false;
        return;
    }
    if (!cleanup.allLogicallyCommitted()) {
        qCritical() << "Clean shutdown cache cleanup remains quarantined:"
                    << cleanup.errorCode
                    << "failed scopes" << cleanup.cleanupErrorScopes;
    }
    if (m_quitDeferredForReaders) {
        finishCleanShutdown();
        QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
    }
}

bool ApplicationRuntime::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == QCoreApplication::instance() && event->type() == QEvent::Quit
        && !m_cleanShutdownIncomingCacheTeardownStarted) {
        m_quitDeferredForReaders = true;
        prepareCleanShutdown();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

void ApplicationRuntime::finishCleanShutdown()
{
    if (m_cleanShutdownFinished || (m_quitDeferredForReaders
        && !m_cleanShutdownIncomingCacheTeardownStarted)) return;
    m_cleanShutdownFinished = true;
    if (m_connectionManager) m_connectionManager->disconnect();
    else if (m_webSocketClient) m_webSocketClient->disconnect();
}

void ApplicationRuntime::handleApplicationAboutToQuit()
{
    prepareCleanShutdown();
    finishCleanShutdown();
}

void ApplicationRuntime::onTrayIconActivated(int reason) {
    switch (static_cast<SystemTrayManager::ActivationReason>(reason)) {
    case SystemTrayManager::ActivationReason::Trigger:
    case SystemTrayManager::ActivationReason::DoubleClick:
    case SystemTrayManager::ActivationReason::Context:
        emit qmlRaiseRequested();
        break;
    default:
        break;
    }
}

bool ApplicationRuntime::isUserDisconnected() const
{
    return !m_connectionManager || !m_connectionManager->connectionEnabled();
}

void ApplicationRuntime::setConnectionEnabled(bool enabled)
{
    if (!m_connectionManager || m_cleanShutdownPrepared) return;
    m_connectionManager->setConnectionEnabled(enabled);
    emit presentationStateChanged();
}

void ApplicationRuntime::onEnableDisableClicked()
{
    setConnectionEnabled(isUserDisconnected());
}

void ApplicationRuntime::beginControlledDisconnect(quint64 transitionId)
{
    if (m_controlledDisconnectInProgress) return;
    m_sessionRecovery.suspend();
    m_controlledDisconnectInProgress = true;
    m_controlledDisconnectTransition = transitionId;
    m_intentionalTransportClose = true;
    refreshOverlayActionsState(false, false);
    m_controlledDisconnectAcknowledged = false;
    m_controlledDisconnectPendingSessionIds.clear();

    RemoteSessionCoordinator* coordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    if (coordinator) {
        for (const RemoteSessionCoordinator::Binding& binding :
             coordinator->all()) {
            if (!binding.remoteSessionId.isEmpty()
                && binding.phase != QLatin1String("Closed")) {
                m_controlledDisconnectPendingSessionIds.insert(
                    binding.remoteSessionId);
                m_locallyTerminatingRemoteSessions.insert(
                    binding.remoteSessionId);
            }
        }
    }

    m_controlledDisconnectRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_controlledDisconnectGeneration = m_webSocketClient
        ? m_webSocketClient->connectionGeneration() : 0;
    if (!m_webSocketClient
        || !m_webSocketClient->beginEndpointDisable(m_controlledDisconnectRequestId)) {
        finishControlledDisconnect();
        return;
    }
    QTimer::singleShot(AppConfig::instance().controlledDisconnectDrainTimeoutMs(),
                      this, [this, transitionId]() {
        if (m_controlledDisconnectInProgress
            && m_controlledDisconnectTransition == transitionId)
            finishControlledDisconnect();
    });
}

void ApplicationRuntime::finishControlledDisconnectIfReady()
{
    if (!m_controlledDisconnectInProgress
        || !m_controlledDisconnectAcknowledged
        || !m_controlledDisconnectPendingSessionIds.isEmpty()) {
        return;
    }
    finishControlledDisconnect();
}

void ApplicationRuntime::finishControlledDisconnect()
{
    if (!m_controlledDisconnectInProgress) return;
    m_controlledDisconnectInProgress = false;
    m_controlledDisconnectAcknowledged = false;
    m_controlledDisconnectPendingSessionIds.clear();
    if (m_connectionManager)
        m_connectionManager->completeDisconnect(m_controlledDisconnectTransition);
    else if (m_webSocketClient) m_webSocketClient->disconnect();
}

// Settings dialog: server URL with Save/Cancel

// (Removed stray duplicated code block previously injected)

void ApplicationRuntime::resetAllWorkspaceUploadStates() {
    for (ClientWorkspace* session : m_workspaceManager->allWorkspaces()) {
        if (session->canvas) {
            for (CanvasMedia* media : session->canvas->enumerateMediaItems()) {
                if (!media) {
                    continue;
                }
                if (media->uploadState() == CanvasMedia::UploadState::Uploading) {
                    media->setUploadNotUploaded();
                }
            }
        }
        session->upload.remoteFilesPresent = false;
        clearUploadTracking(*session);
    }
    m_uploadWorkspaceByUploadId.clear();
    m_activeUploadWorkspaceEndpointId.clear();
}

void ApplicationRuntime::onConnected() {
    if (m_webSocketMessageHandler) {
        m_webSocketMessageHandler->onConnected();
    }
}

void ApplicationRuntime::onDisconnected() {
    if (m_webSocketMessageHandler) {
        m_webSocketMessageHandler->onDisconnected();
    }
    if (m_controlledDisconnectInProgress) finishControlledDisconnect();
}

QString ApplicationRuntime::screenSharingStatus() const
{
    return m_screenSharing ? m_screenSharing->status() : QString();
}

QString ApplicationRuntime::audioSharingStatus() const
{
    return m_audioSharing ? m_audioSharing->status() : QString();
}

QString ApplicationRuntime::remoteAudioState() const
{
    return m_audioSharing ? m_audioSharing->state() : QStringLiteral("unavailable");
}

QString ApplicationRuntime::remoteAudioStatus() const
{
    return m_audioSharing ? m_audioSharing->remoteStatus() : QString();
}

QString ApplicationRuntime::remoteScreenState() const
{
    return m_screenSharing ? m_screenSharing->remoteState(m_activeWorkspaceEndpointId)
                           : QStringLiteral("unavailable");
}

QString ApplicationRuntime::remoteScreenStatus() const
{
    return m_screenSharing ? m_screenSharing->remoteStatus(m_activeWorkspaceEndpointId) : QString();
}

bool ApplicationRuntime::remoteScreenAvailable() const
{
    return m_screenSharing && m_screenSharing->isRemoteScreenAvailable(m_activeWorkspaceEndpointId);
}

bool ApplicationRuntime::remoteScreenLoading() const
{
    return m_screenSharing && m_screenSharing->isRemoteScreenLoading(m_activeWorkspaceEndpointId);
}

void ApplicationRuntime::refreshMediaSharing()
{
    if (!m_screenSharing || m_cleanShutdownPrepared) return;
    // Hiding the control application must not stop an authorized publisher.
    // Native lock/sleep suspends both directions, independently of tray state.
    m_screenSharing->setSuspended(m_nativeSystemSuspended
        || QGuiApplication::applicationState() == Qt::ApplicationSuspended);
    if (m_audioSharing) {
        m_audioSharing->setSuspended(m_nativeSystemSuspended
            || QGuiApplication::applicationState() == Qt::ApplicationSuspended);
        m_audioSharing->setViewedEndpoint(m_qmlWindowVisible && m_applicationPage == 1
            && !m_applicationSuspended ? m_activeWorkspaceEndpointId : QString());
    }
    m_screenSharing->setViewedEndpoint(m_qmlWindowVisible && m_applicationPage == 1
        && !m_applicationSuspended && m_settingsManager->getScreenContentVisible()
            ? m_activeWorkspaceEndpointId : QString());
    if (auto* canvas = getActiveCanvas()) {
        connect(canvas, &ICanvasHost::screenPreviewDemandChanged,
                this, &ApplicationRuntime::refreshMediaSharing, Qt::UniqueConnection);
        const auto demand = canvas->screenPreviewDemand();
        if (demand.isArray()) m_screenSharing->setViewedScreens(demand.toArray());
    }
}

void ApplicationRuntime::refreshRemoteCursorStreaming()
{
    const auto* sessions = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    bool publish = false, receive = false;
    if (sessions && m_webSocketClient->isConnected()) {
        for (const auto& binding : sessions->all()) {
            if (!isCommandReadyBinding(m_webSocketClient, binding)) continue;
            publish |= binding.targetEndpointId == m_webSocketClient->endpointId();
            receive |= binding.ownerEndpointId == m_webSocketClient->endpointId();
        }
    }
    if (m_deviceSnapshotTimer) {
        const int interval = publish ? 1000 : 5000;
        if (m_deviceSnapshotTimer->interval() != interval) m_deviceSnapshotTimer->setInterval(interval);
        if (m_webSocketClient->isConnected()) {
            if (!m_deviceSnapshotTimer->isActive()) m_deviceSnapshotTimer->start();
        } else m_deviceSnapshotTimer->stop();
    }
    // Session proof refreshes may arrive faster than cursor expiry ticks.
    // Starting an active QTimer postpones its deadline and could keep a stale
    // cursor visible forever, so only start on an inactive-to-active edge.
    if (publish) {
        if (!m_cursorPublishTimer->isActive()) m_cursorPublishTimer->start();
    } else m_cursorPublishTimer->stop();
    if (receive) {
        if (!m_cursorExpiryTimer->isActive()) m_cursorExpiryTimer->start();
    } else m_cursorExpiryTimer->stop();
    for (auto* workspace : m_workspaceManager->allWorkspaces()) {
        if (!workspace || !workspace->canvas) continue;
        const auto binding = sessions
            ? sessions->outgoingForPeer(workspace->targetEndpointId)
            : RemoteSessionCoordinator::Binding();
        if (!isCommandReadyBinding(m_webSocketClient, binding)) {
            workspace->canvas->hideRemoteCursor();
            m_receivedCursorAtByEndpoint.remove(workspace->targetEndpointId);
        }
    }
}

void ApplicationRuntime::publishLocalCursor()
{
    if (!m_webSocketClient->isConnected()) {
        refreshRemoteCursorStreaming();
        return;
    }
    int screenId = -1;
    QPointF position;
    const bool visible = m_systemMonitor->getLocalCursorPosition(&screenId, &position);
    if (visible) position = QPointF(std::floor(position.x()), std::floor(position.y()));
    const qint64 now = m_cursorClock.elapsed();
    for (const auto& binding : m_webSocketClient->remoteSessionCoordinator()->all()) {
        if (!isCommandReadyBinding(m_webSocketClient, binding)
            || binding.targetEndpointId != m_webSocketClient->endpointId()) continue;
        auto& state = m_publishedCursors[binding.remoteSessionId];
        if (state.generation != binding.generation) {
            state = PublishedCursor{};
            state.generation = binding.generation;
        }
        // Still pointers send a low-rate freshness pulse, so a dropped sample
        // or a hidden stale marker recovers without requiring mouse movement.
        if (state.lastSentAtMs >= 0 && now - state.lastSentAtMs < 1000
            && state.visible == visible && state.screenId == screenId
            && state.position == position) continue;
        if (m_webSocketClient->sendRemoteCursor(binding.remoteSessionId,
                binding.generation, state.sequence + 1, visible, screenId, position)) {
            ++state.sequence;
            state.visible = visible;
            state.screenId = screenId;
            state.position = position;
            state.lastSentAtMs = now;
        }
    }
}

void ApplicationRuntime::expireStaleRemoteCursors()
{
    const qint64 now = m_cursorClock.elapsed();
    for (auto it = m_receivedCursorAtByEndpoint.begin();
         it != m_receivedCursorAtByEndpoint.end();) {
        if (now - it.value() < 3000) { ++it; continue; }
        if (auto* workspace = m_workspaceManager->findWorkspace(it.key());
            workspace && workspace->canvas) workspace->canvas->hideRemoteCursor();
        it = m_receivedCursorAtByEndpoint.erase(it);
    }
}

void ApplicationRuntime::onConnectionError(const QString& error) {
    if (m_cleanShutdownPrepared) return;
    // Reconnect attempts are intentionally silent. The transport-disconnected
    // edge emits one correlated outage notification for the entire retry run.
    qWarning() << "Failed to connect to server:" << error;
}

void ApplicationRuntime::onClientListReceived(const QList<ClientInfo>& clients) {
    if (m_activeWorkspaceEndpointId.isEmpty() || !m_navigationManager
        || !m_navigationManager->isOnScreenView()) {
        return;
    }

    const ClientInfo* onlineClient = nullptr;
    for (const ClientInfo& client : clients) {
        if (client.endpointId() == m_activeWorkspaceEndpointId
            && client.isOnline()) {
            onlineClient = &client;
            break;
        }
    }
    if (!onlineClient) return;

    const QString targetEndpointId = m_activeWorkspaceEndpointId;
    RemoteSessionCoordinator* coordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    RemoteSessionCoordinator::Binding binding = coordinator
        ? coordinator->outgoingForPeer(targetEndpointId)
        : RemoteSessionCoordinator::Binding();
    const bool explicitlyRequested =
        m_remoteSessionOpenDesiredTargets.contains(targetEndpointId);
    const bool recoveringAuthenticatedBinding =
        binding.phase == QLatin1String("Opening")
        || binding.phase == QLatin1String("Grace")
        || (binding.phase == QLatin1String("Active")
            && !isCommandReadyBinding(m_webSocketClient, binding));
    const bool closeFence = hasPendingOutgoingSessionClose(targetEndpointId)
        || hasCancelledInitialOpenForTarget(targetEndpointId)
        || (!binding.remoteSessionId.isEmpty()
            && m_locallyTerminatingRemoteSessions.contains(
                binding.remoteSessionId));
    if ((explicitlyRequested || wantsForegroundRemoteSession(targetEndpointId)
         || recoveringAuthenticatedBinding)
        && !closeFence) {
        m_remoteSessionOpenSuppressedTargets.remove(targetEndpointId);
        ensureRemoteSessionForClient(*onlineClient);
        binding = coordinator
            ? coordinator->outgoingForPeer(targetEndpointId)
            : RemoteSessionCoordinator::Binding();
    }

    const bool terminalBinding =
        binding.phase == QLatin1String("Terminating")
        || binding.phase == QLatin1String("CleanupPending")
        || binding.phase == QLatin1String("Closed");
    const bool closing = closeFence || terminalBinding;
    const bool active = !closing
        && isCommandReadyBinding(m_webSocketClient, binding);
    const bool grace = !closing && !active
        && (binding.phase == QLatin1String("Active")
            || binding.phase == QLatin1String("Grace"));
    const bool opening = !closing
        && (binding.phase == QLatin1String("Opening")
            || m_remoteSessionOpenPendingTargets.contains(targetEndpointId));

    WorkspaceManager::RemoteSessionState workspaceState =
        WorkspaceManager::RemoteSessionState::Absent;
    if (closing) {
        workspaceState = WorkspaceManager::RemoteSessionState::Closing;
    } else if (active) {
        workspaceState = WorkspaceManager::RemoteSessionState::Active;
    } else if (grace) {
        workspaceState = WorkspaceManager::RemoteSessionState::Grace;
    } else if (opening) {
        workspaceState = WorkspaceManager::RemoteSessionState::Opening;
    }
    if (m_workspaceManager) {
        m_workspaceManager->setRemoteSessionState(
            targetEndpointId, workspaceState);
    }

    refreshProjectClientList();
    ClientWorkspace* workspace = m_workspaceManager
        ? m_workspaceManager->findWorkspace(targetEndpointId) : nullptr;
    if (workspace) {
        m_selectedClient = workspace->lastClientInfo;
    }
    refreshRemoteConnectionPresentation(false);
    updateWorkspaceCapabilities(targetEndpointId);
}

void ApplicationRuntime::onRegistrationConfirmed(const ClientInfo& clientInfo) {
    m_thisClient = clientInfo;
    if (m_sceneActivityModel) {
        const QString endpointId = clientInfo.endpointId().isEmpty()
            ? (m_webSocketClient ? m_webSocketClient->endpointId() : QString())
            : clientInfo.endpointId();
        m_sceneActivityModel->setLocalEndpointId(endpointId);
    }
    qDebug() << "Registration confirmed for:" << clientInfo.getMachineName();
}

void ApplicationRuntime::syncRegistration() {
    // Every endpoint_snapshot path (initial connection, periodic refresh and
    // topology/volume changes) converges here.  Keep the receiver absent from
    // discovery while a previous process/server-boot cache has not reached a
    // durable logical quarantine commit.  The connection handler owns retries;
    // periodic refreshes must never bypass that fail-closed decision.
    if (m_uploadManager
        && !m_uploadManager->receiverReadyForAdvertisement()) {
        if (m_webSocketClient) m_webSocketClient->invalidateLocalDeviceSnapshot();
        qWarning() << "Device snapshot suppressed until remote cache cleanup commits:"
                   << m_uploadManager->receiverCleanupError();
        return;
    }
    if (m_screenEventHandler) {
        m_screenEventHandler->syncRegistration();
        for (auto& cursor : m_publishedCursors) cursor.lastSentAtMs = -1;
    }
}

void ApplicationRuntime::onRemoteSceneLaunchStateChanged(bool active, const QString& targetClientId, const QString& targetMachineName) {
    Q_UNUSED(active);
    Q_UNUSED(targetClientId);
    Q_UNUSED(targetMachineName);
}

bool ApplicationRuntime::captureLocalScreenInfo(QList<ScreenInfo>* screens) {
    return m_systemMonitor && m_systemMonitor->captureScreenInfo(screens);
}

QList<ScreenInfo> ApplicationRuntime::getLocalScreenInfo() {
    QList<ScreenInfo> screens;
    captureLocalScreenInfo(&screens);
    return screens;
}


void ApplicationRuntime::connectToServer() {
    if (!m_connectionManager) return;
    const QString url = m_settingsManager
        ? m_settingsManager->getServerUrl()
        : AppConfig::instance().serverUrl();
    if (m_connectionManager->getServerUrl().isEmpty()) {
        m_connectionManager->setServerUrl(url);
    } else if (m_connectionManager->getServerUrl() != url) {
        m_connectionManager->reconfigureServer(url);
    }
    m_connectionManager->setConnectionEnabled(m_connectionManager->connectionEnabled());
}

QString ApplicationRuntime::getMachineName() {
    return m_systemMonitor ? m_systemMonitor->getMachineName() : "Unknown Machine";
}

QString ApplicationRuntime::getPlatformName() {
    return m_systemMonitor ? m_systemMonitor->getPlatformName() : "Unknown";
}

int ApplicationRuntime::getSystemVolumePercent() {
    return m_systemMonitor ? m_systemMonitor->getSystemVolumePercent() : -1;
}

void ApplicationRuntime::updateConnectionStatus() {
    QString status = m_connectionManager->getConnectionStatus();
    // Update the local network status in the new container
    setLocalNetworkStatus(status);
}

void ApplicationRuntime::updateIndividualProgressFromServer(int globalPercent, int filesCompleted, int totalFiles) {
    if (m_uploadEventHandler) {
        m_uploadEventHandler->updateIndividualProgressFromServer(globalPercent, filesCompleted, totalFiles);
    }
}


bool ApplicationRuntime::hasUnuploadedFilesForTarget(const QString& targetClientId) const {
    ICanvasHost* canvas = canvasForEndpointId(targetClientId);
    if (!canvas) {
        return false;
    }

    for (CanvasMedia* media : canvas->enumerateMediaItems()) {
        if (!media || media->isText()) {
            continue;
        }
        const QString fileId = media->fileId();
        if (fileId.isEmpty()) {
            return true;
        }
        if (!m_fileManager->isFileUploadedToClient(fileId, targetClientId)
            || !m_uploadManager || !m_uploadManager->remoteMediaReady(targetClientId, fileId)) {
            return true;
        }
    }
    return false;
}

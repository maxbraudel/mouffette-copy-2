#include "backend/runtime/ApplicationRuntime.h"
#include "frontend/managers/ui/RemoteClientState.h"
#include "backend/network/WebSocketClient.h"
#include "backend/domain/models/ClientInfo.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "backend/network/UploadManager.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/RemoteCacheStore.h"
#include "backend/files/FileWatcher.h"
#include "shared/rendering/ICanvasHost.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/files/FileManager.h"
#include "frontend/rendering/remote/RemoteSceneController.h"
#include "backend/domain/session/SessionManager.h"
#include "backend/domain/session/IncomingSessionOrphanWatchdog.h"
#include "backend/domain/project/ProjectManager.h"
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
#include "backend/controllers/CanvasSessionController.h"
#include "backend/controllers/TimerController.h"
#include "backend/config/AppConfig.h"
#include "backend/managers/app/SettingsManager.h"
#include "backend/managers/app/MigrationTelemetryManager.h"
#include "backend/managers/network/ClientListBuilder.h"
#include "backend/managers/network/ConnectionManager.h"
#include "frontend/handlers/UploadSignalConnector.h"
#include <QHostInfo>
#include <QGuiApplication>

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
#include <QCoreApplication>
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

ProjectManager::TimingPolicy projectTimingPolicyFromConfig() {
    ProjectManager::TimingPolicy timing;
    timing.projectHiddenRetentionMs =
        AppConfig::instance().projectHiddenRetentionMs();
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

QString sha256ForPath(const QString& canonicalPath) {
    QFile file(canonicalPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(4 * 1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            return {};
        }
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}
}

#ifdef Q_OS_MACOS
#include "backend/platform/macos/MacVideoThumbnailer.h"
#include "backend/platform/macos/MacWindowManager.h"
#endif
#include <QSet>
#include <QElapsedTimer>
#include <QDateTime>
#include <QThreadPool>
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
// Ensure GUIDs (IIDs/CLSIDs) are defined in this translation unit for MinGW linkers
#ifndef INITGUID
#define INITGUID
#endif
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#endif
#ifdef Q_OS_MACOS
#include <QProcess>
#endif

static RemoteSceneController* g_remoteSceneController = nullptr; // Remote scene controller global instance

// Z-ordering constants used throughout the scene
namespace {
constexpr qreal Z_SCREENS = -1000.0;
constexpr qreal Z_MEDIA_BASE = 1.0;
constexpr qreal Z_REMOTE_CURSOR = 10000.0;
constexpr qreal Z_SCENE_OVERLAY = 12000.0; // above all scene content
}


void ApplicationRuntime::setRemoteConnectionStatus(const QString& status, bool propagateLoss) {
    const QString up = status.toUpper();
    m_remoteStatusText = up;
    if (up == "CONNECTED") {
        m_remoteClientConnected = true;
    } else if (up == "DISCONNECTED" || up.startsWith("CONNECTING") || up == "ERROR") {
        m_remoteClientConnected = false;
    }
    m_remoteBusy = up.startsWith("CONNECTING") || up.startsWith("RECONNECTING");

    refreshOverlayActionsState(up == "CONNECTED", propagateLoss);
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
      m_fileManager(new FileManager()),  // Phase 4.3: Inject FileManager
      m_sessionManager(new SessionManager(this)),  // Phase 4.1
      m_projectManager(new ProjectManager(projectTimingPolicyFromConfig(), this)),
      m_sceneActivityModel(new SceneActivityModel(this)),
      m_systemMonitor(new SystemMonitor(this)), // Phase 3
      m_systemTrayManager(new SystemTrayManager(this)), // Phase 6.2
      m_webSocketClient(new WebSocketClient(
          RuntimeProfile::identityLocation(), runtimeProfile.isPersistent(), this, {},
          runtimeProfile.instanceId, runtimeProfile.ordinal)),
      m_connectionManager(new ConnectionManager(m_webSocketClient, this)),
      m_settingsManager(new SettingsManager(this)),
      m_webSocketMessageHandler(new WebSocketMessageHandler(this, this)), // Phase 7.1
      m_screenEventHandler(new ScreenEventHandler(this, this)), // Phase 7.2
      m_uploadEventHandler(new UploadEventHandler(this, this)), // Phase 7.4
      m_clientListEventHandler(new ClientListEventHandler(this, m_webSocketClient, this)), // Phase 7.3
      m_canvasSessionController(new ClientWorkspaceController(this, this)),
      m_timerController(new TimerController(this, this)), // Phase 10
      m_uploadSignalConnector(new UploadSignalConnector(this)), // Phase 15
      m_statusUpdateTimer(new QTimer(this)),
      m_displaySyncTimer(new QTimer(this)),
      m_incomingSessionOrphanWatchdog(
          new IncomingSessionOrphanWatchdog(this)),
      m_uploadManager(new UploadManager(m_fileManager, this)),
      m_fileWatcher(new FileWatcher(this)),
      m_navigationManager(new ScreenNavigationManager(this))
{
    m_sessionManager->setRemoteSessionHiddenTimeoutMs(
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
    connect(m_sessionManager, &SessionManager::remoteSessionCloseDue,
            this, [this](const QString& targetEndpointId) {
        terminateProjectRemoteSession(targetEndpointId, true);
    });

    // [Phase 12] Load persisted settings (server URL, auto-upload, persistent client ID)
    m_settingsManager->loadSettings();
    if (m_projectManager) {
        connect(m_projectManager, &ProjectManager::projectCheckpointDue,
                this, &ApplicationRuntime::persistProjectCanvas);
        connect(m_projectManager, &ProjectManager::projectDeleted,
                this, [this](const QString&, const QString& targetEndpointId) {
            clearDeletedProjectFromWorkspace(targetEndpointId);
        });
        connect(m_projectManager, &ProjectManager::projectsChanged,
                this, &ApplicationRuntime::refreshProjectClientList);
        connect(m_projectManager, &ProjectManager::persistenceError,
                this, [](const QString& message) {
            qWarning().noquote() << "Project persistence error:" << message;
        });
    }
    if (m_canvasSessionController) {
        QTimer::singleShot(0, this, [this]() {
            if (m_canvasSessionController) {
                m_canvasSessionController->prewarmQuickCanvasHost();
            }
        });
    }
    // Initialize remote scene controller once
    if (!g_remoteSceneController) {
        g_remoteSceneController = new RemoteSceneController(m_fileManager, m_webSocketClient, this);
    }
    connect(g_remoteSceneController, &RemoteSceneController::teardownSettled,
            this, &ApplicationRuntime::handleRemoteRendererTeardownSettled,
            Qt::UniqueConnection);
    // [PHASE 6.2] Setup system tray
    if (m_systemTrayManager) {
        m_systemTrayManager->setup();
        connect(m_systemTrayManager, &SystemTrayManager::activated, this,
                [this](SystemTrayManager::ActivationReason reason) {
            onTrayIconActivated(static_cast<int>(reason));
        });
    }
    
    // [PHASE 3] Start system monitoring
    if (m_systemMonitor) {
        m_systemMonitor->startVolumeMonitoring();
        // Volume is part of every protocol-v3 device snapshot; it is not tied
        // to any remote controller's Project or session.
        connect(m_systemMonitor, &SystemMonitor::volumeChanged, this, [this](int) {
            if (m_webSocketClient && m_webSocketClient->isConnected()) {
                syncRegistration();
            }
        });
        connect(m_systemMonitor, &SystemMonitor::screenConfigurationChanged,
                this, [this](const QList<ScreenInfo>&) {
            if (m_webSocketClient && m_webSocketClient->isConnected()) {
                syncRegistration();
            }
        });
    }

    // [PHASE 7.1] Setup WebSocket message handler connections
    if (m_webSocketMessageHandler) {
        m_webSocketMessageHandler->setupConnections(m_webSocketClient);
    }
    
    // [PHASE 7.2] Setup screen event handler connections
    if (m_screenEventHandler) {
        m_screenEventHandler->setupConnections(m_webSocketClient);
    }
    
    // [PHASE 7.3] Setup client list event handler connections
    if (m_clientListEventHandler) {
        m_clientListEventHandler->setupConnections(m_webSocketClient);
    }
    // The handler above applies discovery/snapshot updates first. This second
    // observer only reconciles the active workspace with its explicit RemoteSession;
    // it never owns retry policy or rebuilds the list a second time.
    connect(m_webSocketClient, &WebSocketClient::clientListReceived,
            this, &ApplicationRuntime::onClientListReceived);

    // SceneActivityModel is the sole source for the Ongoing Scenes UI. A run
    // is inserted only after the protocol-v3 coordinator reaches Live and is
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
                        : QDateTime::currentMSecsSinceEpoch();
                }
                m_sceneActivityModel->upsertLive(
                    run.sceneRunId, run.remoteSessionId, run.ownerEndpointId,
                    run.targetEndpointId, startedAt);
            });
        }
        connect(m_webSocketClient, &WebSocketClient::transportHealthChanged,
                m_sceneActivityModel, &SceneActivityModel::setAllDegraded);
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
                m_sceneActivityModel->setSessionDegraded(sessionId, false);
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
        connect(m_webSocketClient, &WebSocketClient::remoteSessionError,
                this, &ApplicationRuntime::handleRemoteSessionError);
    }
    
    // ConnectionManager is the sole owner of retries and transport status.
    connect(m_connectionManager, &ConnectionManager::connectionError,
            this, &ApplicationRuntime::onConnectionError);
    connect(m_webSocketClient, &WebSocketClient::connected,
            this, &ApplicationRuntime::retryPendingTeardownAcks);
    connect(m_webSocketClient, &WebSocketClient::transportConnected,
            m_incomingSessionOrphanWatchdog,
            &IncomingSessionOrphanWatchdog::transportConnected);
    connect(m_webSocketClient, &WebSocketClient::disconnected,
            this, [this]() {
        armIncomingSessionOrphanWatchdog();
        if (m_cleanShutdownPrepared && !m_cleanShutdownFinished) {
            finishCleanShutdown();
        }
    });
    connect(m_connectionManager, &ConnectionManager::statusChanged,
            this, [this](const QString& status) {
        setLocalNetworkStatus(status);
        if (status == QLatin1String("Reconnecting")) {
            for (ClientInfo& client : m_discoveredClients) {
                client.setStatus(QStringLiteral("Reconnecting"));
                client.setAvailabilityStatus(QStringLiteral("Reconnecting"));
            }
            refreshProjectClientList();
            if (m_activeCanvas) {
                // Grace disables new remote commands but does not tear down the
                // running graph or discard resumable upload state.
                m_activeCanvas->setOverlayActionsEnabled(false);
            }
        }
    });
    connect(m_connectionManager, &ConnectionManager::leaseExpired,
            this, [this](const QString& serverBootId, quint64 generation) {
        if (m_incomingSessionOrphanWatchdog) {
            m_incomingSessionOrphanWatchdog->cancelAll();
            m_incomingOrphanSessionIds.clear();
        }
        if (m_cleanShutdownPrepared) {
            // WebSocketClient::disconnect() deliberately expires its in-memory
            // lease. During a requested quit this is cleanup, not an outage,
            // and must not create a misleading history entry/toast.
            if (m_sceneActivityModel) m_sceneActivityModel->clear();
            return;
        }
        // This signal is delivered synchronously from WebSocketClient before
        // SceneRunCoordinator::clearSessions(). Capture every incoming binding
        // now; a queued cleanup would lose the immutable cache correlation.
        beginTerminalIncomingCacheCleanup(QStringLiteral("lease_expired"));
        if (m_toastSystem) {
            NotificationRequest notification;
            notification.severity = NotificationSeverity::Warning;
            notification.category = QStringLiteral("Remote session");
            notification.message = QStringLiteral(
                "Connection lost for more than 3 seconds. Remote scenes were stopped and session uploads were invalidated.");
            notification.correlationId = QStringLiteral("lease-expired:%1:%2")
                                             .arg(serverBootId)
                                             .arg(generation);
            notification.terminal = true;
            m_toastSystem->publishNotification(notification);
        }
        if (m_sceneActivityModel) m_sceneActivityModel->clear();
        if (m_sessionManager) {
            const QList<QString> targets =
                m_sessionManager->getAllPersistentClientIds();
            for (const QString& target : targets) {
                m_remoteSessionOpenSuppressedTargets.insert(target);
                if (CanvasSession* session =
                        m_sessionManager->findSession(target)) {
                    terminateProjectRemoteSession(target, false);
                }
            }
            markAllSessionsOffline();
        }
        m_remoteSessionOpenPendingTargets.clear();
        m_remoteSessionOpenTargetByRequestId.clear();
        m_locallyTerminatingRemoteSessions.clear();
        m_pendingTeardownAcks.clear();
        m_discoveredClients.clear();
        refreshProjectClientList();
    });
    connect(m_webSocketClient, &WebSocketClient::registrationConfirmed, this, &ApplicationRuntime::onRegistrationConfirmed);
    // UploadManager subscribes to the canonical protocol-v3 upload envelope.
    m_uploadManager->setWebSocketClient(m_webSocketClient);
    connect(m_uploadManager, &UploadManager::terminalIncomingCleanupRequired,
            this, &ApplicationRuntime::beginTerminalIncomingCacheCleanup);
    connect(m_uploadManager,
            &UploadManager::receiverAdvertisementReadinessChanged,
            this, [this](bool ready, const QString&) {
        // A welcome may have completed while renderer destruction was still
        // pending. Publish the device only after logical quarantine commits.
        if (ready && m_webSocketClient && m_webSocketClient->isConnected()) {
            syncRegistration();
        }
    });

    connect(m_uploadManager, &UploadManager::assetRemovalCommitted,
            this, [this](const QString& targetEndpointId,
                         const QStringList& localFileIds) {
        CanvasSession* session = m_sessionManager
            ? m_sessionManager->findSession(targetEndpointId) : nullptr;
        if (!session) return;
        for (const QString& fileId : localFileIds) {
            session->knownRemoteFileIds.remove(fileId);
            session->expectedIdeaFileIds.remove(fileId);
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
                        .arg(reason), 5000);
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
            clearRemoteSessionRuntimeState(targetEndpointId, false);
        }
    });
    
    // A file is invalidated only after its last local media reference is gone.
    // Remote copies are addressed through the exact authenticated session
    // inventory; no target name or historical canvas identifier is accepted.
    FileManager::setFileRemovalNotifier([this](const QString& fileId, const QList<QString>& clientIds, const QList<QString>& canvasSessionIds) {
        Q_UNUSED(canvasSessionIds);
        if (!m_uploadManager) return;
        QSet<QString> uniqueTargets(clientIds.cbegin(), clientIds.cend());
        for (const QString& targetEndpointId : uniqueTargets) {
            m_uploadManager->requestAssetRemoval(
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
    // [Phase 10] Setup timers via TimerController
    m_timerController->setupTimers();

    // Initialize toast notification system
    m_toastSystem = new ToastNotificationSystem(this);
    ToastNotificationSystem::setInstance(m_toastSystem);
    updateHistoryVisibilityState();

    if (m_projectManager && !m_projectManager->load()) {
        TOAST_ERROR(QStringLiteral("Projects could not be loaded: %1")
                        .arg(m_projectManager->lastError()), 5000);
    } else {
        validateAllProjectSources();
        refreshProjectClientList();
    }

    connectToServer();
}


void ApplicationRuntime::stopInlineSpinner() {
    if (!m_remoteBusy) return;
    m_remoteBusy = false;
    emit presentationStateChanged();
}

void ApplicationRuntime::setLocalNetworkStatus(const QString& status) {
    const QString normalized = status.trimmed().toUpper();
    if (m_localStatusText == normalized) return;
    m_localStatusText = normalized;
    emit presentationStateChanged();
}

// Phase 4.1: Migrated to use SessionManager
// [PHASE 8] Session lookup - delegate to CanvasSessionController
ApplicationRuntime::CanvasSession* ApplicationRuntime::findCanvasSession(const QString& persistentClientId) {
    return m_canvasSessionController ? static_cast<CanvasSession*>(m_canvasSessionController->findCanvasSession(persistentClientId)) : nullptr;
}

const ApplicationRuntime::CanvasSession* ApplicationRuntime::findCanvasSession(const QString& persistentClientId) const {
    return m_canvasSessionController ? static_cast<const CanvasSession*>(m_canvasSessionController->findCanvasSession(persistentClientId)) : nullptr;
}

ApplicationRuntime::CanvasSession* ApplicationRuntime::findCanvasSessionByServerClientId(const QString& serverClientId) {
    return m_canvasSessionController ? static_cast<CanvasSession*>(m_canvasSessionController->findCanvasSessionByServerClientId(serverClientId)) : nullptr;
}

const ApplicationRuntime::CanvasSession* ApplicationRuntime::findCanvasSessionByServerClientId(const QString& serverClientId) const {
    return m_canvasSessionController ? static_cast<const CanvasSession*>(m_canvasSessionController->findCanvasSessionByServerClientId(serverClientId)) : nullptr;
}

ApplicationRuntime::CanvasSession* ApplicationRuntime::findCanvasSessionByIdeaId(const QString& canvasSessionId) {
    return m_canvasSessionController ? static_cast<CanvasSession*>(m_canvasSessionController->findCanvasSessionByIdeaId(canvasSessionId)) : nullptr;
}

// [PHASE 8] Session lifecycle - delegate to CanvasSessionController
ApplicationRuntime::CanvasSession& ApplicationRuntime::ensureCanvasSession(const ClientInfo& client) {
    void* sessionPtr = m_canvasSessionController->ensureCanvasSession(client);
    return *static_cast<CanvasSession*>(sessionPtr);
}

// [PHASE 8] Session configuration - delegate to CanvasSessionController
void ApplicationRuntime::configureCanvasSession(CanvasSession& session) {
    if (m_canvasSessionController) {
        m_canvasSessionController->configureCanvasSession(&session);
    }
}

// [PHASE 8] Switch canvas session - delegate to CanvasSessionController
void ApplicationRuntime::switchToCanvasSession(const QString& persistentClientId) {
    if (m_canvasSessionController) {
        m_canvasSessionController->switchToCanvasSession(persistentClientId);
    }
}

// [PHASE 8] Update upload button - delegate to CanvasSessionController
void ApplicationRuntime::updateUploadButtonForSession(CanvasSession& session) {
    if (m_canvasSessionController) {
        m_canvasSessionController->updateUploadButtonForSession(&session);
    }
}

// [Phase 12] Delegate to SettingsManager
bool ApplicationRuntime::getAutoUploadImportedMedia() const {
    return m_settingsManager ? m_settingsManager->getAutoUploadImportedMedia() : false;
}


void ApplicationRuntime::markCanvasLoadRequest(const QString& persistentClientId) {
    if (persistentClientId.isEmpty()) {
        return;
    }
    m_canvasLoadRequestMsBySession.insert(persistentClientId, QDateTime::currentMSecsSinceEpoch());
    MigrationTelemetryManager::logCanvasLoadRequest(persistentClientId);
}

void ApplicationRuntime::recordCanvasLoadReady(const QString& persistentClientId, int screenCount) {
    if (persistentClientId.isEmpty()) {
        return;
    }
    const qint64 startedAt = m_canvasLoadRequestMsBySession.take(persistentClientId);
    qint64 latencyMs = -1;
    if (startedAt > 0) {
        latencyMs = qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - startedAt);
    }
    MigrationTelemetryManager::logCanvasLoadReady(persistentClientId, screenCount, latencyMs);
}

QString ApplicationRuntime::createIdeaId() const {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// [PHASE 8] Rotate session idea - delegate to CanvasSessionController
void ApplicationRuntime::rotateSessionIdea(CanvasSession& session) {
    if (m_canvasSessionController) {
        m_canvasSessionController->rotateSessionIdea(&session);
    }
}

void ApplicationRuntime::reconcileRemoteFilesForSession(CanvasSession& session, const QSet<QString>& currentFileIds) {
    session.expectedIdeaFileIds = currentFileIds;
    m_fileManager->replaceIdeaFileSet(session.canvasSessionId, currentFileIds);

    if (!session.persistentClientId.isEmpty()) {
        const QSet<QString> toRemove = session.knownRemoteFileIds - currentFileIds;
        for (const QString& fileId : toRemove) {
            if (m_uploadManager) {
                m_uploadManager->requestAssetRemoval(
                    session.persistentClientId, fileId,
                    QStringLiteral("no_longer_referenced"));
            }
        }
    }
}

// [Phase 15] Delegate upload signal connections to UploadSignalConnector
void ApplicationRuntime::connectUploadSignals() {
    if (m_uploadSignalConnector) {
        m_uploadSignalConnector->connectAllSignals(this, m_uploadManager, m_webSocketClient, m_uploadSignalsConnected);
    }
}

void ApplicationRuntime::setUploadSessionByUploadId(const QString& uploadId, const QString& sessionIdentity) {
    m_uploadSessionByUploadId.insert(uploadId, sessionIdentity);
}

void ApplicationRuntime::markAllSessionsOffline() {
    for (CanvasSession* session : m_sessionManager->getAllSessions()) {
        session->lastClientInfo.setEndpointId(session->persistentClientId);
        session->lastClientInfo.setFromMemory(true);
        session->lastClientInfo.setOnline(false);
    }
}

QList<ClientInfo> ApplicationRuntime::buildDisplayClientList(const QList<ClientInfo>& connectedClients) {
    m_discoveredClients = connectedClients;
    if (!m_projectManager) {
        m_displayClients = ClientListBuilder::buildDisplayClientList(this, connectedClients);
        emit displayClientsChanged(m_displayClients);
        return m_displayClients;
    }

    const QList<ProjectClientEntry> entries =
        m_projectManager->mergeDiscoveredClients(connectedClients);
    QList<ClientInfo> result;
    result.reserve(entries.size());
    for (const ProjectClientEntry& entry : entries) {
        ClientInfo client = entry.client;
        client.setEndpointId(entry.endpointId);
        client.setOnline(entry.online);
        client.setFromMemory(entry.hasProject);
        client.setProjectId(entry.projectId);
        client.setHasProject(entry.hasProject);
        client.setRemoteSessionCloseAtMs(m_sessionManager
            ? m_sessionManager->remoteSessionCloseAtMs(entry.endpointId) : -1);
        client.setProjectDeleteAtMs(entry.projectDeleteAtMs);
        if (!entry.online) {
            client.setStatus(QStringLiteral("Offline"));
            client.setAvailabilityStatus(QStringLiteral("Offline"));
        } else {
            static const QSet<QString> allowedStatuses = {
                QStringLiteral("Available"),
                QStringLiteral("Connecting"),
                QStringLiteral("Connected"),
                QStringLiteral("Reconnecting"),
                QStringLiteral("Disconnecting"),
                QStringLiteral("In use"),
                QStringLiteral("Unavailable")
            };
            QString status = client.availabilityStatus().trimmed();
            if (status.compare(QStringLiteral("active"), Qt::CaseInsensitive) == 0) {
                status = QStringLiteral("Connected");
            } else if (status.compare(QStringLiteral("in_use"), Qt::CaseInsensitive) == 0
                       || status.compare(QStringLiteral("busy"), Qt::CaseInsensitive) == 0) {
                status = QStringLiteral("In use");
            } else if (status.compare(QStringLiteral("opening"), Qt::CaseInsensitive) == 0) {
                status = QStringLiteral("Connecting");
            } else if (status.compare(QStringLiteral("grace"), Qt::CaseInsensitive) == 0) {
                status = QStringLiteral("Reconnecting");
            } else if (status.compare(QStringLiteral("terminating"), Qt::CaseInsensitive) == 0
                       || status.compare(QStringLiteral("cleanup_pending"), Qt::CaseInsensitive) == 0) {
                status = QStringLiteral("Disconnecting");
            } else if (status.compare(QStringLiteral("cleanup_error"), Qt::CaseInsensitive) == 0) {
                status = QStringLiteral("Unavailable");
            }
            if (!allowedStatuses.contains(status)) {
                status = QStringLiteral("Available");
            }
            client.setStatus(status);
            client.setAvailabilityStatus(status);
        }

        if (CanvasSession* session = m_sessionManager->findSession(entry.endpointId)) {
            if (entry.online) {
                m_sessionManager->updateSessionServerId(entry.endpointId, client.getId());
                session = m_sessionManager->findSession(entry.endpointId);
            }
            if (session) {
                session->lastClientInfo = client;
                session->remoteContentClearedOnDisconnect = !entry.online
                    ? session->remoteContentClearedOnDisconnect : false;
                if (session->canvas) {
                    session->canvas->setRemoteSceneTarget(
                        entry.endpointId, client.getMachineName());
                }
            }
        }
        result.append(client);
    }
    m_displayClients = result;
    emit displayClientsChanged(m_displayClients);
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
        if (reference.canonicalSourcePath != canonicalPath
            || reference.sourceIdentity != identity
            || reference.sha256.isEmpty()) {
            reference.canonicalSourcePath = canonicalPath;
            reference.sourceIdentity = identity;
            reference.sha256 = sha256ForPath(canonicalPath);
            reference.assetId = reference.sha256;
        }
        reference.mediaType = media->isVideo()
            ? QStringLiteral("video") : QStringLiteral("image");
        if (!reference.sha256.isEmpty()) {
            references.append(reference);
        }
    }
    return references;
}

void ApplicationRuntime::persistProjectCanvas(const QString& targetEndpointId) {
    if (!m_projectManager || targetEndpointId.isEmpty()) {
        return;
    }
    CanvasSession* session = m_sessionManager->findSession(targetEndpointId);
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

void ApplicationRuntime::restoreProjectCanvas(CanvasSession& session) {
    if (!m_projectManager || !session.canvas
        || m_restoredProjectIds.contains(session.persistentClientId)) {
        return;
    }
    const ProjectRecord* stored =
        m_projectManager->projectForTarget(session.persistentClientId);
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
            || identity != reference.sourceIdentity
            || sha256ForPath(canonicalPath) != reference.sha256) {
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

    QStringList skipped;
    if (!session.canvas->restoreProjectState(durableState, validSources, &skipped)) {
        qWarning() << "Project canvas restoration failed for device"
                   << session.persistentClientId.left(12);
        return;
    }
    for (const QString& mediaId : skipped) {
        invalidMediaIds.insert(mediaId);
    }
    m_restoredProjectIds.insert(session.persistentClientId);

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
        m_projectManager->updateCanvasState(
            session.persistentClientId, durableState, validReferences,
            stored->savedScreens);
        TOAST_WARNING(QStringLiteral("%1 missing or modified media item%2 removed from the project")
                          .arg(invalidMediaIds.size())
                          .arg(invalidMediaIds.size() == 1 ? QString() : QStringLiteral("s")),
                      5000);
    }
}

void ApplicationRuntime::removeInvalidMediaItems(
    const QList<CanvasMedia*>& mediaItems) {
    if (!m_sessionManager || mediaItems.isEmpty()) {
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
    for (CanvasSession* session : m_sessionManager->getAllSessions()) {
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
                itemsByProject[session->persistentClientId].append(candidate);
            }
        }
    }

    // Stop every affected graph before deleting the first item. The canvas
    // method also sends the remote stop when a remote run is active.
    for (auto it = itemsByProject.cbegin(); it != itemsByProject.cend(); ++it) {
        if (CanvasSession* session = m_sessionManager->findSession(it.key());
            session && session->canvas) {
            session->canvas->stopScenesForSourceInvalidation();
        }
    }

    int removedCount = 0;
    for (auto it = itemsByProject.cbegin(); it != itemsByProject.cend(); ++it) {
        CanvasSession* session = m_sessionManager->findSession(it.key());
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
                && sourceIdentityForPath(canonicalPath) == reference.sourceIdentity
                && sha256ForPath(canonicalPath) == reference.sha256;
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

ICanvasHost* ApplicationRuntime::canvasForClientId(const QString& clientId) const {
    if (clientId.isEmpty()) {
        const CanvasSession* active = findCanvasSession(m_activeSessionIdentity);
        return active ? active->canvas : nullptr;
    }
    for (CanvasSession* session : m_sessionManager->getAllSessions()) {
        if (session->serverAssignedId == clientId) {
            return session->canvas;
        }
    }
    return nullptr;
}


// [PHASE 8] Session for active upload - delegate to CanvasSessionController
ApplicationRuntime::CanvasSession* ApplicationRuntime::sessionForActiveUpload() {
    return m_canvasSessionController ? static_cast<CanvasSession*>(m_canvasSessionController->sessionForActiveUpload()) : nullptr;
}

// [PHASE 8] Session for upload ID - delegate to CanvasSessionController
ApplicationRuntime::CanvasSession* ApplicationRuntime::sessionForUploadId(const QString& uploadId) {
    return m_canvasSessionController ? static_cast<CanvasSession*>(m_canvasSessionController->sessionForUploadId(uploadId)) : nullptr;
}

// [PHASE 8] Clear upload tracking - delegate to CanvasSessionController
void ApplicationRuntime::clearUploadTracking(CanvasSession& session) {
    if (m_canvasSessionController) {
        m_canvasSessionController->clearUploadTracking(&session);
    }
}


void ApplicationRuntime::updateApplicationSuspendedState(bool suspended) {
    setApplicationSuspended(suspended);
}



void ApplicationRuntime::handleApplicationStateChanged(Qt::ApplicationState state) {
    setApplicationSuspended(state == Qt::ApplicationHidden
                            || state == Qt::ApplicationSuspended
                            || m_nativeSystemSuspended);
}

void ApplicationRuntime::handleNativeSystemSuspendedChanged(bool suspended) {
    m_nativeSystemSuspended = suspended;
    setApplicationSuspended(suspended
                            || QGuiApplication::applicationState() == Qt::ApplicationHidden
                            || QGuiApplication::applicationState() == Qt::ApplicationSuspended);
}

void ApplicationRuntime::showScreenView(const ClientInfo& client) {
    if (!m_navigationManager) return;
    ClientInfo selectedClient = client;
    const QString targetEndpointId = selectedClient.endpointId().trimmed();
    if (targetEndpointId.isEmpty()) {
        TOAST_ERROR(QStringLiteral("This client has no authenticated device identity"), 4000);
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
        TOAST_ERROR(QStringLiteral("This client's identity is incomplete. Refresh the client list and try again."), 4000);
        return;
    }

    if (!m_activeSessionIdentity.isEmpty()
        && m_activeSessionIdentity != targetEndpointId
        && m_navigationManager->isOnScreenView()) {
        persistProjectCanvas(m_activeSessionIdentity);
        if (m_projectManager) m_projectManager->setHidden(m_activeSessionIdentity);
        if (m_sessionManager) m_sessionManager->setWorkspaceHidden(m_activeSessionIdentity);
    }

    CanvasSession& workspace =
        m_sessionManager->getOrCreateSession(targetEndpointId, selectedClient);
    workspace.lastClientInfo = selectedClient;
    workspace.lastClientInfo.setEndpointId(targetEndpointId);
    workspace.workspaceVisible = true;
    workspace.sessionHiddenAtMs = -1;
    m_activeSessionIdentity = targetEndpointId;
    m_activeRemoteClientId = targetEndpointId;
    m_selectedClient = workspace.lastClientInfo;

    bool hasProject = m_projectManager
        && m_projectManager->hasProjectForTarget(targetEndpointId);
    if (hasProject) {
        m_projectManager->updateTargetReference(
            ProjectTargetReference::fromClientInfo(selectedClient));
        if (!m_projectManager->setVisible(targetEndpointId)) {
            hasProject = false;
        } else if (const ProjectRecord* project =
                       m_projectManager->projectForTarget(targetEndpointId)) {
            m_sessionManager->updateSessionIdeaId(targetEndpointId,
                                                  project->projectId);
        }
    }

    RemoteSessionCoordinator* remoteSessionCoordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    const RemoteSessionCoordinator::Binding retainedBinding = remoteSessionCoordinator
        ? remoteSessionCoordinator->outgoingForPeer(targetEndpointId)
        : RemoteSessionCoordinator::Binding();
    const bool active = retainedBinding.phase == QLatin1String("Active")
        && !m_locallyTerminatingRemoteSessions.contains(retainedBinding.remoteSessionId);
    const bool grace = retainedBinding.phase == QLatin1String("Grace")
        && !m_locallyTerminatingRemoteSessions.contains(retainedBinding.remoteSessionId);
    if (active) {
        m_sessionManager->setRemoteSessionState(
            targetEndpointId, SessionManager::RemoteSessionState::Active);
    } else if (grace) {
        m_sessionManager->setRemoteSessionState(
            targetEndpointId, SessionManager::RemoteSessionState::Grace);
    } else if (!m_remoteSessionOpenPendingTargets.contains(targetEndpointId)) {
        m_sessionManager->setRemoteSessionState(
            targetEndpointId, SessionManager::RemoteSessionState::Absent);
    }

    const bool hasRemoteSession = active || grace
        || m_remoteSessionOpenPendingTargets.contains(targetEndpointId);
    if ((hasProject || hasRemoteSession) && !workspace.canvas) {
        ensureCanvasSession(workspace.lastClientInfo);
    }
    CanvasSession* currentWorkspace = m_sessionManager->findSession(targetEndpointId);
    if (!currentWorkspace) return;
    if (hasProject && currentWorkspace->canvas) restoreProjectCanvas(*currentWorkspace);
    if (hasRemoteSession && currentWorkspace->canvas) {
        currentWorkspace->canvas->setScreens(
            currentWorkspace->lastClientInfo.getScreens());
    }
    updateWorkspaceCapabilities(targetEndpointId);

    if (currentWorkspace->canvas) switchToCanvasSession(targetEndpointId);
    else {
        m_activeCanvas = nullptr;
        if (m_navigationManager) m_navigationManager->setActiveCanvas(nullptr);
        if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
    }

    m_canvasRevealedForCurrentClient = currentWorkspace->canvas != nullptr;
    m_canvasContentEverLoaded = currentWorkspace->canvas != nullptr;
    m_preserveViewportOnReconnect = hasProject;
    updateClientNameDisplay(currentWorkspace->lastClientInfo);
    m_remoteClientConnected = active || grace;
    m_remoteVolumePercent = active
        ? currentWorkspace->lastClientInfo.getVolumePercent() : -1;
    if (active) setRemoteConnectionStatus(QStringLiteral("CONNECTED"), false);
    else if (grace) setRemoteConnectionStatus(QStringLiteral("RECONNECTING..."), false);
    else setRemoteConnectionStatus(currentWorkspace->lastClientInfo.isOnline()
        ? QStringLiteral("AVAILABLE") : QStringLiteral("DISCONNECTED"), false);

    m_navigationManager->showScreenView(currentWorkspace->lastClientInfo,
                                        currentWorkspace->canvas != nullptr);
    if (currentWorkspace->canvas) m_navigationManager->revealCanvas();
    setActiveProjectVisibleIfAppropriate();
    m_sessionManager->setWorkspaceVisible(targetEndpointId);
    m_applicationPage = 1;
    updateHistoryVisibilityState();
    emit activeSessionChanged(m_activeSessionIdentity);
    emit applicationPageChanged(1);
    emit presentationStateChanged();
}

void ApplicationRuntime::updateClientNameDisplay(const ClientInfo& client) {
    m_remoteDisplayName = client.getMachineName().trimmed();
    emit presentationStateChanged();
}

void ApplicationRuntime::showClientListView() {
    // Do NOT unload when navigating back to client list - uploads persist per session
    // Each client maintains its own upload state that should survive navigation
    
    const QString leavingTarget = m_activeSessionIdentity;
    if (!leavingTarget.isEmpty() && m_projectManager
        && m_navigationManager && m_navigationManager->isOnScreenView()) {
        persistProjectCanvas(leavingTarget);
        m_projectManager->setHidden(leavingTarget);
    }
    if (!leavingTarget.isEmpty() && m_sessionManager) {
        m_sessionManager->setWorkspaceHidden(leavingTarget);
    }
    if (m_navigationManager) m_navigationManager->showClientList();
    m_uploadManager->setTargetClientId(QString());
    // Navigation hides the canvas but does not terminate its RemoteSession.
    // Preserve the authenticated binding and its presentation state until the
    // hidden-project deadline or an explicit user action closes it.
    
    m_remoteBusy = false;
    m_applicationPage = 0;
    updateHistoryVisibilityState();
    emit applicationPageChanged(0);
    emit presentationStateChanged();
}

void ApplicationRuntime::updateRemoteClientAvailability(const QString& targetEndpointId,
                                                const QString& status) {
    if (targetEndpointId.isEmpty() || status.isEmpty()) {
        return;
    }
    for (ClientInfo& client : m_discoveredClients) {
        if (client.endpointId() != targetEndpointId) {
            continue;
        }
        client.setStatus(status);
        client.setAvailabilityStatus(status);
        if (status == QLatin1String("Offline")) {
            client.setOnline(false);
        }
    }
    if (m_sessionManager) {
        if (CanvasSession* session = m_sessionManager->findSession(targetEndpointId)) {
            session->lastClientInfo.setStatus(status);
            session->lastClientInfo.setAvailabilityStatus(status);
            if (status == QLatin1String("Offline")) {
                session->lastClientInfo.setOnline(false);
            }
        }
    }
    refreshProjectClientList();
}

void ApplicationRuntime::ensureRemoteSessionForClient(const ClientInfo& client) {
    if (m_cleanShutdownPrepared || !m_webSocketClient) {
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
    if (!binding.remoteSessionId.isEmpty()) {
        QString status = QStringLiteral("Disconnecting");
        if (binding.phase == QLatin1String("Active")) {
            status = QStringLiteral("Connected");
        } else if (binding.phase == QLatin1String("Opening")) {
            status = QStringLiteral("Connecting");
        } else if (binding.phase == QLatin1String("Grace")) {
            status = QStringLiteral("Reconnecting");
        } else if (binding.phase == QLatin1String("Closed")) {
            status = client.isOnline() ? QStringLiteral("Available")
                                       : QStringLiteral("Offline");
        }
        updateRemoteClientAvailability(targetEndpointId, status);
        if (m_sessionManager) {
            SessionManager::RemoteSessionState state =
                SessionManager::RemoteSessionState::Closing;
            if (binding.phase == QLatin1String("Active")) {
                state = SessionManager::RemoteSessionState::Active;
            } else if (binding.phase == QLatin1String("Opening")) {
                state = SessionManager::RemoteSessionState::Opening;
            } else if (binding.phase == QLatin1String("Grace")) {
                state = SessionManager::RemoteSessionState::Grace;
            } else if (binding.phase == QLatin1String("Closed")) {
                state = SessionManager::RemoteSessionState::Absent;
            }
            m_sessionManager->setRemoteSessionState(targetEndpointId, state);
            updateWorkspaceCapabilities(targetEndpointId);
        }
        const bool commandReady = binding.phase == QLatin1String("Active")
            && !m_locallyTerminatingRemoteSessions.contains(binding.remoteSessionId);
        const bool sessionClosable = (binding.phase == QLatin1String("Active")
                                      || binding.phase == QLatin1String("Grace"))
            && !m_locallyTerminatingRemoteSessions.contains(binding.remoteSessionId);
        if (m_activeSessionIdentity == targetEndpointId) {
            m_remoteClientConnected = binding.phase == QLatin1String("Active")
                || binding.phase == QLatin1String("Grace");
            setRemoteConnectionStatus(commandReady ? QStringLiteral("CONNECTED")
                                                   : status.toUpper(), false);
            Q_UNUSED(sessionClosable);
            Q_UNUSED(commandReady);
            updateWorkspaceCapabilities(targetEndpointId);
        }
        return;
    }

    if (m_remoteSessionOpenPendingTargets.contains(targetEndpointId)) {
        updateRemoteClientAvailability(targetEndpointId, QStringLiteral("Connecting"));
        if (m_activeSessionIdentity == targetEndpointId && m_uploadManager) {
            m_uploadManager->setTargetClientId(QString());
        }
        return;
    }
    if (m_remoteSessionOpenSuppressedTargets.contains(targetEndpointId)) {
        const QString status = client.isOnline()
            ? client.availabilityBadgeText() : QStringLiteral("Offline");
        updateRemoteClientAvailability(targetEndpointId, status);
        if (m_activeSessionIdentity == targetEndpointId) {
            setRemoteConnectionStatus(status.toUpper(), false);
            if (m_activeCanvas) m_activeCanvas->setOverlayActionsEnabled(false);
            if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
        }
        return;
    }

    const QString badge = client.availabilityBadgeText();
    const bool canOpen = client.isOnline()
        && badge != QLatin1String("In use")
        && badge != QLatin1String("Unavailable")
        && badge != QLatin1String("Disconnecting")
        && m_webSocketClient->isConnected();
    if (!canOpen) {
        const QString status = client.isOnline() ? badge : QStringLiteral("Offline");
        updateRemoteClientAvailability(targetEndpointId, status);
        m_remoteSessionOpenSuppressedTargets.insert(targetEndpointId);
        if (m_activeSessionIdentity == targetEndpointId) {
            setRemoteConnectionStatus(status.toUpper(), false);
            if (m_activeCanvas) m_activeCanvas->setOverlayActionsEnabled(false);
            if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
        }
        return;
    }

    QString requestId;
    if (!m_webSocketClient->openRemoteSession(targetEndpointId, &requestId)) {
        if (m_sessionManager) {
            m_sessionManager->setRemoteSessionState(
                targetEndpointId, SessionManager::RemoteSessionState::Absent);
        }
        updateRemoteClientAvailability(targetEndpointId, QStringLiteral("Unavailable"));
        if (m_activeSessionIdentity == targetEndpointId && m_uploadManager) {
            m_uploadManager->setTargetClientId(QString());
        }
        return;
    }
    m_remoteSessionOpenPendingTargets.insert(targetEndpointId);
    m_remoteSessionOpenTargetByRequestId.insert(requestId, targetEndpointId);
    updateRemoteClientAvailability(targetEndpointId, QStringLiteral("Connecting"));
    if (m_sessionManager) {
        m_sessionManager->setRemoteSessionState(
            targetEndpointId, SessionManager::RemoteSessionState::Opening);
        updateWorkspaceCapabilities(targetEndpointId);
    }
    if (m_activeSessionIdentity == targetEndpointId) {
        m_remoteClientConnected = false;
        setRemoteConnectionStatus(QStringLiteral("CONNECTING..."), false);
        if (m_activeCanvas) m_activeCanvas->setOverlayActionsEnabled(false);
        if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
    }
}

void ApplicationRuntime::handleRemoteSessionReady(const QJsonObject& envelope,
                                          bool resumed) {
    Q_UNUSED(resumed);
    if (!m_webSocketClient) return;
    const QString ownerEndpointId = envelope.value(QStringLiteral("ownerEndpointId")).toString();
    const QString targetEndpointId = envelope.value(QStringLiteral("targetEndpointId")).toString();
    const QString localEndpointId = m_webSocketClient->endpointId();
    const QString peerEndpointId = ownerEndpointId == localEndpointId
        ? targetEndpointId
        : (targetEndpointId == localEndpointId ? ownerEndpointId : QString());
    if (peerEndpointId.isEmpty()) return;

    // A response to an in-flight open (or a new incoming session) may cross
    // the quit request. Never let it resurrect command-capable state while the
    // transport is being drained: include it in the same idempotent close set.
    if (m_cleanShutdownPrepared) {
        const QString remoteSessionId =
            envelope.value(QStringLiteral("remoteSessionId")).toString();
        if (!remoteSessionId.isEmpty()) {
            m_cleanShutdownPendingSessionIds.insert(remoteSessionId);
            m_locallyTerminatingRemoteSessions.insert(remoteSessionId);
            m_webSocketClient->closeRemoteSession(
                remoteSessionId, nullptr, QStringLiteral("clean_shutdown"));
            if (targetEndpointId == localEndpointId && g_remoteSceneController) {
                g_remoteSceneController->teardownRemoteSession(remoteSessionId);
            }
        }
        return;
    }

    const QString requestId = envelope.value(QStringLiteral("requestId")).toString();
    if (!requestId.isEmpty()) m_remoteSessionOpenTargetByRequestId.remove(requestId);
    m_remoteSessionOpenPendingTargets.remove(peerEndpointId);
    m_remoteSessionOpenSuppressedTargets.remove(peerEndpointId);
    m_locallyTerminatingRemoteSessions.remove(
        envelope.value(QStringLiteral("remoteSessionId")).toString());
    if (ownerEndpointId != localEndpointId) {
        return; // Incoming sessions do not create a local Project.
    }

    if (m_sessionManager) {
        m_sessionManager->setRemoteSessionState(
            peerEndpointId, SessionManager::RemoteSessionState::Active);
    }
    updateRemoteClientAvailability(peerEndpointId, QStringLiteral("Connected"));
    if (m_activeSessionIdentity == peerEndpointId) {
        m_remoteClientConnected = true;
        setRemoteConnectionStatus(QStringLiteral("CONNECTED"), false);
        updateWorkspaceCapabilities(peerEndpointId);

        // The discovery snapshot may already have supplied the target screen
        // topology, but it is this authenticated event that makes the initial
        // canvas connection ready.  Keep the full loader visible until here.
        CanvasSession* session = m_sessionManager
            ? m_sessionManager->findSession(peerEndpointId) : nullptr;
        if (session && session->canvas) {
            session->canvas->setScreens(session->lastClientInfo.getScreens());
            if (m_projectManager
                && m_projectManager->hasProjectForTarget(peerEndpointId)) {
                m_projectManager->updateSavedScreens(
                    peerEndpointId, session->lastClientInfo.getScreens());
            }
            m_remoteVolumePercent = session->lastClientInfo.getVolumePercent();
        }
        if (session && session->canvas && session->canvas->hasActiveScreens()
            && !m_canvasRevealedForCurrentClient) {
            if (m_navigationManager) m_navigationManager->revealCanvas();
            session->canvas->requestDeferredInitialRecenter(53);
            if (!m_preserveViewportOnReconnect) {
                session->canvas->recenterWithMargin(53);
            }
            m_preserveViewportOnReconnect = false;
            m_canvasRevealedForCurrentClient = true;
            m_canvasContentEverLoaded = true;
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
    QString status;
    if (phase == QLatin1String("Grace") || state == QLatin1String("Grace")
        || state == QLatin1String("Degraded")
        || envelope.value(QStringLiteral("degraded")).toBool()) {
        status = QStringLiteral("Reconnecting");
    } else if (phase == QLatin1String("Active") && state == QLatin1String("Active")) {
        status = QStringLiteral("Connected");
    } else if (phase == QLatin1String("Terminating")
               || phase == QLatin1String("CleanupPending")) {
        status = QStringLiteral("Disconnecting");
    }
    if (status.isEmpty()) return;
    updateRemoteClientAvailability(targetEndpointId, status);
    const bool commandReady = status == QLatin1String("Connected")
        && !m_locallyTerminatingRemoteSessions.contains(remoteSessionId);
    const bool sessionClosable = (phase == QLatin1String("Active")
                                  || phase == QLatin1String("Grace"))
        && !m_locallyTerminatingRemoteSessions.contains(remoteSessionId);
    if (m_sessionManager) {
        SessionManager::RemoteSessionState workspaceState =
            SessionManager::RemoteSessionState::Closing;
        if (status == QLatin1String("Connected")) {
            workspaceState = SessionManager::RemoteSessionState::Active;
        } else if (status == QLatin1String("Reconnecting")) {
            workspaceState = SessionManager::RemoteSessionState::Grace;
        }
        m_sessionManager->setRemoteSessionState(targetEndpointId,
                                                workspaceState);
        updateWorkspaceCapabilities(targetEndpointId);
    }
    if (m_activeSessionIdentity == targetEndpointId) {
        m_remoteClientConnected = phase == QLatin1String("Active")
            || phase == QLatin1String("Grace");
        setRemoteConnectionStatus(status.toUpper(), false);
        Q_UNUSED(sessionClosable);
        Q_UNUSED(commandReady);
        if (status != QLatin1String("Connected")) m_remoteVolumePercent = -1;
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
    m_locallyTerminatingRemoteSessions.insert(remoteSessionId);

    if (ownerEndpointId == m_webSocketClient->endpointId()) {
        if (m_activeSessionIdentity == targetEndpointId) m_remoteClientConnected = false;
        updateRemoteClientAvailability(targetEndpointId, QStringLiteral("Disconnecting"));
        clearRemoteSessionRuntimeState(targetEndpointId, false);
        return;
    }
    if (targetEndpointId != m_webSocketClient->endpointId()) return;

    // Duplicate terminal envelopes replay the result already being built or
    // acknowledged. They must never enter renderer destruction or quarantine
    // a second time.
    const auto acknowledged = m_pendingTeardownAcks.constFind(remoteSessionId);
    if (acknowledged != m_pendingTeardownAcks.cend()) {
        if (acknowledged->teardownId == teardownId) retryPendingTeardownAcks();
        return;
    }
    const auto pending = m_pendingRendererTeardowns.constFind(remoteSessionId);
    if (pending != m_pendingRendererTeardowns.cend()) return;

    // Target-side commit order is enforced asynchronously: renderer objects
    // are stopped/detached now, but cache quarantine is forbidden until their
    // actual QObject/native-window destruction completes.
    PendingRendererTeardown rendererTeardown;
    rendererTeardown.ownerEndpointId = ownerEndpointId;
    rendererTeardown.teardownId = teardownId;
    rendererTeardown.generation = generation;
    m_pendingRendererTeardowns.insert(remoteSessionId, rendererTeardown);
    const bool accepted = g_remoteSceneController
        && g_remoteSceneController->teardownRemoteSession(remoteSessionId);
    if (!accepted) {
        handleRemoteRendererTeardownSettled(remoteSessionId, false);
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
    if (reasonCode != QLatin1String("incoming_session_orphan_timeout")) {
        // Lease/server terminal events supersede a narrower local watchdog
        // cleanup and must quarantine every incoming scope.
        m_terminalIncomingSessionFilter.clear();
    }
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
        if (!g_remoteSceneController
            || !g_remoteSceneController->teardownRemoteSession(
                binding.remoteSessionId)) {
            // Keep the id pending and the receiver unadvertised. Clearing it
            // here would permit cache quarantine without renderer settlement.
            qCritical() << "Terminal renderer teardown could not start for"
                        << binding.remoteSessionId;
        }
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
        if (!g_remoteSceneController
            || !g_remoteSceneController->teardownRemoteSession(
                remoteSessionId)) {
            qCritical() << "Captured orphan renderer teardown could not start for"
                        << remoteSessionId;
        }
    }
    finishTerminalIncomingCacheCleanupIfReady();
}

void ApplicationRuntime::finishTerminalIncomingCacheCleanupIfReady()
{
    if (!m_terminalIncomingCleanupActive
        || m_terminalIncomingCacheTeardownStarted
        || !m_terminalRendererPendingSessionIds.isEmpty()
        || !m_uploadManager) {
        return;
    }

    m_terminalIncomingCacheTeardownStarted = true;
    const UploadManager::BulkTeardownResult cleanup =
        m_uploadManager->completeTerminalIncomingCleanup(
            m_terminalIncomingCleanupReason,
            m_terminalIncomingSessionFilter);
    if (!cleanup.allLogicallyCommitted()) {
        qCritical() << "Terminal incoming cache cleanup remains pending:"
                    << cleanup.errorCode
                    << "failed scopes" << cleanup.cleanupErrorScopes;
    }
    m_terminalIncomingSessionFilter.clear();
    m_terminalIncomingCleanupActive = false;
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
    for (const RemoteSessionCoordinator::Binding& binding : coordinator->all()) {
        if (binding.targetEndpointId == localEndpointId
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
    if (remoteSessionId.isEmpty()) return;
    const bool terminalTransportBarrier =
        m_terminalRendererPendingSessionIds.remove(remoteSessionId) > 0;
    const bool cleanShutdownBarrier =
        m_cleanShutdownRendererPendingSessionIds.remove(remoteSessionId) > 0;
    const auto pending = m_pendingRendererTeardowns.find(remoteSessionId);
    if (pending == m_pendingRendererTeardowns.end()) {
        if (terminalTransportBarrier) {
            finishTerminalIncomingCacheCleanupIfReady();
        }
        if (cleanShutdownBarrier) {
            finishCleanShutdownIncomingCacheTeardownIfReady();
        }
        return;
    }
    const PendingRendererTeardown teardown = pending.value();
    m_pendingRendererTeardowns.erase(pending);

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

    PendingTeardownAck ack;
    ack.teardownId = teardown.teardownId;
    ack.sceneStopped = sceneStopped;
    ack.uploadsAborted = uploadsAborted;
    ack.cacheQuarantined = cacheResult.acknowledgementSafe();
    ack.removedFileCount = removedFileCount;
    ack.errorCode = cacheResult.errorCode;
    ack.quarantinedBytes = cacheResult.quarantinedBytes;
    m_pendingTeardownAcks.insert(remoteSessionId, ack);
    retryPendingTeardownAcks();

    if (!ack.sceneStopped || !ack.uploadsAborted || !ack.cacheQuarantined) {
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
    if (cleanShutdownBarrier) {
        finishCleanShutdownIncomingCacheTeardownIfReady();
    }
    if (terminalTransportBarrier) {
        finishTerminalIncomingCacheCleanupIfReady();
    }
}

void ApplicationRuntime::retryPendingTeardownAcks() {
    if (!m_webSocketClient || !m_webSocketClient->isConnected()) return;
    for (auto it = m_pendingTeardownAcks.cbegin();
         it != m_pendingTeardownAcks.cend(); ++it) {
        const PendingTeardownAck& ack = it.value();
        m_webSocketClient->acknowledgeRemoteSessionTeardown(
            it.key(), ack.teardownId, ack.sceneStopped, ack.uploadsAborted,
            ack.cacheQuarantined, ack.removedFileCount, ack.errorCode,
            ack.quarantinedBytes);
    }
}

void ApplicationRuntime::clearRemoteSessionRuntimeState(const QString& targetEndpointId,
                                                bool connectionLost) {
    CanvasSession* session = m_sessionManager
        ? m_sessionManager->findSession(targetEndpointId) : nullptr;
    if (!session) return;
    if (session->canvas) {
        if (connectionLost) session->canvas->handleRemoteConnectionLost();
        else session->canvas->stopScenesForSourceInvalidation();
    }
    session->knownRemoteFileIds.clear();
    session->expectedIdeaFileIds.clear();
    session->remoteContentClearedOnDisconnect = true;
    if (session->canvas) {
        for (CanvasMedia* media : session->canvas->enumerateMediaItems()) {
            if (media) media->setUploadNotUploaded();
        }
    }
    clearUploadTracking(*session);
    if (m_sessionManager) {
        m_sessionManager->setRemoteSessionState(
            targetEndpointId, SessionManager::RemoteSessionState::Absent);
    }
    if (m_activeSessionIdentity == targetEndpointId && m_uploadManager) {
        // RemoteSession terminal envelopes invalidate only their own upload.
        // onConnectionLost() is transport-wide and would suspend unrelated
        // concurrent uploads to other devices indefinitely.
        m_uploadManager->setTargetClientId(QString());
    }
    if (m_activeSessionIdentity == targetEndpointId) {
        m_remoteClientConnected = false;
        m_remoteVolumePercent = -1;
        setRemoteConnectionStatus(
            session->lastClientInfo.isOnline()
                ? QStringLiteral("AVAILABLE") : QStringLiteral("DISCONNECTED"),
            false);
    }
    updateWorkspaceCapabilities(targetEndpointId);
    destroyWorkspaceCanvasIfUnused(targetEndpointId);
    if (m_activeSessionIdentity == targetEndpointId) {
        emit activeSessionChanged(targetEndpointId);
    }
}

void ApplicationRuntime::handleRemoteSessionClosed(const QJsonObject& envelope) {
    if (!m_webSocketClient) return;
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    if (m_cleanShutdownPrepared) {
        m_cleanShutdownPendingSessionIds.remove(remoteSessionId);
        // Defer final disconnect until every slot handling this envelope has
        // returned, in particular the target-side teardown/ACK handlers.
        QTimer::singleShot(0, this, &ApplicationRuntime::maybeFinishCleanShutdown);
    }
    const QString ownerEndpointId = envelope.value(QStringLiteral("ownerEndpointId")).toString();
    const QString targetEndpointId = envelope.value(QStringLiteral("targetEndpointId")).toString();
    m_pendingRendererTeardowns.remove(remoteSessionId);
    m_cleanShutdownRendererPendingSessionIds.remove(remoteSessionId);
    m_pendingTeardownAcks.remove(remoteSessionId);
    m_locallyTerminatingRemoteSessions.remove(remoteSessionId);
    if (m_incomingSessionOrphanWatchdog
        && m_incomingSessionOrphanWatchdog->officiallyClosed(
            remoteSessionId)) {
        m_incomingOrphanSessionIds.remove(remoteSessionId);
    }
    if (ownerEndpointId != m_webSocketClient->endpointId()) return;

    // A terminal lease/peer close is never converted into an implicit new
    // session when discovery later reports the device again.
    m_remoteSessionOpenSuppressedTargets.insert(targetEndpointId);
    clearRemoteSessionRuntimeState(targetEndpointId, false);
    if (m_activeSessionIdentity == targetEndpointId) m_remoteClientConnected = false;
    bool online = false;
    ClientInfo current;
    for (const ClientInfo& client : m_discoveredClients) {
        if (client.endpointId() == targetEndpointId) {
            current = client;
            online = client.isOnline();
            break;
        }
    }
    if (online) {
        current.setStatus(QStringLiteral("Available"));
        current.setAvailabilityStatus(QStringLiteral("Available"));
    }
    updateRemoteClientAvailability(
        targetEndpointId, online ? QStringLiteral("Available")
                               : QStringLiteral("Offline"));

    m_closingActiveSession = false;
}

void ApplicationRuntime::handleRemoteSessionError(const QJsonObject& envelope) {
    if (m_cleanShutdownPrepared) {
        // A duplicate/late close rejection cannot change a terminal local
        // shutdown. Keep draining any other correlated sessions until timeout.
        return;
    }
    const QString code = envelope.value(QStringLiteral("code")).toString();
    const QString requestId = envelope.value(QStringLiteral("requestId")).toString();
    QString targetEndpointId =
        envelope.value(QStringLiteral("targetEndpointId")).toString();
    if (targetEndpointId.isEmpty() && !requestId.isEmpty()) {
        targetEndpointId = m_remoteSessionOpenTargetByRequestId.value(requestId);
    }
    if (targetEndpointId.isEmpty()
        && m_remoteSessionOpenPendingTargets.size() == 1) {
        targetEndpointId = *m_remoteSessionOpenPendingTargets.cbegin();
    }
    if (!requestId.isEmpty()) m_remoteSessionOpenTargetByRequestId.remove(requestId);
    if (!targetEndpointId.isEmpty()) {
        m_remoteSessionOpenPendingTargets.remove(targetEndpointId);
        m_remoteSessionOpenSuppressedTargets.insert(targetEndpointId);
        if (m_sessionManager) {
            m_sessionManager->setRemoteSessionState(
                targetEndpointId, SessionManager::RemoteSessionState::Absent);
            updateWorkspaceCapabilities(targetEndpointId);
            destroyWorkspaceCanvasIfUnused(targetEndpointId);
        }
    }

    QString status = QStringLiteral("Unavailable");
    if (code == QLatin1String("target_in_use")) status = QStringLiteral("In use");
    else if (code == QLatin1String("target_offline")) status = QStringLiteral("Offline");
    else if (code == QLatin1String("cleanup_not_committed")) status = QStringLiteral("Unavailable");
    else if (!targetEndpointId.isEmpty()) status = QStringLiteral("Available");
    if (!targetEndpointId.isEmpty()) {
        updateRemoteClientAvailability(targetEndpointId, status);
        if (m_activeSessionIdentity == targetEndpointId) {
            setRemoteConnectionStatus(status.toUpper(), false);
            m_remoteVolumePercent = -1;
            if (m_activeCanvas) m_activeCanvas->setOverlayActionsEnabled(false);

            // A rejected open is terminal for this attempt.  Do not leave an
            // indeterminate spinner running forever; reveal the known local
            // project/topology with remote actions disabled.
            if (m_navigationManager) m_navigationManager->revealCanvas();
        }
    }
    if (m_toastSystem) {
        NotificationRequest notification;
        notification.severity = NotificationSeverity::Warning;
        notification.category = QStringLiteral("Remote session");
        notification.message = code == QLatin1String("target_in_use")
            ? QStringLiteral("This client is already controlled by another device.")
            : envelope.value(QStringLiteral("message"))
                  .toString(QStringLiteral("The remote session command was rejected."));
        notification.correlationId =
            envelope.value(QStringLiteral("messageId")).toString();
        m_toastSystem->publishNotification(notification);
    }
}

void ApplicationRuntime::terminateProjectRemoteSession(const QString& targetEndpointId,
                                               bool attemptRemote) {
    CanvasSession* session = m_sessionManager
        ? m_sessionManager->findSession(targetEndpointId) : nullptr;
    if (session && session->canvas) {
        if (attemptRemote) session->canvas->stopScenesForSourceInvalidation();
        else session->canvas->handleRemoteConnectionLost();
    }

    RemoteSessionCoordinator* coordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    const RemoteSessionCoordinator::Binding binding = coordinator
        ? coordinator->outgoingForPeer(targetEndpointId)
        : RemoteSessionCoordinator::Binding();
    if (attemptRemote && m_webSocketClient
        && !binding.remoteSessionId.isEmpty()
        && binding.ownerEndpointId == m_webSocketClient->endpointId()
        && binding.phase != QLatin1String("Terminating")
        && binding.phase != QLatin1String("CleanupPending")) {
        m_locallyTerminatingRemoteSessions.insert(binding.remoteSessionId);
        updateRemoteClientAvailability(targetEndpointId, QStringLiteral("Disconnecting"));
        if (m_uploadManager && m_uploadManager->canRequestCancel()) {
            m_uploadManager->requestCancel();
        }
        m_webSocketClient->closeRemoteSession(binding.remoteSessionId);
    }

    // The workspace is updated synchronously. The protocol close and target
    // cleanup above are best effort and may settle after the local UI has
    // already become session-less.
    clearRemoteSessionRuntimeState(targetEndpointId, !attemptRemote);
}

void ApplicationRuntime::removeRuntimeCanvasSession(const QString& targetEndpointId) {
    if (!m_sessionManager) {
        return;
    }
    CanvasSession* session = m_sessionManager->findSession(targetEndpointId);
    if (!session) {
        m_restoredProjectIds.remove(targetEndpointId);
        return;
    }

    ICanvasHost* canvas = session->canvas;
    const QString ideaId = session->canvasSessionId;
    if (canvas) {
        for (CanvasMedia* media : canvas->enumerateMediaItems()) {
            if (m_fileWatcher && media) {
                m_fileWatcher->unwatchMediaItem(media);
            }
        }
    }
    clearUploadTracking(*session);
    if (m_fileManager && !ideaId.isEmpty()) {
        m_fileManager->removeIdeaAssociations(ideaId);
    }
    if (m_activeSessionIdentity == targetEndpointId) {
        m_activeSessionIdentity.clear();
        m_activeRemoteClientId.clear();
        m_activeCanvas = nullptr;
    }
    m_sessionManager->deleteSession(targetEndpointId);
    if (canvas) canvas->deleteLater();
    m_restoredProjectIds.remove(targetEndpointId);
}

void ApplicationRuntime::onDisconnectProjectRequested() {
    const QString targetEndpointId = m_activeSessionIdentity;
    if (targetEndpointId.isEmpty() || !activeRemoteSessionExists()) {
        return;
    }

    persistProjectCanvas(targetEndpointId);
    if (m_sessionManager) {
        m_sessionManager->setRemoteSessionState(
            targetEndpointId, SessionManager::RemoteSessionState::Closing);
    }
    terminateProjectRemoteSession(targetEndpointId, true);
    m_closingActiveSession = false;
    emit activeSessionChanged(targetEndpointId);
    emit presentationStateChanged();
}

void ApplicationRuntime::onDeleteProjectRequested() {
    deleteActiveProjectConfirmed();
}

void ApplicationRuntime::deleteActiveProjectConfirmed() {
    const QString targetEndpointId = m_activeSessionIdentity;
    if (!m_projectManager || targetEndpointId.isEmpty()
        || !activeProjectExists()) {
        return;
    }
    if (!m_projectManager->deleteProject(targetEndpointId)) {
        TOAST_ERROR(QStringLiteral("The project could not be deleted"), 4000);
        return;
    }
    emit activeSessionChanged(targetEndpointId);
    emit presentationStateChanged();
}

void ApplicationRuntime::clearDeletedProjectFromWorkspace(
    const QString& targetEndpointId)
{
    CanvasSession* workspace = m_sessionManager
        ? m_sessionManager->findSession(targetEndpointId) : nullptr;
    if (!workspace) {
        m_restoredProjectIds.remove(targetEndpointId);
        return;
    }

    const QString formerProjectId = workspace->canvasSessionId;
    if (workspace->canvas) {
        workspace->canvas->stopScenesForSourceInvalidation();
        const QList<CanvasMedia*> media = workspace->canvas->enumerateMediaItems();
        for (CanvasMedia* item : media) {
            if (m_fileWatcher && item) m_fileWatcher->unwatchMediaItem(item);
        }
        if (workspace->canvas->document()) workspace->canvas->document()->clear();
    }
    for (const QString& fileId : std::as_const(workspace->knownRemoteFileIds)) {
        if (m_uploadManager) {
            m_uploadManager->requestAssetRemoval(
                targetEndpointId, fileId, QStringLiteral("project_deleted"));
        }
    }
    clearUploadTracking(*workspace);
    if (m_fileManager && !formerProjectId.isEmpty()) {
        m_fileManager->removeIdeaAssociations(formerProjectId);
    }

    const QString ephemeralDocumentId = createIdeaId();
    m_sessionManager->updateSessionIdeaId(targetEndpointId,
                                          ephemeralDocumentId);
    workspace = m_sessionManager->findSession(targetEndpointId);
    if (workspace && workspace->canvas) {
        workspace->canvas->setActiveIdeaId(ephemeralDocumentId);
    }
    m_restoredProjectIds.remove(targetEndpointId);
    updateWorkspaceCapabilities(targetEndpointId);
    destroyWorkspaceCanvasIfUnused(targetEndpointId);
    if (m_activeSessionIdentity == targetEndpointId) {
        emit activeSessionChanged(targetEndpointId);
        emit presentationStateChanged();
    }
}

void ApplicationRuntime::destroyWorkspaceCanvasIfUnused(
    const QString& targetEndpointId)
{
    if (!m_sessionManager) return;
    CanvasSession* workspace = m_sessionManager->findSession(targetEndpointId);
    if (!workspace || !workspace->canvas) return;
    const bool hasProject = m_projectManager
        && m_projectManager->hasProjectForTarget(targetEndpointId);
    const SessionManager::RemoteSessionState state =
        m_sessionManager->remoteSessionState(targetEndpointId);
    const bool hasUsableRemoteSession =
        state == SessionManager::RemoteSessionState::Opening
        || state == SessionManager::RemoteSessionState::Active
        || state == SessionManager::RemoteSessionState::Grace;
    if (hasProject || hasUsableRemoteSession) return;

    ICanvasHost* canvas = workspace->canvas;
    workspace->canvas = nullptr;
    workspace->connectionsInitialized = false;
    if (m_activeSessionIdentity == targetEndpointId) {
        m_activeCanvas = nullptr;
        if (m_navigationManager) m_navigationManager->setActiveCanvas(nullptr);
        if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
    }
    canvas->deleteLater();
}

void ApplicationRuntime::updateWorkspaceCapabilities(
    const QString& targetEndpointId)
{
    CanvasSession* workspace = m_sessionManager
        ? m_sessionManager->findSession(targetEndpointId) : nullptr;
    if (!workspace || !workspace->canvas) return;
    const bool hasProject = m_projectManager
        && m_projectManager->hasProjectForTarget(targetEndpointId);
    const bool remoteActive = workspace->remoteSessionState
        == SessionManager::RemoteSessionState::Active;
    workspace->canvas->setProjectEditingEnabled(hasProject);
    workspace->canvas->setOverlayActionsEnabled(remoteActive);
    if (m_activeSessionIdentity == targetEndpointId && m_uploadManager) {
        m_uploadManager->setTargetClientId(
            hasProject && remoteActive ? targetEndpointId : QString());
        if (hasProject) {
            m_uploadManager->setActiveIdeaId(workspace->canvasSessionId);
        }
        emit m_uploadManager->uiStateChanged();
    }
}

void ApplicationRuntime::setActiveProjectVisibleIfAppropriate() {
    if (!m_projectManager || m_activeSessionIdentity.isEmpty()) {
        return;
    }
    const bool canvasShown = m_navigationManager
        && m_navigationManager->isOnScreenView();
    const bool visible = canvasShown && m_qmlWindowVisible
        && !m_applicationSuspended;
    if (visible) {
        m_projectManager->setVisible(m_activeSessionIdentity);
    } else {
        persistProjectCanvas(m_activeSessionIdentity);
        m_projectManager->setHidden(m_activeSessionIdentity);
    }
}

void ApplicationRuntime::setApplicationSuspended(bool suspended) {
    if (m_applicationSuspended == suspended) {
        updateHistoryVisibilityState();
        return;
    }
    m_applicationSuspended = suspended;
    setActiveProjectVisibleIfAppropriate();
    if (m_sessionManager && !m_activeSessionIdentity.isEmpty()) {
        const bool workspaceVisible = !suspended && m_qmlWindowVisible
            && m_navigationManager && m_navigationManager->isOnScreenView();
        if (workspaceVisible) {
            m_sessionManager->setWorkspaceVisible(m_activeSessionIdentity);
        } else {
            m_sessionManager->setWorkspaceHidden(m_activeSessionIdentity);
        }
    }
    updateHistoryVisibilityState();
}

void ApplicationRuntime::setSelectedClient(const ClientInfo& client)
{
    m_selectedClient = client;
    updateClientNameDisplay(client);
}

void ApplicationRuntime::updateVolumeIndicator()
{
    m_remoteVolumePercent = m_selectedClient.getVolumePercent();
    emit presentationStateChanged();
}

void ApplicationRuntime::setRemoteClientState(const RemoteClientState& state,
                                              bool propagateLoss)
{
    m_remoteClientConnected =
        state.connectionStatus == RemoteClientState::Connected
        || state.connectionStatus == RemoteClientState::Reconnecting;
    m_remoteBusy = state.spinnerActive;
    m_remoteStatusText = state.statusText().trimmed().toUpper();
    m_selectedClient = state.clientInfo;
    m_remoteDisplayName = state.clientInfo.getMachineName().trimmed();
    m_remoteVolumePercent = state.volumeVisible ? state.volumePercent : -1;
    refreshOverlayActionsState(
        state.connectionStatus == RemoteClientState::Connected,
        propagateLoss);
    emit presentationStateChanged();
}

void ApplicationRuntime::onUploadButtonClicked()
{
    if (!activeProjectExists() || !m_sessionManager
        || m_sessionManager->remoteSessionState(m_activeSessionIdentity)
            != SessionManager::RemoteSessionState::Active) {
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
    if (CanvasSession* session = findCanvasSession(activity.peerEndpointId)) {
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
    return m_remoteDisplayName.isEmpty()
        ? m_selectedClient.getMachineName() : m_remoteDisplayName;
}

bool ApplicationRuntime::canCloseActiveSession() const
{
    if (m_activeSessionIdentity.isEmpty() || m_closingActiveSession
        || !activeRemoteSessionExists()) return false;
    const RemoteSessionCoordinator* coordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    return coordinator
        && !coordinator->outgoingForPeer(m_activeSessionIdentity)
                .remoteSessionId.isEmpty();
}

bool ApplicationRuntime::canDeleteActiveProject() const
{
    return m_projectManager && !m_activeSessionIdentity.isEmpty()
        && m_projectManager->hasProjectForTarget(m_activeSessionIdentity);
}

bool ApplicationRuntime::activeProjectExists() const
{
    return m_projectManager && !m_activeSessionIdentity.isEmpty()
        && m_projectManager->hasProjectForTarget(m_activeSessionIdentity);
}

bool ApplicationRuntime::activeRemoteSessionExists() const
{
    if (!m_sessionManager || m_activeSessionIdentity.isEmpty()) return false;
    const SessionManager::RemoteSessionState state =
        m_sessionManager->remoteSessionState(m_activeSessionIdentity);
    return state == SessionManager::RemoteSessionState::Opening
        || state == SessionManager::RemoteSessionState::Active
        || state == SessionManager::RemoteSessionState::Grace;
}

bool ApplicationRuntime::canCreateActiveProject() const
{
    return !m_activeSessionIdentity.isEmpty() && !activeProjectExists();
}

bool ApplicationRuntime::canLaunchActiveSession() const
{
    if (m_cleanShutdownPrepared || m_activeSessionIdentity.isEmpty()
        || activeRemoteSessionExists() || !m_webSocketClient
        || !m_webSocketClient->isConnected()) {
        return false;
    }
    const CanvasSession* workspace = m_sessionManager
        ? m_sessionManager->findSession(m_activeSessionIdentity) : nullptr;
    if (!workspace || !workspace->lastClientInfo.isOnline()
        || workspace->remoteSessionState
            != SessionManager::RemoteSessionState::Absent) return false;
    const RemoteSessionCoordinator* coordinator =
        m_webSocketClient->remoteSessionCoordinator();
    if (coordinator
        && !coordinator->outgoingForPeer(m_activeSessionIdentity)
                .remoteSessionId.isEmpty()) {
        return false;
    }
    const QString badge = workspace->lastClientInfo.availabilityBadgeText();
    return badge != QLatin1String("In use")
        && badge != QLatin1String("Unavailable")
        && badge != QLatin1String("Disconnecting");
}

void ApplicationRuntime::setQmlWindowVisible(bool visible)
{
    if (m_qmlWindowVisible == visible) return;
    m_qmlWindowVisible = visible;
    setActiveProjectVisibleIfAppropriate();
    if (m_sessionManager && !m_activeSessionIdentity.isEmpty()) {
        const bool workspaceVisible = visible && !m_applicationSuspended
            && m_navigationManager && m_navigationManager->isOnScreenView();
        if (workspaceVisible) {
            m_sessionManager->setWorkspaceVisible(m_activeSessionIdentity);
        } else {
            m_sessionManager->setWorkspaceHidden(m_activeSessionIdentity);
        }
    }
    updateHistoryVisibilityState();
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

void ApplicationRuntime::closeActiveSession()
{
    onDisconnectProjectRequested();
}

void ApplicationRuntime::createActiveProject()
{
    const QString targetEndpointId = m_activeSessionIdentity;
    if (!canCreateActiveProject() || !m_projectManager || !m_sessionManager) return;
    CanvasSession* workspace = m_sessionManager->findSession(targetEndpointId);
    if (!workspace) return;

    const QString projectId = m_projectManager->ensureProject(
        ProjectTargetReference::fromClientInfo(workspace->lastClientInfo),
        ProjectLifecycleState::Visible);
    if (projectId.isEmpty()) {
        TOAST_ERROR(QStringLiteral("The project could not be created"), 4000);
        return;
    }
    m_sessionManager->updateSessionIdeaId(targetEndpointId, projectId);
    workspace = m_sessionManager->findSession(targetEndpointId);
    if (!workspace) return;
    if (!workspace->canvas) ensureCanvasSession(workspace->lastClientInfo);
    workspace = m_sessionManager->findSession(targetEndpointId);
    if (workspace && workspace->canvas) {
        workspace->canvas->setActiveIdeaId(projectId);
        workspace->canvas->setProjectEditingEnabled(true);
        if (!activeRemoteSessionExists()) workspace->canvas->setScreens({});
        switchToCanvasSession(targetEndpointId);
        persistProjectCanvas(targetEndpointId);
        if (m_navigationManager) m_navigationManager->revealCanvas();
    }
    m_restoredProjectIds.insert(targetEndpointId);
    updateWorkspaceCapabilities(targetEndpointId);
    m_canvasRevealedForCurrentClient = workspace && workspace->canvas;
    emit activeSessionChanged(targetEndpointId);
    emit presentationStateChanged();
}

void ApplicationRuntime::launchActiveSession()
{
    const QString targetEndpointId = m_activeSessionIdentity;
    if (!canLaunchActiveSession() || !m_sessionManager) return;
    CanvasSession* workspace = m_sessionManager->findSession(targetEndpointId);
    if (!workspace) return;
    if (!workspace->canvas) ensureCanvasSession(workspace->lastClientInfo);
    m_remoteSessionOpenSuppressedTargets.remove(targetEndpointId);
    m_sessionManager->setRemoteSessionState(
        targetEndpointId, SessionManager::RemoteSessionState::Opening);
    markCanvasLoadRequest(targetEndpointId);
    switchToCanvasSession(targetEndpointId);
    updateWorkspaceCapabilities(targetEndpointId);
    ensureRemoteSessionForClient(workspace->lastClientInfo);
    emit activeSessionChanged(targetEndpointId);
    emit presentationStateChanged();
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

    if (m_webSocketClient && m_webSocketMessageHandler) {
        QObject::disconnect(m_webSocketClient, nullptr,
                            m_webSocketMessageHandler, nullptr);
    }
    if (m_sessionManager && m_projectManager) {
        for (CanvasSession* session : m_sessionManager->getAllSessions()) {
            if (!session) continue;
            if (session->canvas) session->canvas->stopScenesForSourceInvalidation();
            if (m_projectManager->hasProjectForTarget(session->persistentClientId)) {
                persistProjectCanvas(session->persistentClientId);
            }
        }
        m_projectManager->markAllHidden(QDateTime::currentMSecsSinceEpoch());
        m_projectManager->flush();
    }

    const QString localEndpointId = m_webSocketClient
        ? m_webSocketClient->endpointId() : QString();
    RemoteSessionCoordinator* coordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    const auto bindings = coordinator ? coordinator->all()
                                      : QList<RemoteSessionCoordinator::Binding>();
    for (const auto& binding : bindings) {
        if (binding.remoteSessionId.isEmpty()) continue;
        if (m_webSocketClient && m_webSocketClient->isConnected()) {
            m_webSocketClient->closeRemoteSession(
                binding.remoteSessionId, nullptr,
                QStringLiteral("clean_shutdown"));
        }
        if (binding.targetEndpointId == localEndpointId
            && g_remoteSceneController) {
            g_remoteSceneController->teardownRemoteSession(
                binding.remoteSessionId);
        }
    }
    if (m_uploadManager) {
        m_uploadManager->teardownAllIncomingRemoteSessions(
            QStringLiteral("clean_shutdown"));
    }
    if (m_sceneActivityModel) m_sceneActivityModel->clear();
}

void ApplicationRuntime::finishCleanShutdownIncomingCacheTeardownIfReady()
{
    if (m_uploadManager) {
        m_uploadManager->teardownAllIncomingRemoteSessions(
            QStringLiteral("clean_shutdown"));
    }
}

void ApplicationRuntime::maybeFinishCleanShutdown()
{
    if (m_cleanShutdownPrepared) finishCleanShutdown();
}

void ApplicationRuntime::finishCleanShutdown()
{
    if (m_cleanShutdownFinished) return;
    m_cleanShutdownFinished = true;
    if (m_connectionManager) m_connectionManager->disconnect();
    else if (m_webSocketClient) m_webSocketClient->disconnect();
    if (m_cleanShutdownQuitRequested) {
        QTimer::singleShot(0, QCoreApplication::instance(),
                           &QCoreApplication::quit);
    }
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
        if (m_qmlWindowVisible) emit qmlHideRequested();
        else emit qmlRaiseRequested();
        break;
    default:
        break;
    }
}

void ApplicationRuntime::onEnableDisableClicked() {
    if (!m_connectionManager) return;
    
    if (!m_userDisconnected) {
        m_userDisconnected = true;
        m_connectionManager->disconnect();
    } else {
        m_userDisconnected = false;
        connectToServer();
    }
    emit presentationStateChanged();
}

// Settings dialog: server URL with Save/Cancel

// (Removed stray duplicated code block previously injected)

void ApplicationRuntime::resetAllSessionUploadStates() {
    for (CanvasSession* session : m_sessionManager->getAllSessions()) {
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
    m_uploadSessionByUploadId.clear();
    m_activeUploadSessionIdentity.clear();
}

// [PHASE 7.1] Simplified - delegates to WebSocketMessageHandler
void ApplicationRuntime::onConnected() {
    if (m_webSocketMessageHandler) {
        m_webSocketMessageHandler->onConnected();
    }
}

// [PHASE 7.1] Simplified - delegates to WebSocketMessageHandler  
void ApplicationRuntime::onDisconnected() {
    if (m_webSocketMessageHandler) {
        m_webSocketMessageHandler->onDisconnected();
    }
    if (m_cleanShutdownPrepared && !m_cleanShutdownFinished) {
        finishCleanShutdown();
    }
}

// Legacy screen watching/cursor streaming was removed by protocol v3.

void ApplicationRuntime::onConnectionError(const QString& error) {
    if (m_cleanShutdownPrepared) return;
    qWarning() << "Failed to connect to server:" << error << "(silent mode, aucune popup)";
    TOAST_ERROR(QString("Connection failed: %1").arg(error), 4000);
}

void ApplicationRuntime::onClientListReceived(const QList<ClientInfo>& clients) {
    if (!m_activeSessionIdentity.isEmpty()
        && m_navigationManager && m_navigationManager->isOnScreenView()) {
        for (const ClientInfo& client : clients) {
            if (client.endpointId() == m_activeSessionIdentity) {
                // Discovery may refresh an explicitly opening/active lease,
                // but it must never turn a workspace click into an implicit
                // RemoteSession launch.
                const SessionManager::RemoteSessionState state =
                    m_sessionManager
                    ? m_sessionManager->remoteSessionState(
                          m_activeSessionIdentity)
                    : SessionManager::RemoteSessionState::Absent;
                if (state != SessionManager::RemoteSessionState::Absent
                    && state != SessionManager::RemoteSessionState::Closing) {
                    ensureRemoteSessionForClient(client);
                }
                break;
            }
        }
    }
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
        qWarning() << "Device snapshot suppressed until remote cache cleanup commits:"
                   << m_uploadManager->receiverCleanupError();
        return;
    }
    // [PHASE 7.2] Delegate to ScreenEventHandler
    if (m_screenEventHandler) {
        m_screenEventHandler->syncRegistration();
    }
}

void ApplicationRuntime::onRemoteSceneLaunchStateChanged(bool active, const QString& targetClientId, const QString& targetMachineName) {
    Q_UNUSED(active);
    Q_UNUSED(targetClientId);
    Q_UNUSED(targetMachineName);
}

// [PHASE 3] Delegate to SystemMonitor
QList<ScreenInfo> ApplicationRuntime::getLocalScreenInfo() {
    return m_systemMonitor ? m_systemMonitor->getLocalScreenInfo() : QList<ScreenInfo>();
}


void ApplicationRuntime::connectToServer() {
    if (!m_connectionManager) return;
    // [Phase 12] Get server URL from SettingsManager
    const QString url = m_settingsManager
        ? m_settingsManager->getServerUrl()
        : AppConfig::instance().serverUrl();
    m_connectionManager->connectToServer(url);
}

// [PHASE 3] Delegate to SystemMonitor
QString ApplicationRuntime::getMachineName() {
    return m_systemMonitor ? m_systemMonitor->getMachineName() : "Unknown Machine";
}

// [PHASE 3] Delegate to SystemMonitor
QString ApplicationRuntime::getPlatformName() {
    return m_systemMonitor ? m_systemMonitor->getPlatformName() : "Unknown";
}

// [PHASE 3] Delegate to SystemMonitor
// [PHASE 3] Delegate to SystemMonitor
int ApplicationRuntime::getSystemVolumePercent() {
    return m_systemMonitor ? m_systemMonitor->getSystemVolumePercent() : -1;
}

// [PHASE 3] Delegate to SystemMonitor
// [PHASE 3] Delegate to SystemMonitor - now handled in constructor
void ApplicationRuntime::updateConnectionStatus() {
    QString status = m_webSocketClient->getConnectionStatus();
    // Update the local network status in the new container
    setLocalNetworkStatus(status);
}

void ApplicationRuntime::updateIndividualProgressFromServer(int globalPercent, int filesCompleted, int totalFiles) {
    // [Phase 14.3] Delegate to UploadEventHandler
    if (m_uploadEventHandler) {
        m_uploadEventHandler->updateIndividualProgressFromServer(globalPercent, filesCompleted, totalFiles);
    }
}


bool ApplicationRuntime::hasUnuploadedFilesForTarget(const QString& targetClientId) const {
    ICanvasHost* canvas = canvasForClientId(targetClientId);
    if (!canvas) {
        return false;
    }

    for (CanvasMedia* media : canvas->enumerateMediaItems()) {
        if (!media) {
            continue;
        }
        const QString fileId = media->fileId();
        if (fileId.isEmpty()) {
            continue;
        }
        if (!m_fileManager->isFileUploadedToClient(fileId, targetClientId)) {
            return true;
        }
    }
    return false;
}

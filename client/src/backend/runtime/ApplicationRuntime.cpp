#include "backend/runtime/ApplicationRuntime.h"
#include "backend/media/MediaResidencyManager.h"
#include "backend/runtime/ApplicationActivityMonitor.h"
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
#include "backend/domain/workspace/WorkspaceManager.h"
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
#include "backend/controllers/ClientWorkspaceController.h"
#include "backend/config/AppConfig.h"
#include "backend/managers/app/SettingsManager.h"
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
    if (!client || !client->isConnected()
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

ProjectManager::TimingPolicy projectTimingPolicyFromConfig() {
    ProjectManager::TimingPolicy timing;
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
    // This flag represents command-capable Active state, not mere device
    // discovery. AVAILABLE, RECONNECTING and every terminal state must clear
    // it even when the previous projection was CONNECTED.
    m_remoteClientConnected = up == QLatin1String("CONNECTED");
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
      m_fileManager(new FileManager()),
      m_activityMonitor(new ApplicationActivityMonitor(this)),
      m_workspaceManager(new WorkspaceManager(this)),
      m_projectManager(new ProjectManager(projectTimingPolicyFromConfig(), this)),
      m_sceneActivityModel(new SceneActivityModel(this)),
      m_systemMonitor(new SystemMonitor(this)),
      m_systemTrayManager(new SystemTrayManager(this)),
      m_webSocketClient(new WebSocketClient(
          RuntimeProfile::identityLocation(), runtimeProfile.isPersistent(), this, {},
          runtimeProfile.instanceId, runtimeProfile.ordinal)),
      m_connectionManager(new ConnectionManager(m_webSocketClient, this)),
      m_settingsManager(new SettingsManager(this)),
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
    m_controlledDisconnectTimer = new QTimer(this);
    m_controlledDisconnectTimer->setSingleShot(true);
    connect(m_controlledDisconnectTimer, &QTimer::timeout,
            this, &ApplicationRuntime::finishControlledDisconnect);
    connect(m_webSocketClient, &WebSocketClient::endpointDisableAcknowledged,
            this, [this]() {
        if (!m_controlledDisconnectInProgress) return;
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
    if (m_projectManager) {
        connect(m_projectManager, &ProjectManager::projectCheckpointDue,
                this, &ApplicationRuntime::persistProjectCanvas);
        connect(m_projectManager, &ProjectManager::projectDeleted,
                this, [this](const QString&, const QString& targetEndpointId) {
            const bool wasSelected = m_activeWorkspaceEndpointId == targetEndpointId;
            // Project lifetime never owns the wire session lifetime. Enforce
            // the invariant here as well as in the explicit-delete action so
            // timer-driven deletion cannot leave an outgoing session behind.
            m_remoteSessionOpenDesiredTargets.remove(targetEndpointId);
            m_remoteSessionAutoOpenBlockedTargets.remove(targetEndpointId);
            clearDeletedProjectFromWorkspace(targetEndpointId);
            // Cleanup the canvas/FileWatcher graph before the project-less
            // workspace can make destroyWorkspaceCanvasIfUnused() detach it.
            // The RemoteSession close itself is independent of that workspace.
            terminateProjectRemoteSession(targetEndpointId, true);
            if (wasSelected) showClientListView();
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
        m_systemMonitor->startVolumeMonitoring();
        // Volume is part of every protocol v4 device snapshot; it is not tied
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
        m_transportOutageNotified = false;
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
        armIncomingSessionOrphanWatchdog();
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
            showClientListView();
            removeRuntimeWorkspace(selectedTarget);
        } else if (selectedHasProject) {
            updateRemoteClientAvailability(
                selectedTarget, QStringLiteral("Unreachable"));
            setRemoteConnectionStatus(QStringLiteral("UNREACHABLE"), false);
            if (m_navigationManager) m_navigationManager->revealCanvas();
        }
        if (!m_transportOutageNotified && m_toastSystem) {
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
    });
    connect(m_webSocketClient, &WebSocketClient::registrationConfirmed, this, &ApplicationRuntime::onRegistrationConfirmed);
    // UploadManager subscribes to the canonical upload envelope.
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
            clearRemoteSessionRuntimeState(targetEndpointId, false);
        }
    });
    
    // A file is invalidated only after its last local media reference is gone.
    // Remote copies are addressed through the exact authenticated session
    // inventory; no target name or historical canvas identifier is accepted.
    FileManager::setFileRemovalNotifier([this](const QString& fileId, const QList<QString>& clientIds, const QList<QString>& projectIds) {
        Q_UNUSED(projectIds);
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
    // Initialize toast notification system
    m_toastSystem = new ToastNotificationSystem(this);
    ToastNotificationSystem::setInstance(m_toastSystem);
    updateHistoryVisibilityState();

    if (m_projectManager && !m_projectManager->load()) {
        TOAST_ERROR(QStringLiteral("Projects could not be loaded: %1")
                        .arg(m_projectManager->lastError()),
                    AppConfig::instance().toastErrorDurationMs());
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
    workspace.expectedProjectFileIds = currentFileIds;
    m_fileManager->replaceProjectFileSet(workspace.projectId, currentFileIds);

    if (!workspace.targetEndpointId.isEmpty()) {
        const QSet<QString> toRemove = workspace.knownRemoteFileIds - currentFileIds;
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
        client.setRemoteSessionCloseAtMs(m_workspaceManager
            ? m_workspaceManager->remoteSessionCloseAtMs(entry.endpointId) : -1);
        client.setProjectDeleteAtMs(entry.projectDeleteAtMs);
        if (!entry.online) {
            const QString status = m_webSocketClient
                    && m_webSocketClient->isConnected()
                ? QStringLiteral("Disconnected")
                : QStringLiteral("Unreachable");
            client.setStatus(status);
            client.setAvailabilityStatus(status);
        } else {
            // Discovery answers only the presence question. Session badges
            // are projected here from the authenticated binding plus local
            // intent, so a stale status copied into ClientInfo can never win.
            const RemoteSessionCoordinator* coordinator = m_webSocketClient
                ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
            const RemoteSessionCoordinator::Binding binding = coordinator
                ? coordinator->outgoingForPeer(entry.endpointId)
                : RemoteSessionCoordinator::Binding();
            const bool closePending = hasPendingOutgoingSessionClose(
                entry.endpointId)
                || hasCancelledInitialOpenForTarget(entry.endpointId)
                || (!binding.remoteSessionId.isEmpty()
                    && m_locallyTerminatingRemoteSessions.contains(
                        binding.remoteSessionId));
            const bool terminalBinding = binding.phase == QLatin1String("Terminating")
                || binding.phase == QLatin1String("CleanupPending")
                || binding.phase == QLatin1String("Closed");
            const bool commandReady = isCommandReadyBinding(
                m_webSocketClient, binding);

            QString status = QStringLiteral("Available");
            if (closePending || terminalBinding) {
                status = (m_remoteSessionOpenDesiredTargets.contains(
                             entry.endpointId)
                          || wantsForegroundRemoteSession(entry.endpointId))
                    ? QStringLiteral("Connecting")
                    : QStringLiteral("Available");
            } else if (!m_webSocketClient || !m_webSocketClient->isConnected()) {
                status = QStringLiteral("Reconnecting");
            } else if (m_remoteSessionOpenPendingTargets.contains(
                           entry.endpointId)
                       || m_remoteSessionOpenDesiredTargets.contains(
                           entry.endpointId)
                       || binding.phase == QLatin1String("Opening")) {
                status = QStringLiteral("Connecting");
            } else if (commandReady) {
                status = QStringLiteral("Connected");
            } else if (binding.phase == QLatin1String("Active")
                       || binding.phase == QLatin1String("Grace")) {
                status = QStringLiteral("Reconnecting");
            }
            client.setStatus(status);
            client.setAvailabilityStatus(status);
        }

        if (ClientWorkspace* session = m_workspaceManager->findWorkspace(entry.endpointId)) {
            session->lastClientInfo = client;
            session->remoteContentClearedOnDisconnect = !entry.online
                ? session->remoteContentClearedOnDisconnect : false;
            if (session->canvas) {
                session->canvas->setRemoteSceneTarget(
                    entry.endpointId, client.getMachineName());
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

    // Stop every affected graph before deleting the first item. The canvas
    // method also sends the remote stop when a remote run is active.
    for (auto it = itemsByProject.cbegin(); it != itemsByProject.cend(); ++it) {
        if (ClientWorkspace* session = m_workspaceManager->findWorkspace(it.key());
            session && session->canvas) {
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
    m_remoteVolumePercent = active && hasProject
        ? currentWorkspace->lastClientInfo.getVolumePercent() : -1;
    if (active) setRemoteConnectionStatus(QStringLiteral("CONNECTED"), false);
    else if (grace) setRemoteConnectionStatus(QStringLiteral("RECONNECTING"), false);
    else if (closePending && selectedClient.isOnline()) {
        setRemoteConnectionStatus(QStringLiteral("CONNECTING"), false);
    }
    else setRemoteConnectionStatus(currentWorkspace->lastClientInfo.isOnline()
        ? QStringLiteral("AVAILABLE") : QStringLiteral("DISCONNECTED"), false);

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
    m_remoteDisplayName = client.getMachineName().trimmed();
    emit presentationStateChanged();
}

void ApplicationRuntime::showClientListView() {
    // Do NOT unload when navigating back to client list - uploads persist per session
    // Each client maintains its own upload state that should survive navigation
    
    const QString leavingTarget = m_activeWorkspaceEndpointId;
    if (!leavingTarget.isEmpty()) {
        m_remoteSessionOpenDesiredTargets.remove(leavingTarget);
    }
    if (!leavingTarget.isEmpty() && m_projectManager
        && !m_projectManager->hasProjectForTarget(leavingTarget)
        && m_remoteSessionOpenPendingTargets.contains(leavingTarget)) {
        RemoteSessionCoordinator* coordinator = m_webSocketClient
            ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
        const RemoteSessionCoordinator::Binding opening = coordinator
            ? coordinator->outgoingForPeer(leavingTarget)
            : RemoteSessionCoordinator::Binding();
        if (!opening.remoteSessionId.isEmpty() && m_webSocketClient) {
            m_cancelledInitialOpenSessionByTarget.insert(
                leavingTarget, opening.remoteSessionId);
            rememberPendingOutgoingSessionClose(
                leavingTarget, opening.remoteSessionId,
                opening.generation);
            m_locallyTerminatingRemoteSessions.insert(
                opening.remoteSessionId);
            retryPendingOutgoingSessionClose(
                leavingTarget, QStringLiteral("initial_open_cancelled"));
        }
        m_remoteSessionOpenPendingTargets.remove(leavingTarget);
        for (auto it = m_remoteSessionOpenTargetByRequestId.begin();
             it != m_remoteSessionOpenTargetByRequestId.end();) {
            if (it.value() == leavingTarget) {
                m_automaticRemoteSessionOpenRequestIds.remove(it.key());
                m_cancelledInitialOpenTargetByRequestId.insert(
                    it.key(), leavingTarget);
                it = m_remoteSessionOpenTargetByRequestId.erase(it);
            } else {
                ++it;
            }
        }
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
        if (status == QLatin1String("Disconnected")
            || status == QLatin1String("Unreachable")) {
            client.setOnline(false);
        }
    }
    if (m_workspaceManager) {
        if (ClientWorkspace* session = m_workspaceManager->findWorkspace(targetEndpointId)) {
            session->lastClientInfo.setStatus(status);
            session->lastClientInfo.setAvailabilityStatus(status);
            if (status == QLatin1String("Disconnected")
                || status == QLatin1String("Unreachable")) {
                session->lastClientInfo.setOnline(false);
            }
        }
    }
    refreshProjectClientList();
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
            it = m_remoteSessionOpenTargetByRequestId.erase(it);
        } else {
            ++it;
        }
    }

    clearRemoteSessionRuntimeState(targetEndpointId, false, false);
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
        available.setStatus(QStringLiteral("Available"));
        available.setAvailabilityStatus(QStringLiteral("Available"));
        ensureRemoteSessionForClient(available);
        break;
    }
    return true;
}

bool ApplicationRuntime::wantsForegroundRemoteSession(
    const QString& targetEndpointId) const
{
    return !m_cleanShutdownPrepared && !m_userDisconnected
        && !m_controlledDisconnectInProgress
        && !targetEndpointId.isEmpty()
        && targetEndpointId == m_activeWorkspaceEndpointId
        && m_navigationManager && m_navigationManager->isOnScreenView()
        && m_activityMonitor && m_activityMonitor->isActive()
        && m_projectManager
        && m_projectManager->hasProjectForTarget(targetEndpointId)
        && !m_remoteSessionAutoOpenBlockedTargets.contains(targetEndpointId);
}

void ApplicationRuntime::reconcileForegroundRemoteSession()
{
    const QString targetEndpointId = m_activeWorkspaceEndpointId;
    if (!wantsForegroundRemoteSession(targetEndpointId)
        || !m_webSocketClient || !m_webSocketClient->isConnected()) return;

    for (const ClientInfo& discovered : std::as_const(m_discoveredClients)) {
        if (discovered.endpointId() != targetEndpointId
            || !discovered.isOnline()) continue;
        // ensureRemoteSessionForClient republishes discovery/presentation and
        // can invalidate references into m_discoveredClients.
        const ClientInfo available = discovered;
        m_remoteSessionOpenSuppressedTargets.remove(targetEndpointId);
        ensureRemoteSessionForClient(available);
        return;
    }
}

void ApplicationRuntime::ensureRemoteSessionForClient(const ClientInfo& client) {
    if (m_cleanShutdownPrepared || m_userDisconnected || !m_webSocketClient) {
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
        const QString status = !client.isOnline()
            ? QStringLiteral("Disconnected")
            : ((m_remoteSessionOpenDesiredTargets.contains(targetEndpointId)
                || wantsForegroundRemoteSession(targetEndpointId))
                ? QStringLiteral("Connecting")
                : QStringLiteral("Available"));
        updateRemoteClientAvailability(targetEndpointId, status);
        if (m_activeWorkspaceEndpointId == targetEndpointId) {
            m_remoteClientConnected = false;
            m_remoteVolumePercent = -1;
            setRemoteConnectionStatus(
                status == QLatin1String("Connecting")
                    ? QStringLiteral("CONNECTING") : status.toUpper(),
                false);
            if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
        }
        return;
    }
    if (!binding.remoteSessionId.isEmpty()) {
        m_currentOutgoingSessionIdByTarget.insert(
            targetEndpointId, binding.remoteSessionId);
        m_currentOutgoingSessionGenerationByTarget.insert(
            targetEndpointId, binding.generation);
        QString status = QStringLiteral("Disconnecting");
        const bool commandReady = isCommandReadyBinding(
            m_webSocketClient, binding);
        if (commandReady) {
            status = QStringLiteral("Connected");
        } else if (binding.phase == QLatin1String("Opening")) {
            status = QStringLiteral("Connecting");
        } else if (binding.phase == QLatin1String("Active")
                   || binding.phase == QLatin1String("Grace")) {
            status = QStringLiteral("Reconnecting");
        } else if (binding.phase == QLatin1String("Closed")) {
            status = client.isOnline() ? QStringLiteral("Available")
                                       : QStringLiteral("Disconnected");
        }
        updateRemoteClientAvailability(targetEndpointId, status);
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
            setRemoteConnectionStatus(
                locallyCommandReady ? QStringLiteral("CONNECTED")
                                    : status.toUpper(),
                false);
            if (!locallyCommandReady) m_remoteVolumePercent = -1;
            updateWorkspaceCapabilities(targetEndpointId);
        }
        m_remoteSessionOpenDesiredTargets.remove(targetEndpointId);
        return;
    }

    if (m_remoteSessionOpenPendingTargets.contains(targetEndpointId)) {
        m_remoteSessionOpenDesiredTargets.remove(targetEndpointId);
        updateRemoteClientAvailability(targetEndpointId, QStringLiteral("Connecting"));
        if (m_activeWorkspaceEndpointId == targetEndpointId && m_uploadManager) {
            m_uploadManager->setTargetClientId(QString());
        }
        return;
    }
    if (m_remoteSessionOpenSuppressedTargets.contains(targetEndpointId)
        && !m_remoteSessionOpenDesiredTargets.contains(targetEndpointId)) {
        const QString status = client.isOnline()
            ? client.availabilityBadgeText() : QStringLiteral("Disconnected");
        updateRemoteClientAvailability(targetEndpointId, status);
        if (m_activeWorkspaceEndpointId == targetEndpointId) {
            setRemoteConnectionStatus(status.toUpper(), false);
            if (m_activeCanvas) m_activeCanvas->setOverlayActionsEnabled(false);
            if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
        }
        return;
    }
    if (m_remoteSessionOpenDesiredTargets.contains(targetEndpointId)) {
        m_remoteSessionOpenSuppressedTargets.remove(targetEndpointId);
    }

    const QString badge = client.availabilityBadgeText();
    const bool canOpen = client.isOnline()
        && badge != QLatin1String("Disconnecting")
        && badge != QLatin1String("Unreachable")
        && m_webSocketClient->isConnected();
    if (!canOpen) {
        const QString status = client.isOnline() ? badge : QStringLiteral("Disconnected");
        updateRemoteClientAvailability(targetEndpointId, status);
        if (m_activeWorkspaceEndpointId == targetEndpointId) {
            setRemoteConnectionStatus(status.toUpper(), false);
            if (m_activeCanvas) m_activeCanvas->setOverlayActionsEnabled(false);
            if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
        }
        return;
    }

    QString requestId;
    if (!m_webSocketClient->openRemoteSession(targetEndpointId, &requestId)) {
        if (m_workspaceManager) {
            m_workspaceManager->setRemoteSessionState(
                targetEndpointId, WorkspaceManager::RemoteSessionState::Absent);
        }
        updateRemoteClientAvailability(targetEndpointId, QStringLiteral("Unreachable"));
        if (m_activeWorkspaceEndpointId == targetEndpointId && m_uploadManager) {
            m_uploadManager->setTargetClientId(QString());
        }
        return;
    }
    m_remoteSessionOpenPendingTargets.insert(targetEndpointId);
    m_remoteSessionOpenTargetByRequestId.insert(requestId, targetEndpointId);
    if (!m_remoteSessionOpenDesiredTargets.contains(targetEndpointId)) {
        m_automaticRemoteSessionOpenRequestIds.insert(requestId);
    }
    m_remoteSessionOpenDesiredTargets.remove(targetEndpointId);
    updateRemoteClientAvailability(targetEndpointId, QStringLiteral("Connecting"));
    if (m_workspaceManager) {
        m_workspaceManager->setRemoteSessionState(
            targetEndpointId, WorkspaceManager::RemoteSessionState::Opening);
        updateWorkspaceCapabilities(targetEndpointId);
    }
    if (m_activeWorkspaceEndpointId == targetEndpointId) {
        m_remoteClientConnected = false;
        setRemoteConnectionStatus(QStringLiteral("CONNECTING"), false);
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
    const bool commandActive = phase == QLatin1String("Active");
    const bool resumableGrace = resumed && phase == QLatin1String("Grace");
    // WebSocketClient validates this too, but keep the presentation boundary
    // fail-closed: a Ready-like envelope only grants commands in Active.
    if (!commandActive && !resumableGrace) return;
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
            clearRemoteSessionRuntimeState(peerEndpointId, false, true);
            const bool reopenDesired =
                m_remoteSessionOpenDesiredTargets.contains(peerEndpointId)
                || wantsForegroundRemoteSession(peerEndpointId);
            const QString status = reopenDesired
                ? QStringLiteral("Connecting")
                : QStringLiteral("Available");
            updateRemoteClientAvailability(peerEndpointId, status);
            if (m_activeWorkspaceEndpointId == peerEndpointId) {
                m_remoteClientConnected = false;
                m_remoteVolumePercent = -1;
                setRemoteConnectionStatus(
                    reopenDesired ? QStringLiteral("CONNECTING")
                                  : QStringLiteral("AVAILABLE"),
                    false);
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

    const bool hadProject = m_projectManager
        && m_projectManager->hasProjectForTarget(peerEndpointId);
    if (!resumed) {
        const QJsonObject snapshot =
            envelope.value(QStringLiteral("snapshot")).toObject();
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
    updateRemoteClientAvailability(
        peerEndpointId,
        commandActive ? QStringLiteral("Connected")
                      : QStringLiteral("Reconnecting"));
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
        m_remoteVolumePercent = commandActive
            ? session->lastClientInfo.getVolumePercent() : -1;
        m_remoteClientConnected = commandActive;
        setRemoteConnectionStatus(
            commandActive ? QStringLiteral("CONNECTED")
                          : QStringLiteral("RECONNECTING"),
            false);
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
    workspace->lastClientInfo.setScreens(screens);
    workspace->lastClientInfo.setVolumePercent(volumePercent);
    m_projectManager->updateRemoteSnapshot(
        targetEndpointId, screens, volumePercent, revision, capturedAtMs);
    if (workspace->canvas) workspace->canvas->setScreens(screens);
    if (m_activeWorkspaceEndpointId == targetEndpointId) {
        m_selectedClient = workspace->lastClientInfo;
        m_remoteVolumePercent = volumePercent;
        emit presentationStateChanged();
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
        clearRemoteSessionRuntimeState(targetEndpointId, false, true);
        bool online = false;
        for (const ClientInfo& client : std::as_const(m_discoveredClients)) {
            if (client.endpointId() == targetEndpointId) {
                online = client.isOnline();
                break;
            }
        }
        const bool reopenDesired = online
            && (m_remoteSessionOpenDesiredTargets.contains(targetEndpointId)
                || wantsForegroundRemoteSession(targetEndpointId));
        const QString status = !online
            ? QStringLiteral("Disconnected")
            : (reopenDesired ? QStringLiteral("Connecting")
                             : QStringLiteral("Available"));
        updateRemoteClientAvailability(targetEndpointId, status);
        if (m_activeWorkspaceEndpointId == targetEndpointId) {
            m_remoteClientConnected = false;
            m_remoteVolumePercent = -1;
            setRemoteConnectionStatus(
                reopenDesired ? QStringLiteral("CONNECTING")
                              : status.toUpper(),
                false);
        }
        return;
    }

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
    m_currentOutgoingSessionIdByTarget.insert(
        targetEndpointId, remoteSessionId);
    m_currentOutgoingSessionGenerationByTarget.insert(
        targetEndpointId, generation);
    updateRemoteClientAvailability(targetEndpointId, status);
    const bool commandReady = status == QLatin1String("Connected")
        && !m_locallyTerminatingRemoteSessions.contains(remoteSessionId);
    if (m_workspaceManager) {
        WorkspaceManager::RemoteSessionState workspaceState =
            WorkspaceManager::RemoteSessionState::Closing;
        if (status == QLatin1String("Connected")) {
            workspaceState = WorkspaceManager::RemoteSessionState::Active;
        } else if (status == QLatin1String("Reconnecting")) {
            workspaceState = WorkspaceManager::RemoteSessionState::Grace;
        }
        m_workspaceManager->setRemoteSessionState(targetEndpointId,
                                                workspaceState);
        updateWorkspaceCapabilities(targetEndpointId);
    }
    if (m_activeWorkspaceEndpointId == targetEndpointId) {
        m_remoteClientConnected = commandReady;
        setRemoteConnectionStatus(status.toUpper(), false);
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
        rememberPendingOutgoingSessionClose(
            targetEndpointId, remoteSessionId, generation);
        if (m_activeWorkspaceEndpointId == targetEndpointId) m_remoteClientConnected = false;
        clearRemoteSessionRuntimeState(targetEndpointId, false, true);
        if (m_remoteSessionOpenDesiredTargets.contains(targetEndpointId)
            || wantsForegroundRemoteSession(targetEndpointId)) {
            updateRemoteClientAvailability(
                targetEndpointId, QStringLiteral("Connecting"));
            if (m_activeWorkspaceEndpointId == targetEndpointId) {
                setRemoteConnectionStatus(QStringLiteral("CONNECTING"), false);
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

void ApplicationRuntime::clearRemoteSessionRuntimeState(
    const QString& targetEndpointId,
    bool connectionLost,
    bool teardownPending) {
    ClientWorkspace* session = m_workspaceManager
        ? m_workspaceManager->findWorkspace(targetEndpointId) : nullptr;
    bool online = false;
    for (const ClientInfo& client : std::as_const(m_discoveredClients)) {
        if (client.endpointId() == targetEndpointId) {
            online = client.isOnline();
            break;
        }
    }
    if (session) {
        if (session->canvas) {
            if (connectionLost) session->canvas->handleRemoteConnectionLost();
            else session->canvas->stopScenesForSourceInvalidation();
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

    const bool waitingForReplacement = teardownPending && online
        && (m_remoteSessionOpenDesiredTargets.contains(targetEndpointId)
            || wantsForegroundRemoteSession(targetEndpointId));
    const QString status = connectionLost
        ? QStringLiteral("Unreachable")
        : (!online ? QStringLiteral("Disconnected")
                   : (waitingForReplacement ? QStringLiteral("Connecting")
                                            : QStringLiteral("Available")));
    if (m_activeWorkspaceEndpointId == targetEndpointId) {
        m_remoteClientConnected = false;
        m_remoteVolumePercent = -1;
        setRemoteConnectionStatus(
            status == QLatin1String("Connecting")
                ? QStringLiteral("CONNECTING") : status.toUpper(),
            false);
    }
    // This final projection also updates the list when the Project already
    // removed its runtime workspace before the terminal envelope arrived.
    updateRemoteClientAvailability(targetEndpointId, status);
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
            it = m_remoteSessionOpenTargetByRequestId.erase(it);
        } else {
            ++it;
        }
    }
    clearRemoteSessionRuntimeState(targetEndpointId, false, false);
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
    const auto publishError = [this, &envelope]() {
        if (!m_toastSystem) return;
        NotificationRequest notification;
        notification.severity = NotificationSeverity::Error;
        notification.category = QStringLiteral("Remote session");
        notification.message = envelope.value(QStringLiteral("message"))
            .toString(QStringLiteral(
                "The remote session command was rejected."));
        notification.correlationId =
            envelope.value(QStringLiteral("messageId")).toString();
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
            && binding.remoteSessionId == envelopeSessionId
            && binding.ownerEndpointId == errorOwnerEndpointId
            && binding.targetEndpointId == errorTargetEndpointId
            && binding.generation == errorGeneration
            && binding.phase == QLatin1String("Active");
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
        const bool replacementDesired =
            m_remoteSessionOpenDesiredTargets.contains(
                request.targetEndpointId);
        clearRemoteSessionRuntimeState(
            request.targetEndpointId, false, true);
        updateRemoteClientAvailability(
            request.targetEndpointId,
            replacementDesired ? QStringLiteral("Connecting")
                               : QStringLiteral("Available"));
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
        || code == QLatin1String("session_runtime_conflict");
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
        // Retain the intent and retry only on a later authenticated discovery
        // update; this avoids both a lost click and an unbounded hot loop.
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

    QString status = waitingForConvergence
        ? QStringLiteral("Connecting")
        : (closePending
            ? QStringLiteral("Available")
        : (m_webSocketClient && m_webSocketClient->isConnected()
            ? QStringLiteral("Available") : QStringLiteral("Unreachable")));
    if (!waitingForConvergence
        && code == QLatin1String("target_offline")) {
        status = QStringLiteral("Disconnected");
    }
    if (!targetEndpointId.isEmpty()) {
        updateRemoteClientAvailability(targetEndpointId, status);
        if (m_activeWorkspaceEndpointId == targetEndpointId) {
            setRemoteConnectionStatus(status.toUpper(), false);
            if (!activeProjectExists()) m_remoteVolumePercent = -1;
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
                                               bool attemptRemote) {
    ClientWorkspace* session = m_workspaceManager
        ? m_workspaceManager->findWorkspace(targetEndpointId) : nullptr;
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
        && binding.phase != QLatin1String("Closed")) {
        rememberPendingOutgoingSessionClose(
            targetEndpointId, binding.remoteSessionId, binding.generation);
        m_locallyTerminatingRemoteSessions.insert(binding.remoteSessionId);
        if (m_uploadManager && m_uploadManager->canRequestCancel()) {
            m_uploadManager->requestCancel();
        }
        retryPendingOutgoingSessionClose(
            targetEndpointId, QStringLiteral("inactivity_timeout"));
    }

    // Runtime content becomes unavailable synchronously, while Closing stays
    // monotonic until the correlated Closed envelope removes the wire fence.
    clearRemoteSessionRuntimeState(
        targetEndpointId, !attemptRemote,
        hasPendingOutgoingSessionClose(targetEndpointId));
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
    const bool remoteActive = workspace->remoteSessionState
        == WorkspaceManager::RemoteSessionState::Active;
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
    if (m_activityMonitor) m_activityMonitor->setSystemSuspended(suspended);
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
    // "connected" is a command-capability bit. Reconnecting deliberately
    // keeps the resumable identity while denying every remote command.
    m_remoteClientConnected =
        state.connectionStatus == RemoteClientState::Connected;
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
    return m_remoteDisplayName.isEmpty()
        ? m_selectedClient.getMachineName() : m_remoteDisplayName;
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
        m_projectManager->markAllHidden(QDateTime::currentMSecsSinceEpoch());
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
        beginControlledDisconnect();
    } else {
        if (m_controlledDisconnectInProgress) return;
        m_userDisconnected = false;
        connectToServer();
    }
    emit presentationStateChanged();
}

void ApplicationRuntime::beginControlledDisconnect()
{
    if (m_controlledDisconnectInProgress) return;
    m_controlledDisconnectInProgress = true;
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

    if (!m_webSocketClient || !m_webSocketClient->beginEndpointDisable()) {
        finishControlledDisconnect();
        return;
    }
    m_controlledDisconnectTimer->start(
        AppConfig::instance().controlledDisconnectDrainTimeoutMs());
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
    if (m_controlledDisconnectTimer) m_controlledDisconnectTimer->stop();
    m_controlledDisconnectInProgress = false;
    m_controlledDisconnectAcknowledged = false;
    m_controlledDisconnectPendingSessionIds.clear();
    if (m_connectionManager) m_connectionManager->disconnect();
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
    if (m_controlledDisconnectInProgress) {
        if (m_controlledDisconnectTimer) m_controlledDisconnectTimer->stop();
        m_controlledDisconnectInProgress = false;
        m_controlledDisconnectAcknowledged = false;
        m_controlledDisconnectPendingSessionIds.clear();
    }
}

// Screen watching and cursor streaming are intentionally absent from protocol v4.

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
    const bool replacementDesired =
        m_remoteSessionOpenDesiredTargets.contains(targetEndpointId)
        || (closing && wantsForegroundRemoteSession(targetEndpointId));

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

    QString status = QStringLiteral("Available");
    if (active) {
        status = QStringLiteral("Connected");
    } else if (grace) {
        status = QStringLiteral("Reconnecting");
    } else if (opening || replacementDesired) {
        status = QStringLiteral("Connecting");
    }
    updateRemoteClientAvailability(targetEndpointId, status);
    ClientWorkspace* workspace = m_workspaceManager
        ? m_workspaceManager->findWorkspace(targetEndpointId) : nullptr;
    if (workspace) {
        m_selectedClient = workspace->lastClientInfo;
    }
    m_remoteVolumePercent = active && workspace
        ? workspace->lastClientInfo.getVolumePercent() : -1;
    setRemoteConnectionStatus(
        status == QLatin1String("Connecting")
            ? QStringLiteral("CONNECTING")
            : (status == QLatin1String("Reconnecting")
                ? QStringLiteral("RECONNECTING") : status.toUpper()),
        false);
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
        qWarning() << "Device snapshot suppressed until remote cache cleanup commits:"
                   << m_uploadManager->receiverCleanupError();
        return;
    }
    if (m_screenEventHandler) {
        m_screenEventHandler->syncRegistration();
    }
}

void ApplicationRuntime::onRemoteSceneLaunchStateChanged(bool active, const QString& targetClientId, const QString& targetMachineName) {
    Q_UNUSED(active);
    Q_UNUSED(targetClientId);
    Q_UNUSED(targetMachineName);
}

QList<ScreenInfo> ApplicationRuntime::getLocalScreenInfo() {
    return m_systemMonitor ? m_systemMonitor->getLocalScreenInfo() : QList<ScreenInfo>();
}


void ApplicationRuntime::connectToServer() {
    if (!m_connectionManager) return;
    const QString url = m_settingsManager
        ? m_settingsManager->getServerUrl()
        : AppConfig::instance().serverUrl();
    m_connectionManager->connectToServer(url);
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
    QString status = m_webSocketClient->getConnectionStatus();
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

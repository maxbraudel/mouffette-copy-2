#ifndef APPLICATIONRUNTIME_H
#define APPLICATIONRUNTIME_H

#include "backend/network/SessionRecoveryController.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>
#include <QPointF>

#include "backend/domain/models/ClientInfo.h"
#include "backend/domain/workspace/WorkspaceManager.h"
#include "backend/runtime/RuntimeProfile.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"

class ClientWorkspaceController;
class ClientProfileCache;
class ApplicationActivityMonitor;
class ClientListBuilder;
class ClientListEventHandler;
class ConnectionManager;
class FileManager;
class FileWatcher;
class ICanvasHost;
class IncomingSessionOrphanWatchdog;
class NotificationCenter;
class ProjectManager;
class RemoteSceneController;
class CanvasMedia;
class SceneActivityModel;
class ScreenEventHandler;
class ScreenSharingService;
class AudioSharingService;
class ScreenNavigationManager;
class SettingsManager;
class SystemMonitor;
class SystemTrayManager;
class UploadEventHandler;
class UploadManager;
class UploadSignalConnector;
class WebSocketClient;
class WebSocketMessageHandler;
struct ProjectMediaReference;

// Business/runtime coordinator. It is deliberately a QObject and owns no visual Qt
// object; the only Widgets dependency in the application is isolated inside
// SystemTrayManager.
class ApplicationRuntime final : public QObject
{
    Q_OBJECT
    friend class ClientWorkspaceController;

public:
    explicit ApplicationRuntime(const RuntimeProfileContext& runtimeProfile,
                                QObject* parent = nullptr);
    ~ApplicationRuntime() override;

    using ClientWorkspace = WorkspaceManager::ClientWorkspace;

    void handleApplicationAboutToQuit();
    ScreenNavigationManager* getNavigationManager() const { return m_navigationManager; }
    WebSocketClient* getWebSocketClient() const { return m_webSocketClient; }
    UploadManager* getUploadManager() const { return m_uploadManager; }
    FileManager* getFileManager() const { return m_fileManager; }
    FileWatcher* getFileWatcher() const { return m_fileWatcher; }
    WorkspaceManager* getWorkspaceManager() const { return m_workspaceManager; }
    ProjectManager* getProjectManager() const { return m_projectManager; }
    SceneActivityModel* getSceneActivityModel() const { return m_sceneActivityModel; }
    NotificationCenter* getNotificationCenter() const;
    SettingsManager* getSettingsManager() const { return m_settingsManager; }
    QString screenSharingStatus() const;
    QString audioSharingStatus() const;
    QString remoteAudioState() const;
    QString remoteAudioStatus() const;
    QString remoteScreenState() const;
    QString remoteScreenStatus() const;
    ClientProfileCache* profileCache() const { return m_profileCache; }
    ICanvasHost* getActiveCanvas() const { return m_activeCanvas; }
    const ClientInfo& getSelectedClient() const { return m_selectedClient; }
    const ClientInfo& selectedClient() const { return m_selectedClient; }
    QList<ClientInfo> displayClients() const { return m_displayClients; }

    bool isUserDisconnected() const;
    bool isConnectionDraining() const { return m_controlledDisconnectInProgress; }
    void setConnectionEnabled(bool enabled);
    void setUserDisconnected(bool disconnected) { setConnectionEnabled(!disconnected); }
    bool cleanShutdownInProgress() const { return m_cleanShutdownPrepared; }
    bool isApplicationSuspended() const { return m_applicationSuspended; }
    QString activeWorkspaceEndpointId() const { return m_activeWorkspaceEndpointId; }
    QString activeUploadWorkspaceEndpointId() const {
        return m_activeUploadWorkspaceEndpointId;
    }
    int getLastConnectedClientCount() const { return m_lastConnectedClientCount; }
    bool isRemoteClientConnected() const { return m_remoteClientConnected; }
    bool isCanvasRevealedForCurrentClient() const { return m_canvasRevealedForCurrentClient; }
    bool shouldPreserveViewportOnReconnect() const { return m_preserveViewportOnReconnect; }
    bool areUploadSignalsConnected() const { return m_uploadSignalsConnected; }
    bool isRemoteOverlayActionsEnabled() const { return m_remoteOverlayActionsEnabled; }

    void setActiveCanvas(ICanvasHost* canvas) { m_activeCanvas = canvas; }
    void setSelectedClient(const ClientInfo& client);
    void setActiveWorkspaceEndpointId(const QString& identity) { m_activeWorkspaceEndpointId = identity; }
    void setActiveUploadWorkspaceEndpointId(const QString& identity) { m_activeUploadWorkspaceEndpointId = identity; }
    void setLastConnectedClientCount(int count) { m_lastConnectedClientCount = count; }
    void setRemoteClientConnected(bool connected) { m_remoteClientConnected = connected; }
    void setCanvasRevealedForCurrentClient(bool revealed) { m_canvasRevealedForCurrentClient = revealed; }
    void setCanvasContentEverLoaded(bool loaded) { m_canvasContentEverLoaded = loaded; }
    void setPreserveViewportOnReconnect(bool preserve) { m_preserveViewportOnReconnect = preserve; }

    void resetAllWorkspaceUploadStates();
    void setLocalNetworkStatus(const QString& status);
    void refreshRemoteConnectionPresentation(bool propagateLoss = false);
    QString remoteConnectionStatus(const QString& targetEndpointId,
                                   const ClientInfo* presence,
                                   bool localDiscoveryUsable) const;
    void refreshOverlayActionsState(bool remoteConnected,
                                    bool propagateLoss = true);
    void syncRegistration();
    void connectToServer();
    void ensureRemoteSessionForClient(const ClientInfo& client);
    void updateClientNameDisplay(const ClientInfo& clientInfo);
    void updateVolumeIndicator();

    QList<ScreenInfo> getLocalScreenInfo();
    bool captureLocalScreenInfo(QList<ScreenInfo>* screens);
    int getSystemVolumePercent();
    QString getMachineName();
    QString getPlatformName();

    ClientWorkspace* ensureWorkspace(const ClientInfo& client);
    ClientWorkspace* findWorkspace(const QString& targetEndpointId);
    const ClientWorkspace* findWorkspace(const QString& targetEndpointId) const;
    void configureWorkspace(ClientWorkspace& workspace);
    QList<ClientInfo> buildDisplayClientList(const QList<ClientInfo>& connectedClients);
    void markAllWorkspacesDisconnected();
    void reconcileRemoteFilesForWorkspace(ClientWorkspace& workspace,
                                          const QSet<QString>& currentFileIds);
    void connectUploadSignals();
    void setUploadWorkspaceByUploadId(const QString& uploadId,
                                      const QString& workspaceEndpointId);
    QString uploadWorkspaceByUploadId(const QString& uploadId) const {
        return m_uploadWorkspaceByUploadId.value(uploadId);
    }
    void removeUploadWorkspaceByUploadId(const QString& uploadId) {
        m_uploadWorkspaceByUploadId.remove(uploadId);
    }

    QString localConnectionDetail() const;
    QString clientConnectionDetail(const QString& endpoint) const;
    QString remoteConnectionDetail() const { return clientConnectionDetail(m_activeWorkspaceEndpointId); }
    QString localStatusText() const { return m_localStatusText; }
    QString remoteStatusText() const { return m_remoteStatusText; }
    QString remoteDisplayName() const;
    int remoteVolumePercent() const { return m_remoteVolumePercent; }
    bool remoteScreenAvailable() const;
    bool remoteScreenLoading() const;
    bool canDeleteActiveProject() const;
    bool activeProjectExists() const;
    bool activeRemoteSessionExists() const;
    void setQmlWindowVisible(bool visible);
    void setPointerInsideControlWindow(bool inside);

    void activateClient(const QString& endpointId);
    void activateOngoingScene(const QString& sceneRunId);
    void navigateToClients();
    void navigateToHistory();
    void toggleConnectionEnabled();
    void deleteActiveProjectConfirmed();

    bool getAutoUploadImportedMedia() const;
    ClientWorkspace* workspaceForActiveUpload();
    ClientWorkspace* workspaceForUploadId(const QString& uploadId);
    void clearUploadTracking(ClientWorkspace& workspace);
    bool hasUnuploadedFilesForTarget(const QString& targetClientId) const;
    void setApplicationSuspended(bool suspended);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

public slots:
    void handleApplicationStateChanged(Qt::ApplicationState state);
    void handleNativeSystemSuspendedChanged(bool suspended);
    void onUploadButtonClicked();

signals:
    void screenSharingStatusChanged();
    void audioSharingStatusChanged();
    void displayClientsChanged(const QList<ClientInfo>& clients);
    void presentationStateChanged();
    void applicationPageChanged(int page);
    void activeWorkspaceChanged(const QString& targetEndpointId);
    void qmlRaiseRequested();
    void qmlHideRequested();

private slots:
    void onConnected();
    void onDisconnected();
    void onConnectionError(const QString& error);
    void onClientListReceived(const QList<ClientInfo>& clients);
    void onRegistrationConfirmed(const ClientInfo& clientInfo);
    void onClientSelected(const ClientInfo& clientInfo, int clientIndex);
    void onOngoingSceneSelected(const QString& sceneRunId);
    void updateConnectionStatus();
    void onEnableDisableClicked();
    void onBackToClientListClicked();
    void onRemoteSceneLaunchStateChanged(bool active,
                                         const QString& targetClientId,
                                         const QString& targetMachineName);
    void showHistoryPage();
    void onDeleteProjectRequested();
    void onTrayIconActivated(int reason);

private:
    void refreshMediaSharing();
    void refreshRemoteCursorStreaming();
    void publishLocalCursor();
    void expireStaleRemoteCursors();
    void showScreenView(const ClientInfo& client);
    void showClientListView();
    void switchToWorkspace(const QString& targetEndpointId);
    void updateUploadButtonForWorkspace(ClientWorkspace& workspace);
    ICanvasHost* canvasForEndpointId(const QString& targetEndpointId) const;
    void updateApplicationSuspendedState(bool suspended);
    void updateHistoryVisibilityState();
    void persistProjectCanvas(const QString& targetEndpointId);
    void reconcileProjectMediaResidency(const QString& targetEndpointId);
    void restoreProjectCanvas(ClientWorkspace& workspace);
    bool wantsForegroundRemoteSession(const QString& targetEndpointId) const;
    void reconcileForegroundRemoteSession();
    void terminateProjectRemoteSession(const QString& targetEndpointId,
                                       bool attemptRemote,
                                       const QString& reason = QStringLiteral("inactivity_timeout"));
    bool hasPendingOutgoingSessionClose(
        const QString& targetEndpointId,
        const QString& remoteSessionId = QString()) const;
    void rememberPendingOutgoingSessionClose(
        const QString& targetEndpointId,
        const QString& remoteSessionId,
        quint64 generation);
    bool retryPendingOutgoingSessionClose(const QString& targetEndpointId,
                                          const QString& reason);
    bool hasCancelledInitialOpenForTarget(
        const QString& targetEndpointId) const;
    bool finalizePendingOutgoingSessionClose(
        const QString& targetEndpointId,
        const QString& remoteSessionId,
        quint64 generation,
        bool allowReplacementOpen);
    void handleRemoteSessionReady(const QJsonObject& envelope, bool resumed);
    void handleRemoteSessionSnapshot(const QJsonObject& envelope);
    void handleRemoteSessionLeaseState(const QJsonObject& envelope);
    void handleRemoteSessionTerminating(const QJsonObject& envelope);
    void handleRemoteRendererTeardownSettled(const QString& remoteSessionId,
                                             bool success);
    void enqueueRendererTeardown(const QString& remoteSessionId);
    void startNextPendingRendererTeardown(
        const QString& excludedRemoteSessionId = QString());
    void beginTerminalIncomingCacheCleanup(const QString& reasonCode);
    void finishTerminalIncomingCacheCleanupIfReady();
    void handleRemoteSessionClosed(const QJsonObject& envelope);
    void handleRemoteSessionError(const QJsonObject& envelope);
    void cancelPendingRemoteSessionOpen(const QString& targetEndpointId);
    bool matchesCurrentOutgoingSession(const QString& targetEndpointId,
                                       const QString& remoteSessionId,
                                       quint64 generation = 0) const;
    void clearRemoteSessionRuntimeState(const QString& targetEndpointId,
                                        bool teardownPending = false,
                                        const QString& remoteSessionId = QString());
    void clearDeletedProjectFromWorkspace(const QString& targetEndpointId);
    void destroyWorkspaceCanvasIfUnused(const QString& targetEndpointId);
    void updateWorkspaceCapabilities(const QString& targetEndpointId);
    void armIncomingSessionOrphanWatchdog();
    void cancelIncomingSessionOrphanWatchdogIfResumed(
        const QJsonObject& envelope);
    void retryPendingTeardownAcks();
    void removeRuntimeWorkspace(const QString& targetEndpointId);
    void refreshProjectClientList();
    void prepareCleanShutdown();
    void finishCleanShutdownIncomingCacheTeardownIfReady();
    void finishCleanShutdown();
    void beginControlledDisconnect(quint64 transitionId);
    void handleTerminalTransportLoss(const QString& reason, const QString& bootId,
                                     quint64 generation, bool notify);
    void finishControlledDisconnect();
    void finishControlledDisconnectIfReady();
    QList<ProjectMediaReference> collectProjectMediaReferences(
        const QString& targetEndpointId, ICanvasHost* canvas) const;
    void removeInvalidMediaItems(const QList<CanvasMedia*>& mediaItems);
    void validateAllProjectSources();
    void updateIndividualProgressFromServer(int globalPercent,
                                            int filesCompleted,
                                            int totalFiles);

    FileManager* m_fileManager = nullptr;
    ApplicationActivityMonitor* m_activityMonitor = nullptr;
    WorkspaceManager* m_workspaceManager = nullptr;
    ProjectManager* m_projectManager = nullptr;
    SceneActivityModel* m_sceneActivityModel = nullptr;
    SystemMonitor* m_systemMonitor = nullptr;
    QTimer* m_cursorPublishTimer = nullptr;
    QTimer* m_deviceSnapshotTimer = nullptr;
    QTimer* m_cursorExpiryTimer = nullptr;
    QElapsedTimer m_cursorClock;
    struct PublishedCursor {
        quint64 generation = 0;
        quint64 sequence = 0;
        bool visible = false;
        int screenId = -1;
        QPointF position;
        qint64 lastSentAtMs = -1;
    };
    QHash<QString, PublishedCursor> m_publishedCursors;
    QHash<QString, qint64> m_receivedCursorAtByEndpoint;
    SystemTrayManager* m_systemTrayManager = nullptr;
    WebSocketClient* m_webSocketClient = nullptr;
    ConnectionManager* m_connectionManager = nullptr;
    SettingsManager* m_settingsManager = nullptr;
    ScreenSharingService* m_screenSharing = nullptr;
    AudioSharingService* m_audioSharing = nullptr;
    ClientProfileCache* m_profileCache = nullptr;
    WebSocketMessageHandler* m_webSocketMessageHandler = nullptr;
    ScreenEventHandler* m_screenEventHandler = nullptr;
    UploadEventHandler* m_uploadEventHandler = nullptr;
    ClientListEventHandler* m_clientListEventHandler = nullptr;
    ClientWorkspaceController* m_workspaceController = nullptr;
    UploadSignalConnector* m_uploadSignalConnector = nullptr;
    UploadManager* m_uploadManager = nullptr;
    FileWatcher* m_fileWatcher = nullptr;
    ScreenNavigationManager* m_navigationManager = nullptr;
    ToastNotificationSystem* m_toastSystem = nullptr;
    RemoteSceneController* m_remoteSceneController = nullptr;
    ICanvasHost* m_activeCanvas = nullptr;

    IncomingSessionOrphanWatchdog* m_incomingSessionOrphanWatchdog = nullptr;
    QSet<QString> m_controlledDisconnectPendingSessionIds;
    bool m_controlledDisconnectAcknowledged = false;
    bool m_controlledDisconnectInProgress = false;
    QSet<QString> m_incomingOrphanSessionIds;
    int m_lastConnectedClientCount = 0;
    QString m_activeWorkspaceEndpointId;
    ClientInfo m_thisClient;
    ClientInfo m_selectedClient;
    QString m_selectionEndpointId;
    ClientInfo m_selectionClient;
    bool m_intentionalTransportClose = false;
    quint64 m_controlledDisconnectTransition = 0;
    QString m_controlledDisconnectRequestId;
    quint64 m_controlledDisconnectGeneration = 0;
    bool m_transportOutageNotified = false;
    bool m_ignoreSelectionChange = false;
    bool m_uploadSignalsConnected = false;
    QString m_activeUploadWorkspaceEndpointId;
    QHash<QString, QString> m_uploadWorkspaceByUploadId;
    bool m_canvasRevealedForCurrentClient = false;
    bool m_canvasContentEverLoaded = false;
    bool m_preserveViewportOnReconnect = false;
    bool m_remoteClientConnected = false;
    bool m_remoteOverlayActionsEnabled = false;
    bool m_applicationSuspended = false;
    bool m_nativeSystemSuspended = false;
    bool m_qmlWindowVisible = false;
    int m_applicationPage = 0;
    QString m_localStatusText = QStringLiteral("DISCONNECTED");
    QString m_remoteStatusText = QStringLiteral("DISCONNECTED");
    QString m_remoteDisplayName;
    int m_remoteVolumePercent = -1;
    QList<ClientInfo> m_discoveredClients;
    QList<ClientInfo> m_displayClients;
    QSet<QString> m_restoredProjectIds;
    QHash<QString, QString> m_remoteSessionOpenTargetByRequestId;
    QSet<QString> m_automaticRemoteSessionOpenRequestIds;
    // Validation/permanent failures require a new explicit selection. Ordinary
    // peer/transport loss must not block activity-driven foreground recovery.
    SessionRecoveryController m_sessionRecovery;
    QSet<QString> m_remoteSessionAutoOpenBlockedTargets;
    QSet<QString> m_remoteSessionOpenPendingTargets;
    QSet<QString> m_remoteSessionOpenSuppressedTargets;
    // An explicit selection made while the previous session is still being
    // torn down is durable until exactly one replacement OPEN is dispatched.
    // This is intentionally separate from discovery and Project state.
    QSet<QString> m_remoteSessionOpenDesiredTargets;
    // Cancellation is correlated to the exact OPEN request and, once known,
    // the exact session. A target-only flag can incorrectly cancel a later
    // user-initiated OPEN for the same device.
    QHash<QString, QString> m_cancelledInitialOpenTargetByRequestId;
    QHash<QString, QString> m_cancelledInitialOpenSessionByTarget;
    QSet<QString> m_locallyTerminatingRemoteSessions;

    struct PendingOutgoingSessionClose {
        QString remoteSessionId;
        quint64 generation = 0;
        // A CLOSE must be retried once on every authenticated transport. The
        // RemoteSession generation alone does not change on reconnect.
        quint64 closeDispatchedOnConnectionGeneration = 0;
    };
    struct OutgoingSessionCloseRequest {
        QString targetEndpointId;
        QString remoteSessionId;
        quint64 generation = 0;
    };
    // Keyed by the peer endpoint so a late Active/Resumed event for the same
    // RemoteSession can never restore command-capable state after local close.
    QHash<QString, PendingOutgoingSessionClose>
        m_pendingOutgoingSessionCloses;
    QHash<QString, OutgoingSessionCloseRequest>
        m_outgoingSessionCloseRequestById;
    // Last authenticated outgoing identity remains available across a
    // transport reconnect, allowing replayed old tombstones to be rejected
    // even before the current session has been rebound into the coordinator.
    QHash<QString, QString> m_currentOutgoingSessionIdByTarget;
    QHash<QString, quint64> m_currentOutgoingSessionGenerationByTarget;

    struct PendingTeardownAck {
        QString teardownId;
        bool sceneStopped = false;
        bool uploadsAborted = false;
        bool cacheQuarantined = false;
        int removedFileCount = 0;
        QString errorCode;
        qint64 quarantinedBytes = 0;
    };
    struct PendingRendererTeardown {
        QString ownerEndpointId;
        QString teardownId;
        quint64 generation = 0;
        bool rendererTeardownStarted = false;
        qint64 nextRetryAtMs = -1;
    };
    QHash<QString, PendingRendererTeardown> m_pendingRendererTeardowns;
    QSet<QString> m_readerTeardownPendingSessionIds;
    // RemoteSceneController owns one target-wide renderer graph. This shared
    // FIFO serializes protocol teardown, transport-terminal cleanup, and clean
    // shutdown instead of letting those independent callers race admission.
    QList<QString> m_pendingRendererTeardownOrder;
    QString m_activeRendererTeardownSessionId;
    QHash<QString, PendingTeardownAck> m_pendingTeardownAcks;
    QSet<QString> m_terminalRendererPendingSessionIds;
    QSet<QString> m_terminalIncomingSessionFilter;
    QString m_terminalIncomingCleanupReason;
    bool m_terminalIncomingCleanupActive = false;
    bool m_terminalIncomingCleanupAll = false;
    bool m_terminalIncomingCacheTeardownStarted = false;
    QSet<QString> m_cleanShutdownRendererPendingSessionIds;
    bool m_cleanShutdownIncomingCacheTeardownStarted = false;
    bool m_cleanShutdownPrepared = false;
    bool m_cleanShutdownFinished = false;
    bool m_quitDeferredForReaders = false;
};

#endif // APPLICATIONRUNTIME_H

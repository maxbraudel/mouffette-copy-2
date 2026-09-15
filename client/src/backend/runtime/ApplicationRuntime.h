#ifndef APPLICATIONRUNTIME_H
#define APPLICATIONRUNTIME_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include "backend/domain/models/ClientInfo.h"
#include "backend/domain/workspace/WorkspaceManager.h"
#include "backend/runtime/RuntimeProfile.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"

class ClientWorkspaceController;
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
class CanvasMedia;
class SceneActivityModel;
class ScreenEventHandler;
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
struct RemoteClientState;

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
    ICanvasHost* getActiveCanvas() const { return m_activeCanvas; }
    const ClientInfo& getSelectedClient() const { return m_selectedClient; }
    const ClientInfo& selectedClient() const { return m_selectedClient; }
    QList<ClientInfo> displayClients() const { return m_displayClients; }

    bool isUserDisconnected() const { return m_userDisconnected; }
    void setUserDisconnected(bool disconnected) { m_userDisconnected = disconnected; }
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
    void setRemoteConnectionStatus(const QString& status, bool propagateLoss = true);
    void setRemoteClientState(const RemoteClientState& state,
                              bool propagateLoss = true);
    void refreshOverlayActionsState(bool remoteConnected,
                                    bool propagateLoss = true);
    void syncRegistration();
    void connectToServer();
    void ensureRemoteSessionForClient(const ClientInfo& client);
    void updateClientNameDisplay(const ClientInfo& clientInfo);
    void updateVolumeIndicator();
    void stopInlineSpinner();

    QList<ScreenInfo> getLocalScreenInfo();
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

    QString localStatusText() const { return m_localStatusText; }
    QString remoteStatusText() const { return m_remoteStatusText; }
    QString remoteDisplayName() const;
    int remoteVolumePercent() const { return m_remoteVolumePercent; }
    bool remoteBusy() const { return m_remoteBusy; }
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

public slots:
    void handleApplicationStateChanged(Qt::ApplicationState state);
    void handleNativeSystemSuspendedChanged(bool suspended);
    void onUploadButtonClicked();

signals:
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
    void showScreenView(const ClientInfo& client);
    void showClientListView();
    void switchToWorkspace(const QString& targetEndpointId);
    void updateUploadButtonForWorkspace(ClientWorkspace& workspace);
    ICanvasHost* canvasForEndpointId(const QString& targetEndpointId) const;
    void updateApplicationSuspendedState(bool suspended);
    void updateHistoryVisibilityState();
    void persistProjectCanvas(const QString& targetEndpointId);
    void restoreProjectCanvas(ClientWorkspace& workspace);
    void terminateProjectRemoteSession(const QString& targetEndpointId,
                                       bool attemptRemote);
    void handleRemoteSessionReady(const QJsonObject& envelope, bool resumed);
    void handleRemoteSessionSnapshot(const QJsonObject& envelope);
    void handleRemoteSessionLeaseState(const QJsonObject& envelope);
    void handleRemoteSessionTerminating(const QJsonObject& envelope);
    void handleRemoteRendererTeardownSettled(const QString& remoteSessionId,
                                             bool success);
    void beginTerminalIncomingCacheCleanup(const QString& reasonCode);
    void finishTerminalIncomingCacheCleanupIfReady();
    void handleRemoteSessionClosed(const QJsonObject& envelope);
    void handleRemoteSessionError(const QJsonObject& envelope);
    void updateRemoteClientAvailability(const QString& targetEndpointId,
                                        const QString& status);
    void clearRemoteSessionRuntimeState(const QString& targetEndpointId,
                                        bool connectionLost);
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
    void beginControlledDisconnect();
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
    SystemTrayManager* m_systemTrayManager = nullptr;
    WebSocketClient* m_webSocketClient = nullptr;
    ConnectionManager* m_connectionManager = nullptr;
    SettingsManager* m_settingsManager = nullptr;
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
    ICanvasHost* m_activeCanvas = nullptr;

    IncomingSessionOrphanWatchdog* m_incomingSessionOrphanWatchdog = nullptr;
    QTimer* m_controlledDisconnectTimer = nullptr;
    QSet<QString> m_controlledDisconnectPendingSessionIds;
    bool m_controlledDisconnectAcknowledged = false;
    bool m_controlledDisconnectInProgress = false;
    QSet<QString> m_incomingOrphanSessionIds;
    int m_lastConnectedClientCount = 0;
    QString m_activeWorkspaceEndpointId;
    ClientInfo m_thisClient;
    ClientInfo m_selectedClient;
    bool m_userDisconnected = false;
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
    bool m_remoteBusy = false;
    QList<ClientInfo> m_discoveredClients;
    QList<ClientInfo> m_displayClients;
    QSet<QString> m_restoredProjectIds;
    QHash<QString, QString> m_remoteSessionOpenTargetByRequestId;
    QSet<QString> m_remoteSessionOpenPendingTargets;
    QSet<QString> m_remoteSessionOpenSuppressedTargets;
    QSet<QString> m_cancelledInitialOpenTargets;
    QSet<QString> m_locallyTerminatingRemoteSessions;

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
    };
    QHash<QString, PendingRendererTeardown> m_pendingRendererTeardowns;
    QHash<QString, PendingTeardownAck> m_pendingTeardownAcks;
    QSet<QString> m_terminalRendererPendingSessionIds;
    QSet<QString> m_terminalIncomingSessionFilter;
    QString m_terminalIncomingCleanupReason;
    bool m_terminalIncomingCleanupActive = false;
    bool m_terminalIncomingCacheTeardownStarted = false;
    QSet<QString> m_cleanShutdownRendererPendingSessionIds;
    bool m_cleanShutdownIncomingCacheTeardownStarted = false;
    bool m_cleanShutdownPrepared = false;
    bool m_cleanShutdownFinished = false;
};

#endif // APPLICATIONRUNTIME_H

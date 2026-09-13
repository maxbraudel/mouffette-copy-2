#ifndef APPLICATIONRUNTIME_H
#define APPLICATIONRUNTIME_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include "backend/domain/models/ClientInfo.h"
#include "backend/domain/session/SessionManager.h"
#include "backend/runtime/RuntimeProfile.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"

class CanvasSessionController;
class ClientListBuilder;
class ClientListEventHandler;
class ConnectionManager;
class FileManager;
class FileWatcher;
class ICanvasHost;
class NotificationCenter;
class ProjectManager;
class CanvasMedia;
class SceneActivityModel;
class ScreenEventHandler;
class ScreenNavigationManager;
class SettingsManager;
class SystemMonitor;
class SystemTrayManager;
class TimerController;
class UploadEventHandler;
class UploadManager;
class UploadSignalConnector;
class WebSocketClient;
class WebSocketMessageHandler;
struct ProjectMediaReference;
struct RemoteClientState;

// Business/runtime coordinator retained while the former monolithic window is
// split into services. It is deliberately a QObject and owns no visual Qt
// object; the only Widgets dependency in the application is isolated inside
// SystemTrayManager.
class ApplicationRuntime final : public QObject
{
    Q_OBJECT
    friend class CanvasSessionController;
    friend class TimerController;

public:
    explicit ApplicationRuntime(const RuntimeProfileContext& runtimeProfile,
                                QObject* parent = nullptr);
    ~ApplicationRuntime() override;

    using CanvasSession = SessionManager::CanvasSession;

    void handleApplicationAboutToQuit();
    ScreenNavigationManager* getNavigationManager() const { return m_navigationManager; }
    WebSocketClient* getWebSocketClient() const { return m_webSocketClient; }
    UploadManager* getUploadManager() const { return m_uploadManager; }
    FileManager* getFileManager() const { return m_fileManager; }
    FileWatcher* getFileWatcher() const { return m_fileWatcher; }
    SessionManager* getSessionManager() const { return m_sessionManager; }
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
    QString getActiveSessionIdentity() const { return m_activeSessionIdentity; }
    QString getActiveUploadSessionIdentity() const { return m_activeUploadSessionIdentity; }
    int getLastConnectedClientCount() const { return m_lastConnectedClientCount; }
    QString getActiveRemoteClientId() const { return m_activeRemoteClientId; }
    bool isRemoteClientConnected() const { return m_remoteClientConnected; }
    bool isCanvasRevealedForCurrentClient() const { return m_canvasRevealedForCurrentClient; }
    bool shouldPreserveViewportOnReconnect() const { return m_preserveViewportOnReconnect; }
    bool areUploadSignalsConnected() const { return m_uploadSignalsConnected; }
    bool isRemoteOverlayActionsEnabled() const { return m_remoteOverlayActionsEnabled; }

    void setActiveCanvas(ICanvasHost* canvas) { m_activeCanvas = canvas; }
    void setSelectedClient(const ClientInfo& client);
    void setActiveSessionIdentity(const QString& identity) { m_activeSessionIdentity = identity; }
    void setActiveUploadSessionIdentity(const QString& identity) { m_activeUploadSessionIdentity = identity; }
    void setLastConnectedClientCount(int count) { m_lastConnectedClientCount = count; }
    void setActiveRemoteClientId(const QString& id) { m_activeRemoteClientId = id; }
    void setRemoteClientConnected(bool connected) { m_remoteClientConnected = connected; }
    void setCanvasRevealedForCurrentClient(bool revealed) { m_canvasRevealedForCurrentClient = revealed; }
    void setCanvasContentEverLoaded(bool loaded) { m_canvasContentEverLoaded = loaded; }
    void setPreserveViewportOnReconnect(bool preserve) { m_preserveViewportOnReconnect = preserve; }

    void resetAllSessionUploadStates();
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

    CanvasSession& ensureCanvasSession(const ClientInfo& client);
    CanvasSession* findCanvasSession(const QString& persistentClientId);
    const CanvasSession* findCanvasSession(const QString& persistentClientId) const;
    CanvasSession* findCanvasSessionByServerClientId(const QString& serverClientId);
    const CanvasSession* findCanvasSessionByServerClientId(const QString& serverClientId) const;
    CanvasSession* findCanvasSessionByIdeaId(const QString& canvasSessionId);
    void configureCanvasSession(CanvasSession& session);
    QList<ClientInfo> buildDisplayClientList(const QList<ClientInfo>& connectedClients);
    void markAllSessionsOffline();
    void reconcileRemoteFilesForSession(CanvasSession& session,
                                        const QSet<QString>& currentFileIds);
    void connectUploadSignals();
    void setUploadSessionByUploadId(const QString& uploadId,
                                    const QString& sessionIdentity);
    QString getUploadSessionByUploadId(const QString& uploadId) const {
        return m_uploadSessionByUploadId.value(uploadId);
    }
    void removeUploadSessionByUploadId(const QString& uploadId) {
        m_uploadSessionByUploadId.remove(uploadId);
    }

    QString localStatusText() const { return m_localStatusText; }
    QString remoteStatusText() const { return m_remoteStatusText; }
    QString remoteDisplayName() const;
    int remoteVolumePercent() const { return m_remoteVolumePercent; }
    bool remoteBusy() const { return m_remoteBusy; }
    bool canCloseActiveSession() const;
    bool canDeleteActiveProject() const;
    bool closingActiveSession() const { return m_closingActiveSession; }
    void setQmlWindowVisible(bool visible);

    void activateClient(const QString& endpointId);
    void activateOngoingScene(const QString& sceneRunId);
    void navigateToClients();
    void navigateToHistory();
    void toggleConnectionEnabled();
    void closeActiveSession();
    void deleteActiveProjectConfirmed();

    bool getAutoUploadImportedMedia() const;
    QString createIdeaId() const;
    void markCanvasLoadRequest(const QString& persistentClientId);
    void recordCanvasLoadReady(const QString& persistentClientId, int screenCount);
    CanvasSession* sessionForActiveUpload();
    CanvasSession* sessionForUploadId(const QString& uploadId);
    void clearUploadTracking(CanvasSession& session);
    bool hasUnuploadedFilesForTarget(const QString& targetClientId) const;
    void setApplicationSuspended(bool suspended);
    QTimer* getStatusUpdateTimer() const { return m_statusUpdateTimer; }
    QTimer* getDisplaySyncTimer() const { return m_displaySyncTimer; }

public slots:
    void handleApplicationStateChanged(Qt::ApplicationState state);
    void handleNativeSystemSuspendedChanged(bool suspended);
    void onUploadButtonClicked();

signals:
    void displayClientsChanged(const QList<ClientInfo>& clients);
    void presentationStateChanged();
    void applicationPageChanged(int page);
    void activeSessionChanged(const QString& sessionId);
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
    void onDisconnectProjectRequested();
    void onDeleteProjectRequested();
    void onTrayIconActivated(int reason);

private:
    void showScreenView(const ClientInfo& client);
    void showClientListView();
    void switchToCanvasSession(const QString& persistentClientId);
    void updateUploadButtonForSession(CanvasSession& session);
    void rotateSessionIdea(CanvasSession& session);
    ICanvasHost* canvasForClientId(const QString& clientId) const;
    void updateApplicationSuspendedState(bool suspended);
    void updateHistoryVisibilityState();
    void persistProjectCanvas(const QString& targetEndpointId);
    void restoreProjectCanvas(CanvasSession& session);
    void terminateProjectRemoteSession(const QString& targetEndpointId,
                                       bool attemptRemote);
    void handleRemoteSessionReady(const QJsonObject& envelope, bool resumed);
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
    void finishDeferredProjectDeletion(const QString& targetEndpointId);
    void retryPendingTeardownAcks();
    void removeRuntimeCanvasSession(const QString& targetEndpointId);
    void refreshProjectClientList();
    void setActiveProjectVisibleIfAppropriate();
    void prepareCleanShutdown();
    void finishCleanShutdownIncomingCacheTeardownIfReady();
    void maybeFinishCleanShutdown();
    void finishCleanShutdown();
    QList<ProjectMediaReference> collectProjectMediaReferences(
        const QString& targetEndpointId, ICanvasHost* canvas) const;
    void removeInvalidMediaItems(const QList<CanvasMedia*>& mediaItems);
    void validateAllProjectSources();
    void updateIndividualProgressFromServer(int globalPercent,
                                            int filesCompleted,
                                            int totalFiles);

    FileManager* m_fileManager = nullptr;
    SessionManager* m_sessionManager = nullptr;
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
    CanvasSessionController* m_canvasSessionController = nullptr;
    TimerController* m_timerController = nullptr;
    UploadSignalConnector* m_uploadSignalConnector = nullptr;
    UploadManager* m_uploadManager = nullptr;
    FileWatcher* m_fileWatcher = nullptr;
    ScreenNavigationManager* m_navigationManager = nullptr;
    ToastNotificationSystem* m_toastSystem = nullptr;
    ICanvasHost* m_activeCanvas = nullptr;

    QTimer* m_statusUpdateTimer = nullptr;
    QTimer* m_displaySyncTimer = nullptr;
    int m_lastConnectedClientCount = 0;
    QString m_activeSessionIdentity;
    ClientInfo m_thisClient;
    ClientInfo m_selectedClient;
    bool m_userDisconnected = false;
    bool m_ignoreSelectionChange = false;
    bool m_uploadSignalsConnected = false;
    QString m_activeUploadSessionIdentity;
    QHash<QString, QString> m_uploadSessionByUploadId;
    bool m_canvasRevealedForCurrentClient = false;
    bool m_canvasContentEverLoaded = false;
    bool m_preserveViewportOnReconnect = false;
    QString m_activeRemoteClientId;
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
    bool m_closingActiveSession = false;
    QHash<QString, qint64> m_canvasLoadRequestMsBySession;
    QList<ClientInfo> m_discoveredClients;
    QList<ClientInfo> m_displayClients;
    QSet<QString> m_restoredProjectIds;
    QHash<QString, QString> m_remoteSessionOpenTargetByRequestId;
    QSet<QString> m_remoteSessionOpenPendingTargets;
    QSet<QString> m_remoteSessionOpenSuppressedTargets;
    QSet<QString> m_reopenAfterSessionCloseTargets;
    QSet<QString> m_disconnectPendingTargets;
    QSet<QString> m_deleteAfterSessionCloseTargets;
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
    QString m_terminalIncomingCleanupReason;
    bool m_terminalIncomingCleanupActive = false;
    bool m_terminalIncomingCacheTeardownStarted = false;
    QSet<QString> m_cleanShutdownPendingSessionIds;
    QSet<QString> m_cleanShutdownRendererPendingSessionIds;
    bool m_cleanShutdownIncomingCacheTeardownStarted = false;
    bool m_cleanShutdownPrepared = false;
    bool m_cleanShutdownFinished = false;
    bool m_cleanShutdownQuitRequested = false;
};

#endif // APPLICATIONRUNTIME_H

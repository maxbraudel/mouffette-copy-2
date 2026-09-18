#ifndef WEBSOCKETCLIENT_H
#define WEBSOCKETCLIENT_H

#include "backend/network/RetryScheduler.h"

#include <QObject>
#include <QWebSocket>
#include <QSet>
#include <QHash>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTimer>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QVector>
#include <QPointF>
#include <functional>
#include <memory>
#include <limits>
#include "backend/domain/models/ClientInfo.h"
#include "backend/network/ProtocolConstants.h"

class DeviceIdentityStore;
class SceneRunCoordinator;
class RemoteSessionCoordinator;

class WebSocketClient : public QObject {
    Q_OBJECT

public:
    static constexpr int ProtocolVersion = MouffetteProtocol::Version;
    using SuspendInclusiveClock = std::function<qint64()>;

    explicit WebSocketClient(QObject *parent = nullptr);
    WebSocketClient(const QString& identityFallbackDirectory,
                    bool preferNativeIdentityVault,
                    QObject* parent = nullptr,
                    SuspendInclusiveClock suspendInclusiveClock = {},
                    int instanceOrdinal = 1);
    ~WebSocketClient();
    
    // Connection management
    void connectToServer(const QString& serverUrl);
    void disconnect();
    void abortConnectionAttempt();
    bool isConnected() const; // compatibility alias
    bool isTransportAuthenticated() const { return isConnected(); }
    bool isTransportConnected() const;
    // Upload channel (secondary socket) management
    bool ensureUploadChannel(); // opens m_uploadSocket if needed (async); returns true if already connected or opening
    void closeUploadChannel();  // closes m_uploadSocket if open
    bool isUploadChannelConnected() const;
    // Data and inventories always use the dedicated bidirectional socket.
    // An unavailable socket leaves transfers paused; no control fallback.
    bool beginUploadSession(bool preferUploadChannel);
    void endUploadSession();
    qint64 uploadTransportBytesToWrite() const;
    bool isUploadSessionTransportAvailable() const;
    bool isUploadSessionUsingDedicatedChannel() const {
        return m_uploadSessionActive && m_useUploadSocketForSession;
    }
    
    // Client registration
    void registerClient(const QString& machineName, const QString& platform, const QList<ScreenInfo>& screens, int volumePercent);
    void invalidateLocalDeviceSnapshot();

    // Protocol-v5 uploads. Authenticated socket identity supplies the peer;
    // every command is correlated solely by RemoteSession + generation.
    bool sendUploadStart(const QString& remoteSessionId,
                         quint64 generation,
                         const QString& uploadId,
                         const QJsonArray& filesManifest);
    bool sendUploadResume(const QString& remoteSessionId,
                          quint64 generation,
                          const QString& uploadId);
    bool sendUploadChunk(const QString& remoteSessionId,
                         quint64 generation,
                         const QString& uploadId,
                         const QString& assetId,
                         qint64 offset,
                         const QString& sha256,
                         const QByteArray& data);
    bool sendUploadComplete(const QString& remoteSessionId,
                            quint64 generation,
                            const QString& uploadId,
                            const QJsonArray& assets);
    bool sendUploadAbort(const QString& remoteSessionId,
                         quint64 generation,
                         const QString& uploadId,
                         const QString& reason);
    bool sendUploadRemove(const QString& remoteSessionId,
                          quint64 generation,
                          const QString& removalId,
                          const QString& uploadId,
                          const QString& assetId,
                          qint64 size,
                          const QString& sha256,
                          const QString& reason = QStringLiteral("source_removed"));
    // Target-side upload_ready/progress/finished/rejected/abort_ack response.
    bool sendUploadProtocolResponse(const QJsonObject& response);
    bool sendMediaResidency(const QString& remoteSessionId, quint64 generation,
                            quint64 sequence, const QJsonArray& assets, bool delta = false);
    
    // RemoteSession lifecycle. Tokens remain memory-only and are never
    // exposed in generic logs or durable project state.
    bool openRemoteSession(const QString& targetEndpointId,
                           QString* requestId = nullptr);
    // Replays an OPEN whose exact request identifier was locally cancelled
    // before any session identity arrived. Server idempotency turns this into
    // an authoritative Ready, terminal replay, or correlated error.
    bool replayRemoteSessionOpen(const QString& targetEndpointId,
                                 const QString& requestId);
    // Accepts a server-authenticated offer as the target. The accepted
    // snapshot is the most recently published complete endpoint snapshot.
    bool acceptRemoteSessionOffer(const QJsonObject& offer);
    bool sendRemoteSessionSnapshot(const QString& remoteSessionId,
                                   quint64 generation,
                                   const QJsonObject& targetSnapshot);
    bool sendRemoteCursor(const QString& remoteSessionId, quint64 generation,
                          quint64 sequence, bool visible, int screenId,
                          const QPointF& screenPosition);
    bool resumeRemoteSession(const QString& remoteSessionId);
    void resumeAllRemoteSessions();
    bool reconcileRemoteSessions();
    bool canIssueSessionCommands(const QString& remoteSessionId) const;
    qint64 sessionRecoveryRemainingMs(const QString& remoteSessionId) const;
    bool isSessionRecovering(const QString& remoteSessionId) const;
    // Withdraws this endpoint from discovery and atomically asks the server
    // to terminate all of its RemoteSessions before the socket is closed.
    bool beginEndpointDisable(const QString& requestId = {});
    bool closeRemoteSession(const QString& remoteSessionId,
                            QString* requestId = nullptr,
                            const QString& reason = QStringLiteral("explicit_disconnect"));
    // Retries a locally retained, authenticated session identity after the
    // transport lease has cleared the in-memory coordinator. The server still
    // validates endpoint, runtime, transport and session generation.
    bool closeRemoteSessionByIdentity(
        const QString& remoteSessionId,
        quint64 generation,
        QString* requestId = nullptr,
        const QString& reason = QStringLiteral("explicit_disconnect"));
    bool discardRemoteSessionAfterAuthoritativeRejection(
        const QString& remoteSessionId);
    bool acknowledgeRemoteSessionTeardown(const QString& remoteSessionId,
                                          const QString& teardownId,
                                          bool sceneStopped,
                                          bool uploadsAborted,
                                          bool cacheQuarantined,
                                          int removedFileCount,
                                          const QString& errorCode = QString(),
                                          qint64 quarantinedBytes = 0);

    // Protocol-v5 immutable SceneRun lifecycle. The coordinator resolves the
    // active RemoteSession for a peer and supplies session generation/digest
    // correlation to every message.
    SceneRunCoordinator* sceneRunCoordinator() const { return m_sceneRuns.get(); }
    RemoteSessionCoordinator* remoteSessionCoordinator() const;
    bool sendScenePrepare(const QString& targetEndpointId,
                          quint64 revision,
                          const QJsonArray& manifest,
                          const QJsonObject& scene,
                          QString* sceneRunId = nullptr,
                          QString* digest = nullptr,
                          QString* errorMessage = nullptr);
    bool sendScenePrepareProgress(const QString& sceneRunId,
                                  int percent,
                                  const QJsonArray& checklist);
    bool sendScenePrepared(const QString& sceneRunId,
                           bool success,
                           const QJsonArray& checklist,
                           const QString& errorCode = QString(),
                           const QString& message = QString());
    bool sendSceneArmed(const QString& sceneRunId, qint64 clockUncertaintyMs);
    bool sendSceneStarted(const QString& sceneRunId,
                          bool firstFramePresented,
                          qint64 presentedServerMonotonicMs);
    bool sendSceneStateSnapshot(const QString& sceneRunId,
                                quint64 sequence,
                                qint64 sampledServerMonotonicMs,
                                const QJsonObject& snapshot);
    bool sendSceneStop(const QString& sceneRunId,
                       const QString& reason = QStringLiteral("owner_stop"));
    bool sendSceneStopped(const QString& sceneRunId,
                          bool success,
                          const QString& message = QString());

    qint64 sceneClockUncertaintyMs() const;
    qint64 estimatedServerMonotonicMs() const;
    // Starts a short, coalesced burst of heartbeat probes. Scene preparation
    // uses this when the passive heartbeat sample is absent or too noisy; the
    // regular heartbeat cadence remains responsible for lease health.
    bool requestSceneClockSynchronization();

    // Client-side cancel safeguard: mark an uploadId as cancelled to ignore any further chunk sends
    void cancelUploadId(const QString& uploadId) { m_canceledUploads.insert(uploadId); }
    
    // Getters
    QString installationId() const { return m_installationId; }
    QString endpointId() const { return m_endpointId; }
    QString instanceId() const { return m_instanceId; }
    int instanceOrdinal() const { return m_instanceOrdinal; }
    QString runtimeId() const { return m_runtimeId; }
    quint64 connectionGeneration() const { return m_connectionGeneration; }
    QString serverBootId() const { return m_serverBootId; }
    QJsonObject serverPolicy() const { return m_serverPolicy; }
    bool hasUnexpiredLease() const;
    qint64 leaseRemainingMs() const;
    qint64 transportRecoveryRemainingMs() const;
    QString getConnectionStatus() const { return m_connectionStatus; }

signals:
    void connected();
    void disconnected();
    void transportConnected();
    void connectionError(const QString& error);
    void fatalError(const QString& error);
    void connectionStatusChanged(const QString& status); // emitted whenever textual connection status updates
    void transportHealthChanged(bool degraded);
    void serverPolicyReceived(const QJsonObject& policy);
    void heartbeatSampleReceived(quint64 sequence,
                                 qint64 roundTripMs,
                                 qint64 serverOffsetMs,
                                 qint64 uncertaintyMs);
    void leaseExpired(const QString& serverBootId, quint64 connectionGeneration);
    void sessionsInvalidated(const QString& reason, const QString& serverBootId,
                             quint64 connectionGeneration);
    void remoteSessionRecoveryExpired(const QString& remoteSessionId, quint64 generation);
    void remoteSessionAbsent(const QString& remoteSessionId, quint64 generation);
    void reconciliationCompleted();
    void serverRestarted(const QString& previousServerBootId,
                         const QString& newServerBootId);
    void reauthenticatedWithinLease(quint64 previousGeneration,
                                    quint64 newGeneration);
    void clientListReceived(const QList<ClientInfo>& clients);
    void registrationConfirmed(const ClientInfo& clientInfo);
    void messageReceived(const QJsonObject& message);

    // Canonical protocol v5 upload envelope for both sender and target roles.
    void uploadMessageReceived(const QJsonObject& envelope);
    void mediaResidencyReceived(const QJsonObject& envelope);
    void uploadTransportBytesWritten(qint64 bytes);
    void uploadTransportLost(const QString& reason);
    void remoteSessionOpened(const QJsonObject& envelope);
    void remoteSessionOfferReceived(const QJsonObject& envelope);
    void remoteSessionSnapshotReceived(const QJsonObject& envelope);
    void localDeviceSnapshotRequested();
    void remoteCursorReceived(const QString& remoteSessionId, int screenId,
                              const QPointF& screenPosition, bool visible);
    void remoteSessionResumed(const QJsonObject& envelope);
    void remoteSessionLeaseStateChanged(const QJsonObject& envelope);
    void remoteSessionTerminating(const QJsonObject& envelope);
    void remoteSessionClosed(const QJsonObject& envelope);
    void remoteSessionLogicallyClosed(const QJsonObject& envelope);
    void endpointDisableAcknowledged(const QString& requestId, quint64 connectionGeneration);
    // Business/protocol rejection for a RemoteSession command. This must not
    // be interpreted as a transport failure by ConnectionManager.
    void remoteSessionError(const QJsonObject& envelope);
    // Full correlated protocol v5 scene envelopes. Keeping these as objects
    // makes new checklist fields additive without weakening validation.
    void scenePrepareReceived(const QJsonObject& envelope);
    void scenePrepareProgressReceived(const QJsonObject& envelope);
    void scenePreparedReceived(const QJsonObject& envelope);
    void sceneArmedReceived(const QJsonObject& envelope);
    void sceneCommitReceived(const QJsonObject& envelope);
    void sceneStartedReceived(const QJsonObject& envelope);
    void sceneStateSnapshotReceived(const QJsonObject& envelope);
    void sceneStopReceived(const QJsonObject& envelope);
    void sceneStoppedReceived(const QJsonObject& envelope);
    void sceneErrorReceived(const QJsonObject& envelope);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString& message);
    void onUploadTextMessageReceived(const QString& message);
    void onError(QAbstractSocket::SocketError error);
    void sendHeartbeat();
    void checkLeaseHealth();
    // Upload socket handlers
    void onUploadConnected();
    void onUploadDisconnected();
    void onUploadError(QAbstractSocket::SocketError error);

private:
    struct ClockSample {
        qint64 receivedAtMs = 0;
        qint64 offsetMs = 0;
        qint64 uncertaintyMs = 0;
    };

    void handleMessage(const QJsonObject& message);
    bool handleAuthChallenge(const QJsonObject& message);
    bool handleWelcome(const QJsonObject& message);
    bool validateServerPolicy(const QJsonObject& policy, QString* errorMessage) const;
    QJsonObject addProtocolEnvelope(const QJsonObject& message) const;
    bool sendRawControlMessage(const QJsonObject& message);
    void publishDeviceSnapshots();
    bool sendTrackedControl(const QJsonObject& message);
    void scheduleControlRetry(const QString& requestId);
    void completeControlRequest(const QString& requestId);
    void completeSessionRequests(const QString& sessionId, bool terminal = false, bool final = false);
    void clearControlRequests();
    struct PendingControl { QJsonObject message; quint64 transport; QString boot; };
    QHash<QString, PendingControl> m_pendingControl;
    RetryScheduler m_controlRetries;
    QString m_registrationRequestId;
public:
    void cancelRemoteSessionOpen(const QString& requestId) { completeControlRequest(requestId); }
    bool sessionRecoveryInProgress(const QString& sessionId) const;
private:
    void noteServerContact();
    qint64 suspendInclusiveNowMs() const;
    qint64 leaseElapsedMs() const;
    bool hasFreshSceneClockSample() const;
    void resetSceneClockEstimate();
    void expireLease();
    bool acknowledgeSessionState(const QJsonObject& envelope);
    void updateSessionDeadline(const QJsonObject& envelope);
    void checkSessionRecoveryDeadlines();
    void beginSessionRecovery(qint64 detectedAtMs);
    qint64 sessionProofBudgetMs() const;
    void retryExpiredSessionClose(const QString& remoteSessionId, quint64 generation);
    void refreshSessionProofs(const QJsonObject& heartbeat);
    struct SessionDeadline {
        qint64 localDeadlineMs = -1;
        qint64 serverDeadlineMs = -1;
        qint64 proofDeadlineMs = -1;
        qint64 interruptionDeadlineMs = -1;
        bool expired = false;
        qint64 lastProofAtMs = -1;
    };
    QHash<QString, QJsonObject> m_assetRemovalObligations;
    QHash<QString, SessionDeadline> m_sessionDeadlines;
    QHash<QString, QString> m_resumeRequestIds;
    QString m_reconcileRequestId;
    quint64 m_reconciliationFailureSerial = 0;
    qint64 m_reconcileSentAtMs = -1;
    qint64 m_serverClockAnchorMs = -1;
    qint64 m_authenticationSentAtMs = -1;
    qint64 m_localClockAnchorMs = -1;
    qint64 m_previousLeaseCheckMs = -1;
    void sendMessage(const QJsonObject& message);
    bool sendControlMessage(const QJsonObject& message);
    bool sendMessageUpload(const QJsonObject& message);
    void reportSelectedUploadTransportLost(const QString& reason);
    void setConnectionStatus(const QString& status);
    QJsonObject sceneMessage(const QString& sceneRunId, const QString& type) const;
    QSet<QString> m_canceledUploads; // uploadIds that should drop further chunk sends
    QList<QString> m_canceledUploadOrder;
    std::unique_ptr<DeviceIdentityStore> m_identityStore;
    std::unique_ptr<SceneRunCoordinator> m_sceneRuns;
    
    QWebSocket* m_webSocket;
    QWebSocket* m_uploadSocket = nullptr;
    QString m_serverUrl;
    QString m_installationId;     // SHA-256 of the installation public key
    QString m_endpointId;         // Derived from installationId + instanceId
    QString m_runtimeId;          // Stable for this process only
    const QString m_instanceId;
    const int m_instanceOrdinal;
    QString m_uploadClientId;
    QString m_uploadChannelToken;
    QString m_socketClientId;     // Raw connection ID provided by welcome (diagnostics)
    QString m_serverBootId;
    QString m_pendingServerBootId;
    quint64 m_connectionGeneration = 0;
    QJsonObject m_serverPolicy;
    QString m_connectionStatus;
    QString m_registeredMachineName;
    QString m_registeredPlatform;
    QJsonObject m_registeredTargetSnapshot;
    QJsonObject m_registeredDeviceContent;
    QJsonObject m_registeredEndpointSnapshot;
    QJsonObject m_publishedEndpointSnapshot;
    quint64 m_publishedEndpointGeneration = 0;
    struct PublishedDeviceSnapshot {
        quint64 generation = 0;
        QJsonObject content;
        qint64 sentAtMs = -1;
    };
    QHash<QString, PublishedDeviceSnapshot> m_publishedDeviceSnapshots;
    QTimer m_deviceSnapshotRetryTimer;
    quint64 m_targetSnapshotRevision = 0;
    QHash<QString, quint64> m_targetSnapshotSequenceBySession;
    struct CursorSequence {
        quint64 generation = 0;
        quint64 sequence = 0;
    };
    QHash<QString, CursorSequence> m_receivedCursorSequenceBySession;
    QTimer* m_heartbeatTimer;
    QTimer* m_clockSyncBurstTimer;
    QTimer* m_leaseHealthTimer;
    QElapsedTimer m_processClock;
    SuspendInclusiveClock m_suspendInclusiveClock;
    qint64 m_lastServerContactContinuousMs = -1;
    QHash<quint64, qint64> m_heartbeatSentAt;
    QVector<ClockSample> m_clockSamples;
    quint64 m_heartbeatSequence = 0;
    int m_clockSyncBurstRemaining = 0;
    qint64 m_lastClockSyncBurstStartedAtMs = -1;
    qint64 m_serverMonotonicOffsetMs = 0;
    qint64 m_clockUncertaintyMs = std::numeric_limits<qint64>::max();
    qint64 m_selectedClockSampleReceivedAtMs = -1;
    // Protocol timing is unavailable until an authenticated welcome supplies
    // the authoritative server policy. Keeping these at zero makes any
    // accidental pre-welcome use fail closed instead of duplicating policy.
    int m_heartbeatIntervalMs = 0;
    int m_leaseTimeoutMs = 0;
    int m_sessionRecoveryTimeoutMs = 0;
    int m_transportSuspectAfterMs = 0;
    int m_transportTimeoutMs = 0;
    qint64 m_transportRecoveryDeadlineMs = -1;
    qint64 m_lastHeartbeatAckMs = -1;
    qint64 m_lastRttMs = -1;
    qint64 m_lastDiagnosticsAtMs = -1;
    qint64 m_maxLoopLagMs = 0;
    quint64 m_controlSentBytes = 0;
    quint64 m_dataSentBytes = 0;
    quint64 m_confirmedDataBytes = 0;
    QHash<QString, qint64> m_confirmedUploadOffsets;
    bool m_dispatchingData = false;
    QHash<QString, QJsonObject> m_pendingScenePrepares;
    QHash<QString, QJsonObject> m_pendingUploadResponses;
    void flushPendingUploadResponses();
    bool m_authenticated = false;
    bool m_hasEstablishedLease = false;
    bool m_leaseExpired = false;
    bool m_degraded = false;
    bool m_disconnectSignalEmitted = false;
    bool m_endpointDraining = false;
    QString m_endpointDisableRequestId;
    quint64 m_clientListRevision = 0;
    QString m_identityInitializationError;
    bool m_uploadSessionActive = false;
    int m_uploadSessionRefCount = 0;
    bool m_useUploadSocketForSession = false;
    bool m_uploadTransportLossReported = false;
    bool m_uploadChannelTokenRequested = false;
    bool m_uploadChannelAuthenticated = false;
    RetryScheduler m_uploadRetries;
    int m_uploadRetryAttempt = 0;
    QString m_uploadTokenRequestId;
    void failUploadChannelAttempt(const QString& reason);
    void scheduleUploadChannelRetry(const QString& reason);
};

#endif // WEBSOCKETCLIENT_H

// RemoteSceneController.h - manages rendering of a host client's scene on a remote client
#pragma once

#include <QObject>
#include <QMap>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QVideoFrame>
#include <QImage>
#include <QPointer>
#include <QVariantList>
#include <memory>

#include "backend/network/RemoteCacheStore.h"

class WebSocketClient;
class FileManager;
class QMediaPlayer;
class QVideoSink;
class QAudioOutput;
class QQuickWindow;
class QVariantAnimation;
class MediaListModel;
class RemoteVideoFrameSource;

class RemoteSceneController : public QObject {
	Q_OBJECT
public:
	explicit RemoteSceneController(FileManager* fileManager, WebSocketClient* ws, QObject* parent = nullptr);
	~RemoteSceneController() override;
	void setEnabled(bool en) { m_enabled = en; if (!en) clearScene(); }
	bool isEnabled() const { return m_enabled; }
	// Begins renderer teardown for exactly one RemoteSession.  Returning true
	// means that the request was accepted (or had already settled), not that
	// QObject destruction has completed.  Cache quarantine must wait for the
	// correlated teardownSettled() signal. Idempotent.
	bool teardownRemoteSession(const QString& remoteSessionId);

signals:
	// Emitted only after every tracked player, sink, audio output, frame source,
	// timer, animation, QML model and native top-level QQuickWindow from the
	// retired graph has actually been destroyed. An empty id denotes a normal
	// SceneRun stop rather than a RemoteSession teardown.
	void teardownSettled(const QString& remoteSessionId, bool success);
	void authoritativeSnapshotApplied(quint64 sequence);
	void authoritativeSnapshotRejected(quint64 sequence, const QString& reason);

private slots:
	void onScenePrepareEnvelope(const QJsonObject& envelope);
	void onScenePreparedEnvelope(const QJsonObject& envelope);
	void onSceneCommitEnvelope(const QJsonObject& envelope);
	void onSceneStateSnapshotEnvelope(const QJsonObject& envelope);
	void onSceneStopEnvelope(const QJsonObject& envelope);
	void onSceneStoppedEnvelope(const QJsonObject& envelope);
	void onSceneErrorEnvelope(const QJsonObject& envelope);
	void onRemoteSessionResumedEnvelope(const QJsonObject& envelope);
	void onRemoteSceneStart(const QString& senderClientId, const QJsonObject& scene);
	void onRemoteSceneActivate(const QString& senderClientId,
	                          const QString& sceneInstanceId,
	                          qint64 activationEpochMs,
	                          int activationDelayMs);
	void onRemoteSceneVideoSync(const QString& senderClientId,
	                            const QString& sceneInstanceId,
	                            qint64 sequence,
	                            qint64 sampledEpochMs,
	                            const QJsonArray& videos);
	void onRemoteSceneStop(const QString& senderClientId,
	                      const QString& sceneInstanceId);
	void onConnectionLost();
	void onConnectionError(const QString& errorMessage);
	void onRemoteSpanReady(const QString& mediaId, const QString& spanId);
	// Kept as a slot so focused Qt tests can exercise the transaction without a
	// transport. Production calls it only after envelope correlation succeeds.
	bool applyAuthoritativeStateSnapshot(const QJsonObject& snapshot,
	                                     quint64 sequence,
	                                     qint64 sampleAgeMs);
	bool remoteRenderGraphsReady() const;

private:
	struct ScreenWindow {
		QPointer<QQuickWindow> window;
		QPointer<MediaListModel> mediaModel;
		QVariantList mediaEntries;
		QMetaObject::Connection firstFrameConnection;
		int firstFramePassesRemaining = 0;
		// Immutable source topology for the SceneRun. The QWindow geometry below
		// is the local physical mapping and must never be replaced by a resumed
		// owner's coordinates.
		QJsonObject sourceScreenDefinition;
		int x=0,y=0,w=0,h=0;
		quint64 sceneEpoch = 0;
	};
	struct RemoteMediaItem {
		QString mediaId;
		QString fileId;
		QString fileName;
		QString type; // image | video | text
		// Text-specific properties
		QString text;
		QString fontFamily;
		bool fontItalic = false;
		bool fontUnderline = false;
		bool fontUppercase = false;
		int fontWeight = 400;
		int fontPixelSize = 1;
		QString textColor;
		double textOutlineWidthPx = 0.0;
		QString textBorderColor;
		bool fitToTextEnabled = false;
		bool highlightEnabled = false;
		QString textHighlightColor;
		int baseWidth = 0;
		int baseHeight = 0;
		double z = 0.0;
		bool contentVisible = true;
		double renderOpacity = 0.0;
		bool renderVisible = true;
		enum class HorizontalAlignment { Left, Center, Right };
		enum class VerticalAlignment { Top, Center, Bottom };
		HorizontalAlignment horizontalAlignment = HorizontalAlignment::Center;
		VerticalAlignment verticalAlignment = VerticalAlignment::Center;
		// Multi-screen spans support: each span maps to a screen with its own normalized geom
		struct Span {
			int screenId = -1;
			double nx = 0, ny = 0, nw = 0, nh = 0;
			double destNx = 0, destNy = 0, destNw = 0, destNh = 0;
			double srcNx = 0, srcNy = 0, srcNw = 1, srcNh = 1;
			QString spanId;
			bool qmlReady = false;
		};
		QList<Span> spans;
		bool autoDisplay=false; int autoDisplayDelayMs=0;
		bool autoPlay=false; int autoPlayDelayMs=0;
		bool autoPause=false; int autoPauseDelayMs=0;
		bool autoHide=false; int autoHideDelayMs=0;
		bool hideWhenVideoEnds=false;
	bool autoMute=false; int autoMuteDelayMs=0;
	bool muteWhenVideoEnds=false;
		double fadeInSeconds=0.0; double fadeOutSeconds=0.0; double contentOpacity = 1.0;
		// Audio state from host (videos)
		bool muted = false; double volume = 1.0; // 0..1
		bool autoUnmute = false; int autoUnmuteDelayMs = 0;
		double audioFadeInSeconds = 0.0; double audioFadeOutSeconds = 0.0;
		QPointer<QVariantAnimation> audioFadeAnimation;
		QPointer<QVariantAnimation> visualFadeAnimation;
		bool continuousLoop = false;
		bool repeatEnabled = false; int repeatCount = 0; int repeatRemaining = 0; bool repeatActive = false;
		qint64 lastRepeatTriggerMs = 0;
		qint64 authoritativeSeekGuardUntilMs = 0;
		bool primedFirstFrame = false; bool playAuthorized = false;
		bool displayReady = false; bool displayStarted = false;
		bool hiding = false;
		bool pausedAtEnd = false;
		bool loaded = false; // true when QMediaPlayer reports Loaded/Buffered
		bool readyNotified = false; // true after controller counts this media as ready
		bool fadeInPending = false; // true when fade requested before global activation
		qint64 startPositionMs = 0; bool hasStartPosition = false;
        qint64 endPositionMs = -1;
		qint64 displayTimestampMs = -1; bool hasDisplayTimestamp = false;
		bool awaitingStartFrame = false;
		QVideoFrame primedFrame;
		bool awaitingDecoderSync = false;
		qint64 decoderSyncTargetMs = -1;
		bool awaitingLivePlayback = false;
		bool livePlaybackStarted = false;
		int liveWarmupFramesRemaining = 0;
		qint64 lastLiveFrameTimestampMs = -1;
		QTimer* displayTimer = nullptr; QTimer* playTimer = nullptr; QTimer* pauseTimer = nullptr; QTimer* hideTimer = nullptr;
		// Video only
		QMediaPlayer* player = nullptr; QAudioOutput* audio = nullptr;
		QMetaObject::Connection deferredStartConn; // one-shot start after load
		QMetaObject::Connection primingConn; // one-shot first-frame priming when autoPlay=false
		QMetaObject::Connection mirrorConn; // multi-span frame mirroring
		quint64 sceneEpoch = 0; // generation token to guard delayed actions
		int pendingDisplayDelayMs = -1;
		int pendingPlayDelayMs = -1;
		int pendingPauseDelayMs = -1;
		QVideoSink* primingSink = nullptr;
		QVideoSink* liveSink = nullptr;
		bool videoOutputsAttached = false;
		bool primedFrameSticky = false;
		QTimer* muteTimer = nullptr;
		QTimer* hideEndDelayTimer = nullptr;
		QTimer* muteEndDelayTimer = nullptr;
		// Explicitly-owned retry/automation timers. Avoid untrackable
		// QTimer::singleShot functors surviving a RemoteSession teardown.
		QList<QPointer<QTimer>> auxiliaryTimers;
		bool hideEndTriggered = false;
		bool muteEndTriggered = false;
		bool holdLastFrameAtEnd = false;
		QImage lastFrameImage;
		QPointer<RemoteVideoFrameSource> frameSource;
	};

	struct PendingSceneRequest {
		QString senderId;
		QJsonObject scene;
		bool valid = false;
	};

	QQuickWindow* ensureScreenWindow(int screenId, int x, int y, int w, int h, bool primary);
	void resetWindowForNewScene(ScreenWindow& sw, int screenId, int x, int y, int w, int h, bool primary);
	void buildWindows(const QJsonArray& screensArray);
	void buildMedia(const QJsonArray& mediaArray);
	void scheduleMedia(const std::shared_ptr<RemoteMediaItem>& item);
	void scheduleMediaMulti(const std::shared_ptr<RemoteMediaItem>& item);
	void fadeIn(const std::shared_ptr<RemoteMediaItem>& item);
	void fadeOutAndHide(const std::shared_ptr<RemoteMediaItem>& item);
	void scheduleHideTimer(const std::shared_ptr<RemoteMediaItem>& item);
	void scheduleMuteTimer(const std::shared_ptr<RemoteMediaItem>& item);
	void cancelAudioFade(const std::shared_ptr<RemoteMediaItem>& item, bool applyFinalState);
	void applyAudioMuteState(const std::shared_ptr<RemoteMediaItem>& item, bool muted, bool skipFade = false);
	void clearScene();
	void dispatchDeferredSceneStart();
	void teardownMediaItem(const std::shared_ptr<RemoteMediaItem>& item);
	void trackTeardownObject(QObject* object);
	void trackTeardownObjectTree(QObject* root);
	void scheduleTeardownBarrierCompletion();
	void completeTeardownBarrier(quint64 barrierEpoch);
	void updatePublishedMediaItem(const std::shared_ptr<RemoteMediaItem>& item);
    void markItemReady(const std::shared_ptr<RemoteMediaItem>& item);
    void evaluateItemReadiness(const std::shared_ptr<RemoteMediaItem>& item);
    void startSceneActivationIfReady();
    void activateScene();
    void startDeferredTimers();
    void handleSceneReadyTimeout();
    void resetSceneSynchronization();
    void seekToConfiguredStart(const std::shared_ptr<RemoteMediaItem>& item);
    qint64 effectiveEndPosition(const std::shared_ptr<RemoteMediaItem>& item) const;
    qint64 effectiveStartPosition(const std::shared_ptr<RemoteMediaItem>& item) const;
	void startPendingPauseTimerIfEligible(const std::shared_ptr<RemoteMediaItem>& item);
	void triggerAutoPlayNow(const std::shared_ptr<RemoteMediaItem>& item, quint64 epoch);
	void applyPrimedFrameToSinks(const std::shared_ptr<RemoteMediaItem>& item);
	bool autoDisplayDelayActive(const std::shared_ptr<RemoteMediaItem>& item) const;
	void clearRenderedFrames(const std::shared_ptr<RemoteMediaItem>& item);
	void ensureVideoOutputsAttached(const std::shared_ptr<RemoteMediaItem>& item);
	void finalizeLivePlaybackStart(const std::shared_ptr<RemoteMediaItem>& item, const QVideoFrame& frame);
    qint64 targetDisplayTimestamp(const std::shared_ptr<RemoteMediaItem>& item) const;
	void freezeVideoOutput(const std::shared_ptr<RemoteMediaItem>& item);
	void restoreVideoOutput(const std::shared_ptr<RemoteMediaItem>& item);
	void applyImageToSpans(const std::shared_ptr<RemoteMediaItem>& item, const QImage& image) const;
	void publishScreenModel(int screenId);
	void publishMediaSpan(const std::shared_ptr<RemoteMediaItem>& item, RemoteMediaItem::Span& span);
	void setRemoteMediaVisualState(const std::shared_ptr<RemoteMediaItem>& item, qreal opacity, bool visible);
	bool allSpansReady(const std::shared_ptr<RemoteMediaItem>& item) const;
	bool matchesSceneEnvelope(const QJsonObject& envelope) const;
	void sendPrepareResult(bool success, const QString& message = QString());
	void tryArmPreparedScene();
	void sendFirstFramePresented(bool forceReplay = false);
	void disconnectFirstFrameObservers();
	void updatePrepareProgress();
	QString receivedFilePath(const QString& fileId) const;
	RemoteCacheStore::Scope receivedFileScope() const;


	private:
	FileManager* m_fileManager = nullptr;
	
	WebSocketClient* m_ws = nullptr; // not owned
	bool m_enabled = true;
	QMap<int, ScreenWindow> m_screenWindows;
	QList<std::shared_ptr<RemoteMediaItem>> m_mediaItems;
	quint64 m_sceneEpoch = 0; // incremented on each start/stop
	QString m_pendingSenderClientId;
	QString m_pendingSceneInstanceId;
	QString m_pendingSceneDigest;
	QString m_pendingRemoteSessionId;
	quint64 m_pendingSessionGeneration = 0;
	quint64 m_pendingSceneRevision = 0;
	QJsonArray m_prepareChecklist;
	bool m_scenePreparedReported = false;
	bool m_sceneAllPrepared = false;
	bool m_sceneArmedReported = false;
	bool m_sceneCommitReceived = false;
	bool m_firstFrameReported = false;
	qint64 m_firstFramePresentedServerMonotonicMs = -1;
	qint64 m_firstFramePresentedLocalSteadyMs = -1;
	QSet<int> m_screensAwaitingFirstFrame;
	// Kept separately while clearScene() drains deferred events, because that
	// function resets the prepared/active identifiers before it processes them.
	QString m_startingSenderClientId;
	QString m_startingSceneInstanceId;
	QString m_lastStoppedSenderClientId;
	QString m_lastStoppedSceneInstanceId;
	int m_totalMediaToPrime = 0;
	int m_mediaReadyCount = 0;
	bool m_sceneActivationRequested = false;
	bool m_sceneActivated = false;
	qint64 m_committedActivationLeadMs = 0;
	qint64 m_activationEpochMs = 0;
	bool m_activationClockPlausible = false;
	qint64 m_lastVideoSyncSequence = 0;
	QTimer* m_sceneReadyTimeout = nullptr;
	QTimer* m_activationTimer = nullptr;
	QTimer* m_windowShowTimer = nullptr; // Timer for deferred window showing
	bool m_teardownInProgress = false;
	quint64 m_teardownBarrierEpoch = 0;
	bool m_teardownCompletionScheduled = false;
	QSet<QObject*> m_pendingTeardownObjects;
	QSet<QString> m_teardownSessionWaiters;
	QSet<QString> m_pendingEmptySessionTeardowns;
	QString m_teardownGraphRemoteSessionId;
	bool m_sceneStartInProgress = false;
	PendingSceneRequest m_deferredSceneStart;
};

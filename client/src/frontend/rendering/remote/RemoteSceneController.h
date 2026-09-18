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
#include <QElapsedTimer>
#include "backend/domain/scene/SceneTimeline.h"
#include <memory>

#include "backend/network/RemoteCacheStore.h"
#include "backend/platform/LocalScreenTopology.h"

class WebSocketClient;
class FileManager;
class ResidentVideoPlayer;
class QVideoSink;
class QAudioOutput;
class QQuickWindow;
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
	friend class RemoteSceneControllerLifecycleTest;
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
		QPointer<QScreen> targetScreen;
		QString screenIdentity;
		int x=0,y=0,w=0,h=0;
		quint64 sceneEpoch = 0;
	};
	struct RemoteMediaItem {
		SceneTimeline::ElementState baseState;
		SceneTimeline::MediaTrack timeline;
		QString timelineClipId;
		qint64 timelineSeekGuardUntilMs = 0;
		qint64 timelineRequestedSourceMs = 0;
		bool timelineVideoPlaying = false;
		bool timelinePixelsVisible = false;
		QString mediaId;
		QString fileId;
		QString fileName;
        QString residencyOwner;
		QString type; // image | video | text
		// Text-specific properties
		QString text;
		QString fontFamily;
		bool fontItalic = false;
		bool fontUnderline = false;
		bool fontUppercase = false;
		qreal fontWeight = 400;
		qreal fontPixelSize = 1;
		QString textColor;
		double textOutlineWidthPx = 0.0;
		QString textBorderColor;
		bool fitToTextEnabled = false;
		bool highlightEnabled = false;
		QString textHighlightColor;
		int baseWidth = 0;
		int baseHeight = 0;
		double z = 0.0;
		bool clipActive = false;
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
			qsizetype modelRow = -1;
			bool qmlReady = false;
		};
		QList<Span> spans;
        double contentOpacity = 1.0;
        bool muted = false;
        double volume = 1.0;
        bool primedFirstFrame = false;
        bool loaded = false;
        bool readyNotified = false;
        QVideoFrame primedFrame;
        ResidentVideoPlayer* player = nullptr;
        QAudioOutput* audio = nullptr;
        QMetaObject::Connection mirrorConn;
        quint64 sceneEpoch = 0;
        QVideoSink* liveSink = nullptr;
        bool videoOutputsAttached = false;
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
	void watchLocalScreen(QScreen* screen);
	void handleLocalScreenRemoved(QScreen* screen);
	void refreshScreenBindings(const QList<LocalScreenTopology::Screen>& screens);
	void updateScreenGeometry(int screenId, QScreen* screen, const QRect& geometry);
	void buildMedia(const QJsonArray& mediaArray);
	void scheduleMedia(const std::shared_ptr<RemoteMediaItem>& item);
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
    void handleSceneReadyTimeout();
    void resetSceneSynchronization();
	void ensureVideoOutputsAttached(const std::shared_ptr<RemoteMediaItem>& item);
	void applyImageToSpans(const std::shared_ptr<RemoteMediaItem>& item, const QImage& image) const;
	void publishScreenModel(int screenId);
	void publishMediaSpan(const std::shared_ptr<RemoteMediaItem>& item, RemoteMediaItem::Span& span);
	bool allSpansReady(const std::shared_ptr<RemoteMediaItem>& item) const;
	bool matchesSceneEnvelope(const QJsonObject& envelope) const;
	void sendPrepareResult(bool success, const QString& message = QString());
	void tryArmPreparedScene();
	void retrySceneAcknowledgements(bool replay = false);
	void sendFirstFramePresented(bool forceReplay = false);
	void disconnectFirstFrameObservers();
	void updatePrepareProgress();
	void advanceTimeline();
	void evaluateTimelineAt(qreal positionMs, bool playing);
	void updateTimelineGeometry(const std::shared_ptr<RemoteMediaItem>& item,
	                            const SceneTimeline::ElementState& state);
	QString receivedFilePath(const QString& fileId) const;
	RemoteCacheStore::Scope receivedFileScope() const;


	private:
	QString m_residencyGroup;
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
	QJsonObject m_pendingSceneCommit;
	bool m_scenePreparedReported = false;
	bool m_sceneAllPrepared = false;
	bool m_sceneArmedReported = false;
	bool m_sceneCommitReceived = false;
	bool m_firstFrameReported = false;
	bool m_firstFrameAcknowledged = false;
	qint64 m_lastPrepareAckAttemptMs = -1;
	qint64 m_lastArmedAckAttemptMs = -1;
	qint64 m_lastStartedAckAttemptMs = -1;
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
	qint64 m_activationLocalSteadyMs = -1;
	qint64 m_sceneRecoveryDeadlineMs = -1;
	qint64 m_activationEpochMs = 0;
	bool m_activationClockPlausible = false;
	qint64 m_lastVideoSyncSequence = 0;
	QTimer* m_sceneReadyTimeout = nullptr;
	QTimer* m_activationTimer = nullptr;
	QTimer* m_windowShowTimer = nullptr; // Timer for deferred window showing
	QTimer m_screenRefreshTimer;
	bool m_teardownInProgress = false;
	quint64 m_teardownBarrierEpoch = 0;
	bool m_teardownCompletionScheduled = false;
	QSet<QObject*> m_pendingTeardownObjects;
	QSet<QString> m_teardownSessionWaiters;
	QSet<QString> m_pendingEmptySessionTeardowns;
	QString m_teardownGraphRemoteSessionId;
	bool m_sceneStartInProgress = false;
	PendingSceneRequest m_deferredSceneStart;
	SceneTimeline::SceneSettings m_timelineSettings;
	QTimer m_timelineTimer;
	QElapsedTimer m_timelineClock;
	qint64 m_timelineStartServerMs = -1;
	qreal m_timelineAnchorMs = 0;
	qreal m_timelinePositionMs = 0;
	bool m_timelineFinished = false;
	bool m_batchTimelinePublishing = false;
	QSet<int> m_dirtyTimelineScreens;
};

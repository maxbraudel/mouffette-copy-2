// RemoteSceneController.h - manages rendering of a host client's scene on a remote client
#pragma once

#include <QObject>
#include <QMap>
#include <QList>
#include <QJsonObject>
#include <QTimer>
#include <QPixmap>
#include <QSharedPointer>
#include <QByteArray>
#include <QVideoFrame>
#include <QImage>
#include <QPointer>
#include <memory>

class WebSocketClient;
class FileManager;
class QWidget;
class QMediaPlayer;
class QVideoSink;
class QAudioOutput;
class QBuffer;
class QGraphicsView;
class QGraphicsScene;
class QGraphicsPixmapItem;
class QGraphicsTextItem;
class QVariantAnimation;

class RemoteSceneController : public QObject {
	Q_OBJECT
public:
	explicit RemoteSceneController(FileManager* fileManager, WebSocketClient* ws, QObject* parent = nullptr);
	~RemoteSceneController() override;
	void setEnabled(bool en) { m_enabled = en; if (!en) clearScene(); }
	bool isEnabled() const { return m_enabled; }

private slots:
	void onRemoteSceneStart(const QString& senderClientId, const QJsonObject& scene);
	void onRemoteSceneStop(const QString& senderClientId);
	void onConnectionLost();
	void onConnectionError(const QString& errorMessage);

private:
	struct ScreenWindow {
		QWidget* window = nullptr;
		QGraphicsView* graphicsView = nullptr;
		QGraphicsScene* scene = nullptr;
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
		int fontSize = 12;
		bool fontBold = false;
		bool fontItalic = false;
		int fontWeight = 0; // 0 falls back to bold flag
		QString textColor;
		double textBorderWidthPercent = 0.0;
		QString textBorderColor;
		bool fitToTextEnabled = false;
		bool highlightEnabled = false;
		QString textHighlightColor;
		int baseWidth = 0;
		int baseHeight = 0;
		double uniformScale = 1.0;
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
			QWidget* widget = nullptr;
			QGraphicsPixmapItem* imageItem = nullptr;
			QGraphicsTextItem* textItem = nullptr;
			bool requiresRender = false;
			bool renderReady = false;
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
		bool repeatEnabled = false; int repeatCount = 0; int repeatRemaining = 0; bool repeatActive = false;
		bool primedFirstFrame = false; bool playAuthorized = false;
		bool displayReady = false; bool displayStarted = false;
		bool hiding = false;
		bool pausedAtEnd = false;
		bool loaded = false; // true when QMediaPlayer reports Loaded/Buffered
		bool readyNotified = false; // true after controller counts this media as ready
		bool fadeInPending = false; // true when fade requested before global activation
		qint64 startPositionMs = 0; bool hasStartPosition = false;
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
		// In-memory source support
		QSharedPointer<QByteArray> memoryBytes;
		QBuffer* memoryBuffer = nullptr;
		bool usingMemoryBuffer = false;
		int pendingDisplayDelayMs = -1;
		int pendingPlayDelayMs = -1;
		int pendingPauseDelayMs = -1;
		QVideoSink* primingSink = nullptr;
		QVideoSink* liveSink = nullptr;
		bool videoOutputsAttached = false;
		bool primedFrameSticky = false;
		bool primedFrameDeferred = false;
		QTimer* muteTimer = nullptr;
		QTimer* hideEndDelayTimer = nullptr;
		QTimer* muteEndDelayTimer = nullptr;
		bool hideEndTriggered = false;
		bool muteEndTriggered = false;
		bool holdLastFrameAtEnd = false;
		QImage lastFrameImage;
		QPixmap lastFramePixmap;
	};

	struct PendingSceneRequest {
		QString senderId;
		QJsonObject scene;
		bool valid = false;
	};

	QWidget* ensureScreenWindow(int screenId, int x, int y, int w, int h, bool primary);
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
	void scheduleSceneRestartCooldown();
	void drainDeferredDeletes(int passes = 1, bool processEvents = false);
    void teardownMediaItem(const std::shared_ptr<RemoteMediaItem>& item);
    void markItemReady(const std::shared_ptr<RemoteMediaItem>& item);
    void evaluateItemReadiness(const std::shared_ptr<RemoteMediaItem>& item);
    void startSceneActivationIfReady();
    void activateScene();
    void startDeferredTimers();
    void handleSceneReadyTimeout();
    void resetSceneSynchronization();
    void seekToConfiguredStart(const std::shared_ptr<RemoteMediaItem>& item);
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
	void applyPixmapToSpans(const std::shared_ptr<RemoteMediaItem>& item, const QPixmap& pixmap) const;

	// Phase 4.3: FileManager injected (not singleton)
	FileManager* m_fileManager = nullptr;
	
	WebSocketClient* m_ws = nullptr; // not owned
	bool m_enabled = true;
	QMap<int, ScreenWindow> m_screenWindows;
	QList<std::shared_ptr<RemoteMediaItem>> m_mediaItems;
	quint64 m_sceneEpoch = 0; // incremented on each start/stop
	QString m_pendingSenderClientId;
	int m_totalMediaToPrime = 0;
	int m_mediaReadyCount = 0;
	bool m_sceneActivationRequested = false;
	bool m_sceneActivated = false;
	quint64 m_pendingActivationEpoch = 0;
	QTimer* m_sceneReadyTimeout = nullptr;
	QTimer* m_windowShowTimer = nullptr; // Timer for deferred window showing
	bool m_teardownInProgress = false;
	bool m_sceneStartInProgress = false;
	QTimer* m_sceneRestartDelayTimer = nullptr;
	bool m_restartCooldownActive = false;
	PendingSceneRequest m_deferredSceneStart;
};


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
#include <memory>

class WebSocketClient;
class QWidget;
class QMediaPlayer;
class QVideoSink;
class QAudioOutput;
class QBuffer;
class QGraphicsView;
class QGraphicsVideoItem;
class QGraphicsScene;
class QGraphicsPixmapItem;

class RemoteSceneController : public QObject {
	Q_OBJECT
public:
	explicit RemoteSceneController(WebSocketClient* ws, QObject* parent = nullptr);
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
	};
	struct RemoteMediaItem {
		QString mediaId;
		QString fileId;
		QString fileName;
		QString type; // image | video
		// Multi-screen spans support: each span maps to a screen with its own normalized geom
		struct Span {
			int screenId = -1;
			double nx = 0, ny = 0, nw = 0, nh = 0;
			QWidget* widget = nullptr;
			QGraphicsPixmapItem* imageItem = nullptr;
			QGraphicsVideoItem* videoItem = nullptr;
		};
		QList<Span> spans;
		bool autoDisplay=false; int autoDisplayDelayMs=0;
		bool autoPlay=false; int autoPlayDelayMs=0;
		bool autoPause=false; int autoPauseDelayMs=0;
		bool autoHide=false; int autoHideDelayMs=0;
		bool hideWhenVideoEnds=false;
		double fadeInSeconds=0.0; double fadeOutSeconds=0.0; double contentOpacity = 1.0;
		// Audio state from host (videos)
		bool muted = false; double volume = 1.0; // 0..1
		bool autoUnmute = false; int autoUnmuteDelayMs = 0;
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
		bool videoOutputsAttached = false;
		bool primedFrameSticky = false;
	};

	QWidget* ensureScreenWindow(int screenId, int x, int y, int w, int h, bool primary);
	void buildWindows(const QJsonArray& screensArray);
	void buildMedia(const QJsonArray& mediaArray);
	void scheduleMedia(const std::shared_ptr<RemoteMediaItem>& item);
	void scheduleMediaMulti(const std::shared_ptr<RemoteMediaItem>& item);
	void fadeIn(const std::shared_ptr<RemoteMediaItem>& item);
	void fadeOutAndHide(const std::shared_ptr<RemoteMediaItem>& item);
	void scheduleHideTimer(const std::shared_ptr<RemoteMediaItem>& item);
	void clearScene();
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
	void clearVideoSinks(const std::shared_ptr<RemoteMediaItem>& item);
	void ensureVideoOutputsAttached(const std::shared_ptr<RemoteMediaItem>& item);
	void finalizeLivePlaybackStart(const std::shared_ptr<RemoteMediaItem>& item, const QVideoFrame& frame);
    qint64 targetDisplayTimestamp(const std::shared_ptr<RemoteMediaItem>& item) const;

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
};


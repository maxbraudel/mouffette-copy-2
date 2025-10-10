// RemoteSceneController.h - manages rendering of a host client's scene on a remote client
#pragma once

#include <QObject>
#include <QMap>
#include <QList>
#include <QJsonObject>
#include <QTimer>
#include <QGraphicsOpacityEffect>
#include <QElapsedTimer>
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
class QLabel;
class QBuffer;
class QGraphicsView;
class QGraphicsVideoItem;
class QGraphicsScene;

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
		// Legacy single-span fields (when spans[] not provided)
		int screenId = -1; double normX=0, normY=0, normW=0, normH=0;
		// Multi-screen spans support: each span maps to a screen with its own normalized geom
		struct Span { 
			int screenId=-1; 
			double nx=0, ny=0, nw=0, nh=0; 
			QWidget* widget=nullptr; 
			QGraphicsView* graphicsView=nullptr;
			QGraphicsScene* scene=nullptr;
			QLabel* imageLabel=nullptr; 
			QGraphicsVideoItem* videoItem=nullptr;
		};
		QList<Span> spans;
		bool autoDisplay=false; int autoDisplayDelayMs=0;
		bool autoPlay=false; int autoPlayDelayMs=0;
		double fadeInSeconds=0.0; double fadeOutSeconds=0.0; double contentOpacity = 1.0;
		// Audio state from host (videos)
		bool muted = false; double volume = 1.0; // 0..1
		bool repeatEnabled = false; int repeatCount = 0; int repeatRemaining = 0; bool repeatActive = false;
		bool primedFirstFrame = false; bool playAuthorized = false;
		bool displayReady = false; bool displayStarted = false;
		bool hasLastVideoFrame = false; bool frameCacheAttached = false;
		bool pausedAtEnd = false;
		QVideoFrame lastVideoFrame;
		QMetaObject::Connection frameCacheConn;
		bool loaded = false; // true when QMediaPlayer reports Loaded/Buffered
		// For legacy single-span path
		QWidget* widget = nullptr; QGraphicsOpacityEffect* opacity = nullptr;
		QGraphicsView* graphicsViewSingle = nullptr;
		QGraphicsScene* sceneSingle = nullptr;
		QGraphicsVideoItem* videoItemSingle = nullptr;
		QLabel* imageLabelSingle = nullptr;
		QTimer* displayTimer = nullptr; QTimer* playTimer = nullptr;
		// Video only
		QMediaPlayer* player = nullptr; QAudioOutput* audio = nullptr;
		QMetaObject::Connection deferredStartConn; // one-shot start after load
		QMetaObject::Connection primingConn; // one-shot first-frame priming when autoPlay=false
		quint64 sceneEpoch = 0; // generation token to guard delayed actions
		// In-memory source support
		QSharedPointer<QByteArray> memoryBytes;
		QBuffer* memoryBuffer = nullptr;
		bool usingMemoryBuffer = false;
	};

	QWidget* ensureScreenWindow(int screenId, int x, int y, int w, int h, bool primary);
	void buildWindows(const QJsonArray& screensArray);
	void buildMedia(const QJsonArray& mediaArray);
	void scheduleMedia(const std::shared_ptr<RemoteMediaItem>& item);
	void scheduleMediaLegacy(const std::shared_ptr<RemoteMediaItem>& item);
	void scheduleMediaMulti(const std::shared_ptr<RemoteMediaItem>& item);
	void fadeIn(const std::shared_ptr<RemoteMediaItem>& item);
	void clearScene();
    void teardownMediaItem(const std::shared_ptr<RemoteMediaItem>& item);

	WebSocketClient* m_ws = nullptr; // not owned
	bool m_enabled = true;
	QMap<int, ScreenWindow> m_screenWindows;
	QList<std::shared_ptr<RemoteMediaItem>> m_mediaItems;
	quint64 m_sceneEpoch = 0; // incremented on each start/stop
};


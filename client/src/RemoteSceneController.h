// RemoteSceneController.h - manages rendering of a host client's scene on a remote client
#pragma once

#include <QObject>
#include <QMap>
#include <QList>
#include <QJsonObject>
#include <QTimer>
#include <QGraphicsOpacityEffect>
#include <QPixmap>
#include <QBuffer>

class WebSocketClient;
class QWidget;
class QMediaPlayer;
class QVideoSink;
class QAudioOutput;

class RemoteSceneController : public QObject {
	Q_OBJECT
public:
	explicit RemoteSceneController(WebSocketClient* ws, QObject* parent = nullptr);
	void setEnabled(bool en) { m_enabled = en; if (!en) clearScene(); }
	bool isEnabled() const { return m_enabled; }

private slots:
	void onRemoteSceneStart(const QString& senderClientId, const QJsonObject& scene);
	void onRemoteSceneStop(const QString& senderClientId);

private:
	struct ScreenWindow {
		QWidget* window = nullptr; int x=0,y=0,w=0,h=0;
	};
	struct RemoteMediaItem {
		QString mediaId;
		QString fileId;
		QString fileName;
		QString type; // image | video
		int screenId = -1;
		double normX=0, normY=0, normW=0, normH=0;
		bool autoDisplay=false; int autoDisplayDelayMs=0;
		bool autoPlay=false; int autoPlayDelayMs=0;
		double fadeInSeconds=0.0; double fadeOutSeconds=0.0; double contentOpacity = 1.0;
		// Audio state from host (videos)
		bool muted = false; double volume = 1.0; // 0..1
		bool primedFirstFrame = false; bool playAuthorized = false;
		bool loaded = false; // true when QMediaPlayer reports Loaded/Buffered
		QWidget* widget = nullptr; QGraphicsOpacityEffect* opacity = nullptr;
		QTimer* displayTimer = nullptr; QTimer* playTimer = nullptr;
		// Video only
		QMediaPlayer* player = nullptr; QVideoSink* videoSink = nullptr; QAudioOutput* audio = nullptr;
		QMetaObject::Connection deferredStartConn; // one-shot start after load
		QMetaObject::Connection primingConn; // one-shot first-frame priming when autoPlay=false
		// Preload buffers to ensure zero-lag playback
		QBuffer* videoBuffer = nullptr; // owned via QObject parent (widget)
		QPixmap imagePixmap; // for images
	};

	QWidget* ensureScreenWindow(int screenId, int x, int y, int w, int h, bool primary);
	void buildWindows(const QJsonArray& screensArray);
	void buildMedia(const QJsonArray& mediaArray);
	void scheduleMedia(RemoteMediaItem* item);
	void fadeIn(RemoteMediaItem* item);
	void clearScene();

	WebSocketClient* m_ws = nullptr; // not owned
	bool m_enabled = true;
	QMap<int, ScreenWindow> m_screenWindows;
	QList<RemoteMediaItem*> m_mediaItems;
};


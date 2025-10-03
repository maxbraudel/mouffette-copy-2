#ifndef REMOTESCENECONTROLLER_H
#define REMOTESCENECONTROLLER_H

#include <QObject>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <QLabel>
#include <QVideoSink>
#include <QMediaPlayer>
#include <QGraphicsOpacityEffect>
#include "WebSocketClient.h"
#include "FileManager.h"

// Lightweight media holder for remote display
struct RemoteMediaItem {
    QString mediaId;
    QString fileId;
    QString type; // image | video
    QString fileName; // original filename (no path)
    int screenId = -1;
    double normX = 0, normY = 0, normW = 0, normH = 0; // relative geometry within screen
    bool autoDisplay = false;
    int autoDisplayDelayMs = 0;
    bool autoPlay = false;
    int autoPlayDelayMs = 0;
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
    QWidget* widget = nullptr; // QLabel for image or video surface container
    QGraphicsOpacityEffect* opacity = nullptr;
    QMediaPlayer* player = nullptr; // for video
    QVideoSink* videoSink = nullptr; // for video frame updates without QVideoWidget
    QTimer* displayTimer = nullptr;
    QTimer* playTimer = nullptr;
};

class RemoteSceneController : public QObject {
    Q_OBJECT
public:
    explicit RemoteSceneController(WebSocketClient* ws, QObject* parent=nullptr);
    void setActive(bool enabled) { m_enabled = enabled; }

private slots:
    void onRemoteSceneStart(const QString& senderClientId, const QJsonObject& scene);
    void onRemoteSceneStop(const QString& senderClientId);

private:
    void clearScene();
    void buildWindows(const QJsonArray& screensArray);
    void buildMedia(const QJsonArray& mediaArray);
    QWidget* ensureScreenWindow(int screenId, int x, int y, int w, int h, bool primary);
    void scheduleMedia(RemoteMediaItem* item);
    void fadeIn(RemoteMediaItem* item);

    bool m_enabled = true; // allow external gating later
    WebSocketClient* m_ws = nullptr;
    struct ScreenWindow { QWidget* window = nullptr; int x=0; int y=0; int w=0; int h=0; };
    QMap<int, ScreenWindow> m_screenWindows; // screenId -> window
    QList<RemoteMediaItem*> m_mediaItems; // owned
};

#endif // REMOTESCENECONTROLLER_H

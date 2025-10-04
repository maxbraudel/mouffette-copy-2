#include "RemoteSceneController.h"
#include "WebSocketClient.h"
#include <QJsonArray>
#include <QScreen>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPixmap>
#include <QFileInfo>
#include <QDebug>
#include <QVideoFrame>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <QWidget>
#include <QLabel>
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QVideoSink>
#include "FileManager.h"
#include "MacWindowManager.h"
#include <algorithm>

RemoteSceneController::RemoteSceneController(WebSocketClient* ws, QObject* parent)
    : QObject(parent), m_ws(ws) {
    if (m_ws) {
        connect(m_ws, &WebSocketClient::remoteSceneStartReceived, this, &RemoteSceneController::onRemoteSceneStart);
        connect(m_ws, &WebSocketClient::remoteSceneStopReceived, this, &RemoteSceneController::onRemoteSceneStop);
    }
}

void RemoteSceneController::onRemoteSceneStart(const QString& senderClientId, const QJsonObject& scene) {
    Q_UNUSED(senderClientId);
    if (!m_enabled) return;
    clearScene();
    const QJsonArray screens = scene.value("screens").toArray();
    const QJsonArray media = scene.value("media").toArray();
    buildWindows(screens);
    buildMedia(media);
    // Show windows after populated
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        if (it.value().window) it.value().window->show();
#ifdef Q_OS_MAC
        if (it.value().window) {
            // Apply overlay behavior after show to avoid Qt resetting native levels on show()
            QTimer::singleShot(0, it.value().window, [w = it.value().window]() {
                MacWindowManager::setWindowAsGlobalOverlay(w, /*clickThrough*/ true);
            });
        }
#endif
    }
}

void RemoteSceneController::onRemoteSceneStop(const QString& senderClientId) {
    Q_UNUSED(senderClientId);
    clearScene();
}

void RemoteSceneController::clearScene() {
    for (RemoteMediaItem* item : m_mediaItems) {
        if (!item) continue;
        if (item->displayTimer) { item->displayTimer->stop(); item->displayTimer->deleteLater(); }
        if (item->playTimer) { item->playTimer->stop(); item->playTimer->deleteLater(); }
    if (item->player) { item->player->stop(); item->player->deleteLater(); }
    if (item->audio) { item->audio->deleteLater(); }
        if (item->widget) { item->widget->deleteLater(); }
        delete item;
    }
    m_mediaItems.clear();
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        if (it.value().window) it.value().window->close();
        if (it.value().window) it.value().window->deleteLater();
    }
    m_screenWindows.clear();
}

QWidget* RemoteSceneController::ensureScreenWindow(int screenId, int x, int y, int w, int h, bool primary) {
    if (m_screenWindows.contains(screenId)) return m_screenWindows[screenId].window;
    QWidget* win = new QWidget();
    win->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    win->setWindowFlag(Qt::FramelessWindowHint, true);
    win->setWindowFlag(Qt::WindowStaysOnTopHint, true);
    // On Windows, prevent appearing in taskbar and avoid focus/activation
#ifdef Q_OS_WIN
    win->setWindowFlag(Qt::Tool, true);
    win->setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
    win->setAttribute(Qt::WA_ShowWithoutActivating, true);
#endif
    win->setAttribute(Qt::WA_TranslucentBackground, true);
    win->setObjectName(QString("RemoteScreenWindow_%1").arg(screenId));
    win->setGeometry(x, y, w, h);
    win->setWindowTitle(primary ? "Remote Scene (Primary)" : "Remote Scene");
    auto* layout = new QHBoxLayout(win);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(0);
    ScreenWindow sw; sw.window = win; sw.x=x; sw.y=y; sw.w=w; sw.h=h; m_screenWindows.insert(screenId, sw);

    // On macOS, elevate this window to a global overlay that spans Spaces and is hidden from Mission Control
#ifdef Q_OS_MAC
    MacWindowManager::setWindowAsGlobalOverlay(win, /*clickThrough*/ true);
#endif
    return win;
}

void RemoteSceneController::buildWindows(const QJsonArray& screensArray) {
    // Map host screen list to local physical screens by index.
    const QList<QScreen*> localScreens = QGuiApplication::screens();
    QScreen* primaryLocal = QGuiApplication::primaryScreen();
    int hostIndex = 0;
    for (const auto& v : screensArray) {
        QJsonObject o = v.toObject();
        int hostScreenId = o.value("id").toInt();
        // Pick local screen (fallback to primary if fewer local screens than host screens)
        QScreen* target = (hostIndex < localScreens.size()) ? localScreens[hostIndex] : primaryLocal;
        QRect geom = target ? target->geometry() : QRect(0,0,o.value("width").toInt(), o.value("height").toInt());
        bool primary = target && target == primaryLocal;
        ensureScreenWindow(hostScreenId, geom.x(), geom.y(), geom.width(), geom.height(), primary);
        ++hostIndex;
    }
    qDebug() << "RemoteSceneController: created" << m_screenWindows.size() << "remote screen windows (host screens:" << screensArray.size() << ", local screens:" << localScreens.size() << ")";
}

void RemoteSceneController::buildMedia(const QJsonArray& mediaArray) {
    // QGraphicsScene::items() (used on host serialization) returns items in descending Z (topmost first) by default.
    // If we create children in that order, later widgets sit on top of earlier ones, reversing the stack.
    // Therefore, build from the end to the beginning so the topmost item is created last and remains on top.
    for (int idx = mediaArray.size() - 1; idx >= 0; --idx) {
        const auto& v = mediaArray.at(idx);
        QJsonObject m = v.toObject();
        RemoteMediaItem* item = new RemoteMediaItem();
        item->mediaId = m.value("mediaId").toString();
    item->fileId = m.value("fileId").toString();
        item->type = m.value("type").toString();
    item->fileName = m.value("fileName").toString();
        item->screenId = m.value("screenId").toInt(-1);
        item->normX = m.value("normX").toDouble();
        item->normY = m.value("normY").toDouble();
        item->normW = m.value("normW").toDouble();
        item->normH = m.value("normH").toDouble();
        item->autoDisplay = m.value("autoDisplay").toBool(false);
        item->autoDisplayDelayMs = m.value("autoDisplayDelayMs").toInt(0);
        item->autoPlay = m.value("autoPlay").toBool(false);
        item->autoPlayDelayMs = m.value("autoPlayDelayMs").toInt(0);
        item->fadeInSeconds = m.value("fadeInSeconds").toDouble(0.0);
        item->fadeOutSeconds = m.value("fadeOutSeconds").toDouble(0.0);
        item->contentOpacity = m.value("contentOpacity").toDouble(1.0);
        if (item->type == "video") {
            item->muted = m.value("muted").toBool(false);
            item->volume = m.value("volume").toDouble(1.0);
        }
        m_mediaItems.append(item);
        scheduleMedia(item);
    }
}

void RemoteSceneController::scheduleMedia(RemoteMediaItem* item) {
    if (!item) return;
    auto winIt = m_screenWindows.find(item->screenId);
    if (winIt == m_screenWindows.end()) return;
    QWidget* container = winIt.value().window;
    if (!container) return;
    // Create widget hidden initially
    QWidget* w = new QWidget(container);
    w->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    w->setAutoFillBackground(false);
    w->hide();
    item->widget = w;
    item->opacity = new QGraphicsOpacityEffect(w);
    // Start at 0 for fade if autoDisplay enabled otherwise remain hidden; actual target will be contentOpacity
    item->opacity->setOpacity(0.0);
    w->setGraphicsEffect(item->opacity);

    // Geometry
    int px = int(item->normX * container->width());
    int py = int(item->normY * container->height());
    int pw = int(item->normW * container->width());
    int ph = int(item->normH * container->height());
    if (pw <=0 || ph <=0) { pw = 10; ph = 10; }
    w->setGeometry(px, py, pw, ph);
    qDebug() << "RemoteSceneController: scheduling media" << item->mediaId << "type" << item->type
             << "screenId" << item->screenId << "geom(px,py,w,h)=" << px << py << pw << ph
             << "autoDisplayDelayMs" << item->autoDisplayDelayMs << "autoPlayDelayMs" << item->autoPlayDelayMs;

    if (item->type == "image") {
        QLabel* lbl = new QLabel(w);
        lbl->setScaledContents(true);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setGeometry(0,0,pw,ph);
        auto attemptLoad = [lbl,item]() {
            QString path = FileManager::instance().getFilePathForId(item->fileId);
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                // Preload fully into RAM to avoid disk IO at show time
                QPixmap pm;
                if (pm.load(path)) { lbl->setPixmap(pm); return true; }
            }
            return false;
        };
        if (!attemptLoad()) {
            lbl->setText(QString("Missing file\n%1").arg(item->fileName.isEmpty()? item->fileId : item->fileName));
            // Retry a few times (e.g., if upload arrives slightly after scene start)
            for (int i=1;i<=5;++i) {
                QTimer::singleShot(i*500, w, [attemptLoad]() { attemptLoad(); });
            }
        }
    } else if (item->type == "video") {
        item->player = new QMediaPlayer(w);
        item->audio = new QAudioOutput(w);
        // Apply host audio state before playback
        item->audio->setMuted(item->muted);
        item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0));
        item->player->setAudioOutput(item->audio);
        item->videoSink = new QVideoSink(w);
        QLabel* videoLabel = new QLabel(w);
        videoLabel->setScaledContents(true);
        videoLabel->setGeometry(0,0,pw,ph);
        videoLabel->setAttribute(Qt::WA_TranslucentBackground, true);
        videoLabel->setAutoFillBackground(false);
        videoLabel->setStyleSheet("background: transparent;");
    videoLabel->setPixmap(QPixmap());
    videoLabel->show();
        QObject::connect(item->videoSink, &QVideoSink::videoFrameChanged, videoLabel, [videoLabel,item](const QVideoFrame& frame){
            if (!frame.isValid()) return;
            const QImage img = frame.toImage();
            if (!img.isNull()) videoLabel->setPixmap(QPixmap::fromImage(img));
        });
        item->player->setVideoSink(item->videoSink);
        QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [item](QMediaPlayer::MediaStatus s){
            qDebug() << "RemoteSceneController: mediaStatus" << int(s) << "for" << item->mediaId;
            if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
                item->loaded = true;
            }
        });
        QObject::connect(item->player, &QMediaPlayer::playbackStateChanged, item->player, [item](QMediaPlayer::PlaybackState st){
            qDebug() << "RemoteSceneController: playbackState" << int(st) << "for" << item->mediaId;
        });
        QObject::connect(item->player, &QMediaPlayer::errorOccurred, item->player, [item](QMediaPlayer::Error e, const QString& err){
            if (e != QMediaPlayer::NoError) qWarning() << "RemoteSceneController: player error" << int(e) << err << "for" << item->mediaId;
        });
        auto attemptLoadVid = [item]() {
            QString path = FileManager::instance().getFilePathForId(item->fileId);
            qDebug() << "RemoteSceneController: resolving video path for" << item->mediaId << "->" << path;
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                // Use file URL directly for backend compatibility
                item->player->setSource(QUrl::fromLocalFile(path));
                // Prime only for preview when autoPlay is disabled: play muted, then pause on first video frame
                if (!item->autoPlay && !item->primedFirstFrame) {
                    item->primingConn = QObject::connect(item->videoSink, &QVideoSink::videoFrameChanged, item->player, [item](const QVideoFrame& f){
                        if (!f.isValid()) return;
                        if (item->primedFirstFrame) return;
                        item->primedFirstFrame = true;
                        QObject::disconnect(item->primingConn);
                        if (!item->playAuthorized && item->player) {
                            item->player->pause();
                            item->player->setPosition(0);
                        }
                    });
                    if (item->audio) item->audio->setMuted(true);
                    item->player->play();
                }
                return true;
            }
            qWarning() << "RemoteSceneController: video path not available yet for" << item->mediaId << ", will retry";
            return false;
        };
        if (!attemptLoadVid()) {
            // Retry similarly for videos
            for (int i=1;i<=5;++i) QTimer::singleShot(i*500, w, [attemptLoadVid]() { attemptLoadVid(); });
        }
    }

    // Display scheduling: only if autoDisplay enabled. Otherwise remain hidden until a future command (not yet implemented).
    if (item->autoDisplay) {
        int delay = item->autoDisplayDelayMs;
        item->displayTimer = new QTimer(this);
        item->displayTimer->setSingleShot(true);
        connect(item->displayTimer, &QTimer::timeout, this, [this,item]() { fadeIn(item); });
        item->displayTimer->start(delay);
        if (delay == 0) qDebug() << "RemoteSceneController: immediate display for" << item->mediaId;
    } else {
        qDebug() << "RemoteSceneController: autoDisplay disabled; media will stay hidden" << item->mediaId;
    }

    // Play scheduling: only if autoPlay enabled AND we will display (avoid hidden playback)
    if (item->player && item->autoDisplay && item->autoPlay) {
        int playDelay = item->autoDisplayDelayMs + item->autoPlayDelayMs; // start after display (host parity)
        item->playTimer = new QTimer(this);
        item->playTimer->setSingleShot(true);
        connect(item->playTimer, &QTimer::timeout, this, [item]() {
            item->playAuthorized = true;
            if (!item->player) return;
            // Apply final audio state from host now
            if (item->audio) { item->audio->setMuted(item->muted); item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0)); }
            if (item->loaded) {
                // If we primed, position might already be 0 and paused; just play
                if (item->player->position() != 0) item->player->setPosition(0);
                item->player->play();
            }
            else {
                // Defer until loaded to avoid starting before buffer is ready
                item->deferredStartConn = QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [item](QMediaPlayer::MediaStatus s){
                    if (!item->playAuthorized) return;
                    if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
                        // Disconnect this one-shot handler to avoid multiple resets causing freezes
                        QObject::disconnect(item->deferredStartConn);
                        if (item->audio) { item->audio->setMuted(item->muted); item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0)); }
                        if (item->player->position() != 0) item->player->setPosition(0);
                        item->player->play();
                    }
                });
            }
        });
        item->playTimer->start(playDelay);
        if (playDelay == 0) qDebug() << "RemoteSceneController: immediate play for" << item->mediaId;
    } else if (item->player && !item->autoPlay) {
        qDebug() << "RemoteSceneController: autoPlay disabled; video will not start automatically" << item->mediaId;
    }
    // Make sure the most recently scheduled item for this screen is on top to match host stacking
    // (widgets created later are naturally on top, but explicit raise ensures correctness after reparenting)
    w->raise();
}

void RemoteSceneController::fadeIn(RemoteMediaItem* item) {
    if (!item || !item->widget || !item->opacity) return;
    item->widget->show();
    // Preview enforcement is handled during priming. No extra pause here to avoid fighting playback scheduling.
    const int durMs = int(item->fadeInSeconds * 1000.0);
    if (durMs <= 10) { item->opacity->setOpacity(item->contentOpacity); return; }
    auto* anim = new QVariantAnimation(item->widget);
    anim->setStartValue(0.0);
    anim->setEndValue(item->contentOpacity);
    anim->setDuration(durMs);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim, &QVariantAnimation::valueChanged, item->widget, [item](const QVariant& v){ if (item->opacity) item->opacity->setOpacity(v.toDouble()); });
    connect(anim, &QVariantAnimation::finished, item->widget, [item,anim](){ anim->deleteLater(); });
    anim->start();
}

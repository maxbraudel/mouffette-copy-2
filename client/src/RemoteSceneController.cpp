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
        // Delete multi-span widgets
        for (auto& s : item->spans) {
            if (s.videoSink) { s.videoSink->deleteLater(); }
            if (s.videoLabel) { s.videoLabel->deleteLater(); }
            if (s.imageLabel) { s.imageLabel->deleteLater(); }
            if (s.widget) { s.widget->deleteLater(); }
        }
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
    win->setAttribute(Qt::WA_NoSystemBackground, true);
    win->setAttribute(Qt::WA_OpaquePaintEvent, false);
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
        // Parse spans if present
        if (m.contains("spans") && m.value("spans").isArray()) {
            const QJsonArray spans = m.value("spans").toArray();
            for (const auto& sv : spans) {
                const QJsonObject so = sv.toObject();
                RemoteMediaItem::Span s; s.screenId = so.value("screenId").toInt(-1);
                s.nx = so.value("normX").toDouble(); s.ny = so.value("normY").toDouble(); s.nw = so.value("normW").toDouble(); s.nh = so.value("normH").toDouble();
                item->spans.append(s);
            }
        }
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
    if (!item->spans.isEmpty()) { scheduleMediaMulti(item); return; }
    scheduleMediaLegacy(item);
}

void RemoteSceneController::scheduleMediaLegacy(RemoteMediaItem* item) {
    auto winIt = m_screenWindows.find(item->screenId);
    if (winIt == m_screenWindows.end()) return;
    QWidget* container = winIt.value().window;
    if (!container) return;
    // Create widget hidden initially
    QWidget* w = new QWidget(container);
    w->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    w->setAutoFillBackground(false);
    // Ensure painting is clipped to the screen container; this makes partial off-screen content get cropped as desired
    w->setAttribute(Qt::WA_NoSystemBackground, true);
    w->setAttribute(Qt::WA_OpaquePaintEvent, false);
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
            const QString idOrName = item->fileId.isEmpty() ? item->fileName : item->fileId;
            lbl->setText(QString("Missing file (id)\n%1\nWaiting for upload...").arg(idOrName));
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
    QVideoSink* videoSink = new QVideoSink(w);
        QLabel* videoLabel = new QLabel(w);
        videoLabel->setScaledContents(true);
        videoLabel->setGeometry(0,0,pw,ph);
        videoLabel->setAttribute(Qt::WA_TranslucentBackground, true);
        videoLabel->setAutoFillBackground(false);
        videoLabel->setStyleSheet("background: transparent;");
    videoLabel->setPixmap(QPixmap());
    videoLabel->show();
        QObject::connect(videoSink, &QVideoSink::videoFrameChanged, videoLabel, [videoLabel,item](const QVideoFrame& frame){
            if (!frame.isValid()) return;
            const QImage img = frame.toImage();
            if (!img.isNull()) videoLabel->setPixmap(QPixmap::fromImage(img));
        });
        item->player->setVideoSink(videoSink);
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
        auto attemptLoadVid = [item, videoSink]() {
            QString path = FileManager::instance().getFilePathForId(item->fileId);
            qDebug() << "RemoteSceneController: resolving video path for" << item->mediaId << "->" << path;
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                // Use file URL directly for backend compatibility
                item->player->setSource(QUrl::fromLocalFile(path));
                // Prime only for preview when autoPlay is disabled: play muted, then pause on first video frame
                if (!item->autoPlay && !item->primedFirstFrame) {
                    item->primingConn = QObject::connect(videoSink, &QVideoSink::videoFrameChanged, item->player, [item](const QVideoFrame& f){
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

void RemoteSceneController::scheduleMediaMulti(RemoteMediaItem* item) {
    if (item->spans.isEmpty()) return;
    // Create per-span widgets under corresponding screen windows
    QList<QWidget*> spanWidgets;
    QList<QGraphicsOpacityEffect*> spanEffects;
    for (int i=0;i<item->spans.size();++i) {
        auto& s = item->spans[i];
        auto winIt = m_screenWindows.find(s.screenId);
        if (winIt == m_screenWindows.end()) continue;
        QWidget* container = winIt.value().window; if (!container) continue;
        QWidget* w = new QWidget(container);
        w->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        w->setAutoFillBackground(false);
        w->setAttribute(Qt::WA_NoSystemBackground, true);
        w->setAttribute(Qt::WA_OpaquePaintEvent, false);
        w->hide();
        // Geometry
        int px = int(s.nx * container->width());
        int py = int(s.ny * container->height());
        int pw = int(s.nw * container->width());
        int ph = int(s.nh * container->height());
        if (pw <=0 || ph <=0) { pw = 10; ph = 10; }
        w->setGeometry(px, py, pw, ph);
        // Opacity effect per span
        auto* eff = new QGraphicsOpacityEffect(w);
        eff->setOpacity(0.0);
        w->setGraphicsEffect(eff);
        s.widget = w; spanWidgets.append(w); spanEffects.append(eff);
        if (item->type == "image") {
            QLabel* lbl = new QLabel(w); lbl->setScaledContents(true); lbl->setAlignment(Qt::AlignCenter); lbl->setGeometry(0,0,pw,ph); s.imageLabel = lbl;
        } else if (item->type == "video") {
            QLabel* vLbl = new QLabel(w); vLbl->setScaledContents(true); vLbl->setGeometry(0,0,pw,ph); vLbl->setAttribute(Qt::WA_TranslucentBackground, true); vLbl->setAutoFillBackground(false); vLbl->setStyleSheet("background: transparent;"); vLbl->setPixmap(QPixmap()); vLbl->show(); s.videoLabel = vLbl;
            s.videoSink = new QVideoSink(w);
        }
        w->raise();
    }

    // Content loading
    if (item->type == "image") {
        auto attemptLoad = [item]() {
            QString path = FileManager::instance().getFilePathForId(item->fileId);
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                QPixmap pm; if (pm.load(path)) {
                    for (auto& s : item->spans) { if (s.imageLabel) s.imageLabel->setPixmap(pm); if (s.widget) s.widget->update(); }
                    return true;
                }
            }
            return false;
        };
        if (!attemptLoad()) { for (int i=1;i<=5;++i) QTimer::singleShot(i*500, nullptr, [attemptLoad]() { attemptLoad(); }); }
    } else if (item->type == "video") {
        // Single shared player/audio for all spans
        // Parent the player/audio to the first available span widget if possible, else nullptr
        QWidget* parentForAv = nullptr; if (!item->spans.isEmpty()) parentForAv = item->spans.first().widget;
        item->player = new QMediaPlayer(parentForAv);
        item->audio = new QAudioOutput(parentForAv);
        item->audio->setMuted(item->muted); item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0));
        item->player->setAudioOutput(item->audio);
        // Connect all span sinks to mirror frames into labels
        for (auto& s : item->spans) {
            if (!s.videoSink || !s.videoLabel) continue;
            QObject::connect(s.videoSink, &QVideoSink::videoFrameChanged, s.videoLabel, [&s](const QVideoFrame& frame){
                if (!frame.isValid()) return; const QImage img = frame.toImage(); if (!img.isNull() && s.videoLabel) s.videoLabel->setPixmap(QPixmap::fromImage(img));
            });
        }
        // We'll direct the player's sink dynamically; Qt supports only one sink at a time.
        // Strategy: keep player's sink set to the first span's sink; we still forward frames to other labels via the same sink signal connections above.
        // Note: QMediaPlayer emits frames only to its current sink; so we can't get frames for other sinks automatically.
        // Alternative approach: use one sink and share the QImage across labels in the slot above.
        QVideoSink* sharedSink = nullptr; if (!item->spans.isEmpty()) sharedSink = item->spans.first().videoSink;
        if (sharedSink) item->player->setVideoSink(sharedSink);
        // Additionally, mirror frames from shared sink to all other labels
        if (sharedSink) {
            QObject::connect(sharedSink, &QVideoSink::videoFrameChanged, sharedSink, [item](const QVideoFrame& f){
                if (!f.isValid()) return; const QImage img = f.toImage(); if (img.isNull()) return; QPixmap pm = QPixmap::fromImage(img);
                for (auto& s : item->spans) { if (s.videoLabel) s.videoLabel->setPixmap(pm); }
            });
        }
        QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [item](QMediaPlayer::MediaStatus s){ if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) item->loaded = true; });
        QObject::connect(item->player, &QMediaPlayer::errorOccurred, item->player, [item](QMediaPlayer::Error e, const QString& err){ if (e != QMediaPlayer::NoError) qWarning() << "RemoteSceneController: player error" << int(e) << err << "for" << item->mediaId; });
        auto attemptLoadVid = [item]() {
            QString path = FileManager::instance().getFilePathForId(item->fileId);
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                item->player->setSource(QUrl::fromLocalFile(path));
                if (!item->autoPlay && !item->primedFirstFrame) {
                    // Prime using shared sink since that's where frames will arrive
                    QVideoSink* sink = nullptr; if (!item->spans.isEmpty()) sink = item->spans.first().videoSink;
                    if (sink) {
                        item->primingConn = QObject::connect(sink, &QVideoSink::videoFrameChanged, item->player, [item](const QVideoFrame& f){
                            if (!f.isValid()) return; if (item->primedFirstFrame) return; item->primedFirstFrame = true; QObject::disconnect(item->primingConn); if (!item->playAuthorized && item->player) { item->player->pause(); item->player->setPosition(0); }
                        });
                    }
                    if (item->audio) item->audio->setMuted(true);
                    item->player->play();
                }
                return true;
            }
            return false;
        };
        if (!attemptLoadVid()) { for (int i=1;i<=5;++i) QTimer::singleShot(i*500, nullptr, [attemptLoadVid]() { attemptLoadVid(); }); }
    }

    // Display/play scheduling
    if (item->autoDisplay) {
        int delay = item->autoDisplayDelayMs;
        item->displayTimer = new QTimer(this); item->displayTimer->setSingleShot(true);
        connect(item->displayTimer, &QTimer::timeout, this, [this,item]() { fadeIn(item); });
        item->displayTimer->start(delay);
    }
    if (item->player && item->autoDisplay && item->autoPlay) {
        int playDelay = item->autoDisplayDelayMs + item->autoPlayDelayMs;
        item->playTimer = new QTimer(this); item->playTimer->setSingleShot(true);
        connect(item->playTimer, &QTimer::timeout, this, [item]() {
            item->playAuthorized = true; if (!item->player) return; if (item->audio) { item->audio->setMuted(item->muted); item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0)); }
            if (item->loaded) { if (item->player->position() != 0) item->player->setPosition(0); item->player->play(); }
            else {
                item->deferredStartConn = QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [item](QMediaPlayer::MediaStatus s){ if (!item->playAuthorized) return; if (s==QMediaPlayer::LoadedMedia || s==QMediaPlayer::BufferedMedia) { QObject::disconnect(item->deferredStartConn); if (item->audio) { item->audio->setMuted(item->muted); item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0)); } if (item->player->position() != 0) item->player->setPosition(0); item->player->play(); } });
            }
        });
        item->playTimer->start(playDelay);
    }
}

void RemoteSceneController::fadeIn(RemoteMediaItem* item) {
    if (!item) return;
    const int durMs = int(item->fadeInSeconds * 1000.0);
    // Multi-span path
    if (!item->spans.isEmpty()) {
        if (durMs <= 10) {
            for (auto& s : item->spans) {
                if (!s.widget) continue; s.widget->show();
                if (auto* eff = qobject_cast<QGraphicsOpacityEffect*>(s.widget->graphicsEffect())) eff->setOpacity(item->contentOpacity);
            }
            return;
        }
        for (auto& s : item->spans) {
            if (!s.widget) continue; s.widget->show();
            auto* anim = new QVariantAnimation(s.widget);
            anim->setStartValue(0.0);
            anim->setEndValue(item->contentOpacity);
            anim->setDuration(durMs);
            anim->setEasingCurve(QEasingCurve::OutCubic);
            connect(anim, &QVariantAnimation::valueChanged, s.widget, [&s](const QVariant& v){ if (auto* eff = qobject_cast<QGraphicsOpacityEffect*>(s.widget->graphicsEffect())) eff->setOpacity(v.toDouble()); });
            connect(anim, &QVariantAnimation::finished, s.widget, [anim](){ anim->deleteLater(); });
            anim->start();
        }
        return;
    }
    // Legacy single-span path
    if (!item->widget || !item->opacity) return;
    item->widget->show();
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

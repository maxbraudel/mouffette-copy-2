#include "RemoteSceneController.h"
#include "WebSocketClient.h"
#include <QJsonArray>
#include <QScreen>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPixmap>
#include <QFileInfo>
#include <QFile>
#include <QDebug>
#include <QVideoFrame>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <QWidget>
#include <QLabel>
#include <QPointer>
#include <QVector>
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
    if (!m_enabled) return;
    
    // VALIDATION PHASE: Check if all required resources are available
    const QJsonArray screens = scene.value("screens").toArray();
    const QJsonArray media = scene.value("media").toArray();
    
    // Validate scene structure
    if (screens.isEmpty()) {
        QString errorMsg = "Scene has no screen configuration";
        qWarning() << "RemoteSceneController: validation failed -" << errorMsg;
        if (m_ws) {
            m_ws->sendRemoteSceneValidationResult(senderClientId, false, errorMsg);
        }
        return;
    }
    
    if (media.isEmpty()) {
        QString errorMsg = "Scene has no media items";
        qWarning() << "RemoteSceneController: validation failed -" << errorMsg;
        if (m_ws) {
            m_ws->sendRemoteSceneValidationResult(senderClientId, false, errorMsg);
        }
        return;
    }
    
    // Validate that all media files exist
    QStringList missingFiles;
    for (const QJsonValue& val : media) {
        const QJsonObject mediaObj = val.toObject();
        const QString fileId = mediaObj.value("fileId").toString();
        if (fileId.isEmpty()) {
            qWarning() << "RemoteSceneController: media item has no fileId";
            continue;
        }
        QString path = FileManager::instance().getFilePathForId(fileId);
        if (path.isEmpty() || !QFile::exists(path)) {
            missingFiles.append(fileId);
        }
    }
    
    // If validation fails, send error feedback and abort
    if (!missingFiles.isEmpty()) {
        QString errorMsg = QString("Missing %1 media file(s): %2").arg(missingFiles.size()).arg(missingFiles.join(", "));
        qWarning() << "RemoteSceneController: validation failed -" << errorMsg;
        if (m_ws) {
            m_ws->sendRemoteSceneValidationResult(senderClientId, false, errorMsg);
        }
        return;
    }
    
    // Validation successful - send positive feedback
    qDebug() << "RemoteSceneController: validation successful, launching scene from" << senderClientId;
    if (m_ws) {
        m_ws->sendRemoteSceneValidationResult(senderClientId, true);
    }
    
    // Bump epoch and clear any previous scene
    ++m_sceneEpoch;
    clearScene();
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
    
    // Send final "launched" confirmation after scene is fully set up
    if (m_ws) {
        m_ws->sendRemoteSceneLaunched(senderClientId);
    }
}

void RemoteSceneController::onRemoteSceneStop(const QString& senderClientId) {
    Q_UNUSED(senderClientId);
    ++m_sceneEpoch;
    clearScene();
}

void RemoteSceneController::clearScene() {
    // Defensive teardown to handle rapid start/stop without use-after-free
    for (RemoteMediaItem* item : m_mediaItems) {
        if (!item) continue;
        // Stop and delete timers synchronously
        if (item->displayTimer) { item->displayTimer->stop(); QObject::disconnect(item->displayTimer, nullptr, nullptr, nullptr); delete item->displayTimer; item->displayTimer = nullptr; }
        if (item->playTimer) { item->playTimer->stop(); QObject::disconnect(item->playTimer, nullptr, nullptr, nullptr); delete item->playTimer; item->playTimer = nullptr; }
        // Disconnect one-shot connections
        QObject::disconnect(item->deferredStartConn);
        QObject::disconnect(item->primingConn);
        // Detach video sink before deleting labels/sinks
        if (item->player) {
            item->player->setVideoSink(nullptr);
            QObject::disconnect(item->player, nullptr, nullptr, nullptr);
            item->player->stop();
        }
        if (item->audio) {
            QObject::disconnect(item->audio, nullptr, nullptr, nullptr);
        }
        for (auto& s : item->spans) {
            if (s.videoSink) { QObject::disconnect(s.videoSink, nullptr, nullptr, nullptr); s.videoSink->deleteLater(); s.videoSink = nullptr; }
            if (s.videoLabel) { s.videoLabel->deleteLater(); s.videoLabel = nullptr; }
            if (s.imageLabel) { s.imageLabel->deleteLater(); s.imageLabel = nullptr; }
            if (s.widget) { s.widget->deleteLater(); s.widget = nullptr; }
        }
        if (item->widget) { item->widget->deleteLater(); item->widget = nullptr; }
        if (item->player) { item->player->deleteLater(); item->player = nullptr; }
        if (item->audio) { item->audio->deleteLater(); item->audio = nullptr; }
        delete item;
    }
    m_mediaItems.clear();
    // Close and delete screen windows
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
    item->sceneEpoch = m_sceneEpoch;
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

    // Capture the scene epoch at scheduling time to guard async callbacks
    const quint64 epoch = item->sceneEpoch;

    if (item->type == "image") {
        QLabel* lbl = new QLabel(w);
        lbl->setScaledContents(true);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setGeometry(0,0,pw,ph);
        auto attemptLoad = [this, epoch, lbl, item]() {
            if (epoch != m_sceneEpoch) return false;
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
            for (int i=1;i<=5;++i) QTimer::singleShot(i*500, w, [attemptLoad]() { attemptLoad(); });
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
        QObject::connect(videoSink, &QVideoSink::videoFrameChanged, videoLabel, [this,epoch,videoLabel,item](const QVideoFrame& frame){
            if (epoch != m_sceneEpoch) return;
            if (!frame.isValid()) return;
            const QImage img = frame.toImage();
            if (!img.isNull()) videoLabel->setPixmap(QPixmap::fromImage(img));
        });
        item->player->setVideoSink(videoSink);
        QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this,epoch,item](QMediaPlayer::MediaStatus s){
            if (epoch != m_sceneEpoch) return;
            qDebug() << "RemoteSceneController: mediaStatus" << int(s) << "for" << item->mediaId;
            if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
                item->loaded = true;
            }
        });
        QObject::connect(item->player, &QMediaPlayer::playbackStateChanged, item->player, [this,epoch,item](QMediaPlayer::PlaybackState st){
            if (epoch != m_sceneEpoch) return;
            qDebug() << "RemoteSceneController: playbackState" << int(st) << "for" << item->mediaId;
        });
        QObject::connect(item->player, &QMediaPlayer::errorOccurred, item->player, [this,epoch,item](QMediaPlayer::Error e, const QString& err){
            if (epoch != m_sceneEpoch) return;
            if (e != QMediaPlayer::NoError) qWarning() << "RemoteSceneController: player error" << int(e) << err << "for" << item->mediaId;
        });
        auto attemptLoadVid = [this, epoch, item, videoSink]() {
            if (epoch != m_sceneEpoch) return false;
            QString path = FileManager::instance().getFilePathForId(item->fileId);
            qDebug() << "RemoteSceneController: resolving video path for" << item->mediaId << "->" << path;
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                // Use file URL directly for backend compatibility
                item->player->setSource(QUrl::fromLocalFile(path));
                // Prime only for preview when autoPlay is disabled: play muted, then pause on first video frame
                if (!item->autoPlay && !item->primedFirstFrame) {
                    item->primingConn = QObject::connect(videoSink, &QVideoSink::videoFrameChanged, item->player, [this,epoch,item](const QVideoFrame& f){
                        if (epoch != m_sceneEpoch) return;
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
        if (!attemptLoadVid()) { for (int i=1;i<=5;++i) QTimer::singleShot(i*500, w, [attemptLoadVid]() { attemptLoadVid(); }); }
    }

    // Display scheduling: only if autoDisplay enabled. Otherwise remain hidden until a future command (not yet implemented).
    if (item->autoDisplay) {
        int delay = item->autoDisplayDelayMs;
        item->displayTimer = new QTimer(this);
        item->displayTimer->setSingleShot(true);
        connect(item->displayTimer, &QTimer::timeout, this, [this,epoch,item]() {
            if (epoch != m_sceneEpoch) return;
            fadeIn(item);
        });
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
        connect(item->playTimer, &QTimer::timeout, this, [this,epoch,item]() {
            if (epoch != m_sceneEpoch) return;
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
                item->deferredStartConn = QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this,epoch,item](QMediaPlayer::MediaStatus s){
                    if (epoch != m_sceneEpoch || !item->playAuthorized) return;
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
    const quint64 epoch = item->sceneEpoch;
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
        auto attemptLoad = [this, epoch, item]() {
            if (epoch != m_sceneEpoch) return false;
            QString path = FileManager::instance().getFilePathForId(item->fileId);
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                QPixmap pm; if (pm.load(path)) {
                    for (auto& s : item->spans) { if (s.imageLabel) s.imageLabel->setPixmap(pm); if (s.widget) s.widget->update(); }
                    return true;
                }
            }
            return false;
        };
        if (!attemptLoad()) {
            // Bind retries to each span widget so callbacks are dropped if the widget is destroyed
            for (auto& s : item->spans) {
                QWidget* recv = s.widget;
                for (int i=1;i<=5;++i) QTimer::singleShot(i*500, recv, [attemptLoad]() { attemptLoad(); });
            }
        }
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
            QLabel* targetLabel = s.videoLabel;
            QObject::connect(s.videoSink, &QVideoSink::videoFrameChanged, targetLabel, [this,epoch,targetLabel](const QVideoFrame& frame){
                if (epoch != m_sceneEpoch) return; if (!frame.isValid()) return; const QImage img = frame.toImage(); if (!img.isNull() && targetLabel) targetLabel->setPixmap(QPixmap::fromImage(img));
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
            // Build weak refs to labels so we don't need to access item after scheduling
            QVector<QPointer<QLabel>> labelRefs;
            labelRefs.reserve(item->spans.size());
            for (auto& s : item->spans) { if (s.videoLabel) labelRefs.push_back(QPointer<QLabel>(s.videoLabel)); }
            QObject::connect(sharedSink, &QVideoSink::videoFrameChanged, sharedSink, [this,epoch,labelRefs](const QVideoFrame& f) mutable {
                if (epoch != m_sceneEpoch) return; if (!f.isValid()) return; const QImage img = f.toImage(); if (img.isNull()) return; QPixmap pm = QPixmap::fromImage(img);
                for (auto& ref : labelRefs) { if (ref) ref->setPixmap(pm); }
            });
        }
        QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this,epoch,item](QMediaPlayer::MediaStatus s){ if (epoch != m_sceneEpoch) return; if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) item->loaded = true; });
        QObject::connect(item->player, &QMediaPlayer::errorOccurred, item->player, [this,epoch,item](QMediaPlayer::Error e, const QString& err){ if (epoch != m_sceneEpoch) return; if (e != QMediaPlayer::NoError) qWarning() << "RemoteSceneController: player error" << int(e) << err << "for" << item->mediaId; });
        auto attemptLoadVid = [this, epoch, item]() {
            if (epoch != m_sceneEpoch) return false;
            QString path = FileManager::instance().getFilePathForId(item->fileId);
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                item->player->setSource(QUrl::fromLocalFile(path));
                if (!item->autoPlay && !item->primedFirstFrame) {
                    // Prime using shared sink since that's where frames will arrive
                    QVideoSink* sink = nullptr; if (!item->spans.isEmpty()) sink = item->spans.first().videoSink;
                    if (sink) {
                        item->primingConn = QObject::connect(sink, &QVideoSink::videoFrameChanged, item->player, [this,epoch,item](const QVideoFrame& f){
                            if (epoch != m_sceneEpoch) return; if (!f.isValid()) return; if (item->primedFirstFrame) return; item->primedFirstFrame = true; QObject::disconnect(item->primingConn); if (!item->playAuthorized && item->player) { item->player->pause(); item->player->setPosition(0); }
                        });
                    }
                    if (item->audio) item->audio->setMuted(true);
                    item->player->play();
                }
                return true;
            }
            return false;
        };
        if (!attemptLoadVid()) {
            QWidget* recv = (!item->spans.isEmpty() ? item->spans.first().widget : nullptr);
            for (int i=1;i<=5;++i) QTimer::singleShot(i*500, recv, [attemptLoadVid]() { attemptLoadVid(); });
        }
    }

    // Display/play scheduling
    if (item->autoDisplay) {
        int delay = item->autoDisplayDelayMs;
        item->displayTimer = new QTimer(this); item->displayTimer->setSingleShot(true);
        connect(item->displayTimer, &QTimer::timeout, this, [this,epoch,item]() { if (epoch != m_sceneEpoch) return; fadeIn(item); });
        item->displayTimer->start(delay);
    }
    if (item->player && item->autoDisplay && item->autoPlay) {
        int playDelay = item->autoDisplayDelayMs + item->autoPlayDelayMs;
        item->playTimer = new QTimer(this); item->playTimer->setSingleShot(true);
        connect(item->playTimer, &QTimer::timeout, this, [this,epoch,item]() {
            if (epoch != m_sceneEpoch) return;
            item->playAuthorized = true; if (!item->player) return; if (item->audio) { item->audio->setMuted(item->muted); item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0)); }
            if (item->loaded) { if (item->player->position() != 0) item->player->setPosition(0); item->player->play(); }
            else {
                item->deferredStartConn = QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this,epoch,item](QMediaPlayer::MediaStatus s){ if (epoch != m_sceneEpoch || !item->playAuthorized) return; if (s==QMediaPlayer::LoadedMedia || s==QMediaPlayer::BufferedMedia) { QObject::disconnect(item->deferredStartConn); if (item->audio) { item->audio->setMuted(item->muted); item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0)); } if (item->player->position() != 0) item->player->setPosition(0); item->player->play(); } });
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

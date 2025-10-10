#include "RemoteSceneController.h"
#include "WebSocketClient.h"
#include "ToastNotificationSystem.h"
#include <QJsonArray>
#include <QScreen>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPixmap>
#include <QBuffer>
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
#include <QMetaObject>
#include <QUrl>
#include <QThread>
#include <QIODevice>
#include <QGraphicsView>
#include <QGraphicsVideoItem>
#include <QGraphicsScene>
#include <cmath>
#include "FileManager.h"
#include "MacWindowManager.h"
#include <algorithm>
#include <memory>

// GPU-direct rendering via QGraphicsVideoItem eliminates need for frame throttling

RemoteSceneController::RemoteSceneController(WebSocketClient* ws, QObject* parent)
    : QObject(parent), m_ws(ws) {
    if (m_ws) {
        connect(m_ws, &WebSocketClient::remoteSceneStartReceived, this, &RemoteSceneController::onRemoteSceneStart);
        connect(m_ws, &WebSocketClient::remoteSceneStopReceived, this, &RemoteSceneController::onRemoteSceneStop);
        connect(m_ws, &WebSocketClient::disconnected, this, &RemoteSceneController::onConnectionLost);
        connect(m_ws, &WebSocketClient::connectionError, this, &RemoteSceneController::onConnectionError);
    }
}

RemoteSceneController::~RemoteSceneController() {
	clearScene();
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
    QStringList missingFileNames;
    for (const QJsonValue& val : media) {
        const QJsonObject mediaObj = val.toObject();
        const QString fileId = mediaObj.value("fileId").toString();
        if (fileId.isEmpty()) {
            qWarning() << "RemoteSceneController: media item has no fileId";
            continue;
        }
        QString path = FileManager::instance().getFilePathForId(fileId);
        if (path.isEmpty() || !QFile::exists(path)) {
            // Get the user-friendly filename from the scene data
            QString fileName = mediaObj.value("fileName").toString();
            if (fileName.isEmpty()) {
                // Fallback: use the fileId if fileName is not available
                fileName = fileId;
            }
            missingFileNames.append(fileName);
        }
    }
    
    // If validation fails, send error feedback and abort
    if (!missingFileNames.isEmpty()) {
        QString fileList = missingFileNames.size() <= 3 
            ? missingFileNames.join(", ") 
            : QString("%1, %2, and %3 more").arg(missingFileNames[0]).arg(missingFileNames[1]).arg(missingFileNames.size() - 2);
        QString errorMsg = QString("Missing %1 file%2: %3")
            .arg(missingFileNames.size())
            .arg(missingFileNames.size() > 1 ? "s" : "")
            .arg(fileList);
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

void RemoteSceneController::onConnectionLost() {
    const bool hadScene = !m_mediaItems.isEmpty() || !m_screenWindows.isEmpty();
    ++m_sceneEpoch;
    clearScene();
    if (hadScene) {
        TOAST_WARNING("Remote scene stopped: server connection lost", 3500);
    }
}

void RemoteSceneController::onConnectionError(const QString& errorMessage) {
    Q_UNUSED(errorMessage);
    onConnectionLost();
}

void RemoteSceneController::clearScene() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this]() { clearScene(); }, Qt::QueuedConnection);
        return;
    }
    // Defensive teardown to handle rapid start/stop without use-after-free
    for (const auto& item : m_mediaItems) {
        teardownMediaItem(item);
    }
    m_mediaItems.clear();
    // Close and delete screen windows
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        if (it.value().window) it.value().window->close();
        if (it.value().window) it.value().window->deleteLater();
    }
    m_screenWindows.clear();
}

void RemoteSceneController::teardownMediaItem(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;

    auto stopAndDeleteTimer = [](QTimer*& timer) {
        if (!timer) return;
        timer->stop();
        QObject::disconnect(timer, nullptr, nullptr, nullptr);
        timer->deleteLater();
        timer = nullptr;
    };

    stopAndDeleteTimer(item->displayTimer);
    stopAndDeleteTimer(item->playTimer);

    QObject::disconnect(item->deferredStartConn);
    QObject::disconnect(item->primingConn);

    if (item->player) {
        QMediaPlayer* player = item->player;
        QObject::disconnect(player, nullptr, nullptr, nullptr);
        if (player->playbackState() != QMediaPlayer::StoppedState) {
            player->stop();
        }
        player->setVideoSink(nullptr);
        player->setSource(QUrl());
        if (item->memoryBuffer) {
            item->memoryBuffer->close();
            item->memoryBuffer->deleteLater();
            item->memoryBuffer = nullptr;
        }
    }

    if (item->audio) {
        QObject::disconnect(item->audio, nullptr, nullptr, nullptr);
        item->audio->setMuted(true);
    }

    if (item->videoItemSingle) {
        if (item->videoItemSingle->scene()) {
            item->videoItemSingle->scene()->removeItem(item->videoItemSingle);
        }
        delete item->videoItemSingle;
        item->videoItemSingle = nullptr;
    }
    if (item->imageLabelSingle) {
        QGraphicsPixmapItem* pixmapItem = reinterpret_cast<QGraphicsPixmapItem*>(item->imageLabelSingle);
        if (pixmapItem && pixmapItem->scene()) {
            pixmapItem->scene()->removeItem(pixmapItem);
        }
        delete pixmapItem;
        item->imageLabelSingle = nullptr;
    }
    if (item->sceneSingle) {
        delete item->sceneSingle;
        item->sceneSingle = nullptr;
    }
    if (item->graphicsViewSingle) {
        delete item->graphicsViewSingle;
        item->graphicsViewSingle = nullptr;
    }

    for (auto& span : item->spans) {
        if (span.videoItem) {
            if (span.videoItem->scene()) {
                span.videoItem->scene()->removeItem(span.videoItem);
            }
            delete span.videoItem;
            span.videoItem = nullptr;
        }
        if (span.imageLabel) {
            QGraphicsPixmapItem* pixmapItem = reinterpret_cast<QGraphicsPixmapItem*>(span.imageLabel);
            if (pixmapItem && pixmapItem->scene()) {
                pixmapItem->scene()->removeItem(pixmapItem);
            }
            delete pixmapItem;
            span.imageLabel = nullptr;
        }
        if (span.scene) {
            delete span.scene;
            span.scene = nullptr;
        }
        if (span.graphicsView) {
            delete span.graphicsView;
            span.graphicsView = nullptr;
        }
        if (span.widget) {
            span.widget->hide();
            span.widget->deleteLater();
            span.widget = nullptr;
        }
    }
    item->spans.clear();

    if (item->widget) {
        item->widget->hide();
        item->widget->deleteLater();
        item->widget = nullptr;
    }
    if (item->opacity) {
        item->opacity->deleteLater();
        item->opacity = nullptr;
    }

    if (item->player) {
        item->player->deleteLater();
        item->player = nullptr;
    }
    if (item->audio) {
        item->audio->deleteLater();
        item->audio = nullptr;
    }

    item->memoryBytes.reset();
    item->usingMemoryBuffer = false;
    item->loaded = false;
    item->primedFirstFrame = false;
    item->playAuthorized = false;
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
    
    // Create QGraphicsView for GPU-accelerated rendering
    QGraphicsView* view = new QGraphicsView(win);
    QGraphicsScene* scene = new QGraphicsScene(view);
    scene->setSceneRect(0, 0, w, h);
    view->setScene(scene);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setFrameStyle(QFrame::NoFrame);
    view->setAttribute(Qt::WA_TranslucentBackground, true);
    view->setStyleSheet("background: transparent;");
    view->setRenderHint(QPainter::Antialiasing, true);
    view->setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (view->viewport()) {
        view->viewport()->setAutoFillBackground(false);
        view->viewport()->setAttribute(Qt::WA_TranslucentBackground, true);
    }
    
    auto* layout = new QHBoxLayout(win);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(0);
    layout->addWidget(view);
    
    ScreenWindow sw; 
    sw.window = win; 
    sw.graphicsView = view;
    sw.scene = scene;
    sw.x=x; sw.y=y; sw.w=w; sw.h=h; 
    m_screenWindows.insert(screenId, sw);

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
    auto item = std::make_shared<RemoteMediaItem>();
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
        item->repeatEnabled = m.value("repeatEnabled").toBool(false);
        item->repeatCount = std::max(0, m.value("repeatCount").toInt(0));
        item->repeatRemaining = 0;
        item->repeatActive = false;
        if (item->type == "video") {
            item->muted = m.value("muted").toBool(false);
            item->volume = m.value("volume").toDouble(1.0);
            FileManager::instance().preloadFileIntoMemory(item->fileId);
        }
        m_mediaItems.append(item);
        scheduleMedia(item);
    }
}

void RemoteSceneController::scheduleMedia(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!item->spans.isEmpty()) { scheduleMediaMulti(item); return; }
    scheduleMediaLegacy(item);
}

void RemoteSceneController::scheduleMediaLegacy(const std::shared_ptr<RemoteMediaItem>& item) {
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

    std::weak_ptr<RemoteMediaItem> weakItem = item;

    if (item->type == "image") {
        auto winIt = m_screenWindows.find(item->screenId);
        if (winIt == m_screenWindows.end() || !winIt.value().scene) {
            qWarning() << "RemoteSceneController: no scene for screenId" << item->screenId;
            return;
        }
        
        QGraphicsScene* scene = winIt.value().scene;
        
        // Create QGraphicsPixmapItem for GPU-accelerated image display
        QGraphicsPixmapItem* pixmapItem = new QGraphicsPixmapItem();
        pixmapItem->setPos(px, py);
        pixmapItem->setOpacity(0.0); // Start hidden for fade-in
        pixmapItem->setTransformationMode(Qt::SmoothTransformation);
        scene->addItem(pixmapItem);
        item->imageLabelSingle = reinterpret_cast<QLabel*>(pixmapItem); // Store as label pointer for compatibility
        
        auto attemptLoad = [this, epoch, pixmapItem, pw, ph, weakItem]() {
            auto item = weakItem.lock();
            if (!item) return false;
            if (epoch != m_sceneEpoch) return false;
            QString path = FileManager::instance().getFilePathForId(item->fileId);
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                QPixmap pm;
                if (pm.load(path)) {
                    // Scale pixmap to target size
                    QPixmap scaled = pm.scaled(QSize(pw, ph), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    pixmapItem->setPixmap(scaled);
                    return true;
                }
            }
            qWarning() << "RemoteSceneController: image path not available yet for" << item->mediaId << ", will retry";
            return false;
        };
        if (!attemptLoad()) {
            // Retry a few times (e.g., if upload arrives slightly after scene start)
            for (int i=1;i<=5;++i) QTimer::singleShot(i*500, w, [attemptLoad]() { attemptLoad(); });
        }
        
        // Hide the widget container since image renders directly in scene
        w->hide();
    } else if (item->type == "video") {
        // Use GPU-direct rendering via QGraphicsVideoItem
        auto winIt = m_screenWindows.find(item->screenId);
        if (winIt == m_screenWindows.end() || !winIt.value().scene) {
            qWarning() << "RemoteSceneController: no scene for screenId" << item->screenId;
            return;
        }
        
        QGraphicsScene* scene = winIt.value().scene;
        
        // Create QGraphicsVideoItem for GPU-accelerated playback
        QGraphicsVideoItem* videoItem = new QGraphicsVideoItem();
        videoItem->setSize(QSizeF(pw, ph));
        videoItem->setPos(px, py);
        videoItem->setOpacity(0.0); // Start hidden for fade-in
        scene->addItem(videoItem);
        item->videoItemSingle = videoItem;
        
        // Create media player and connect to video item
        item->player = new QMediaPlayer(w);
        item->audio = new QAudioOutput(w);
        item->audio->setMuted(item->muted);
        item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0));
        item->player->setAudioOutput(item->audio);
        item->player->setVideoOutput(videoItem); // Direct GPU rendering!
        QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this,epoch,weakItem](QMediaPlayer::MediaStatus s){
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            qDebug() << "RemoteSceneController: mediaStatus" << int(s) << "for" << item->mediaId;
            if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
                item->loaded = true;
            }
        });
        QObject::connect(item->player, &QMediaPlayer::playbackStateChanged, item->player, [this,epoch,weakItem](QMediaPlayer::PlaybackState st){
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            qDebug() << "RemoteSceneController: playbackState" << int(st) << "for" << item->mediaId;
        });
        QObject::connect(item->player, &QMediaPlayer::errorOccurred, item->player, [this,epoch,weakItem](QMediaPlayer::Error e, const QString& err){
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            if (e != QMediaPlayer::NoError) qWarning() << "RemoteSceneController: player error" << int(e) << err << "for" << item->mediaId;
        });
        auto attemptLoadVid = [this, epoch, weakItem]() {
            auto item = weakItem.lock();
            if (!item) return false;
            if (epoch != m_sceneEpoch) return false;
            QString path = FileManager::instance().getFilePathForId(item->fileId);
            qDebug() << "RemoteSceneController: resolving video path for" << item->mediaId << "->" << path;
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                auto bytes = FileManager::instance().getFileBytes(item->fileId);
                if (!bytes.isNull() && !bytes->isEmpty()) {
                    item->memoryBytes = bytes;
                    if (item->memoryBuffer) {
                        item->memoryBuffer->close();
                        item->memoryBuffer->deleteLater();
                    }
                    item->memoryBuffer = new QBuffer(item->memoryBytes.data(), item->player);
                    if (!item->memoryBuffer->isOpen()) {
                        item->memoryBuffer->open(QIODevice::ReadOnly);
                    }
                    item->player->setSourceDevice(item->memoryBuffer, !path.isEmpty() ? QUrl::fromLocalFile(path) : QUrl());
                    item->usingMemoryBuffer = true;
                } else {
                    item->player->setSource(QUrl::fromLocalFile(path));
                    item->usingMemoryBuffer = false;
                }

                const int loops = (item->repeatEnabled && item->repeatCount > 0)
                                      ? (item->repeatCount + 1)
                                      : 1;
                item->player->setLoops(loops);

                // Prime the first frame if not already done
                if (!item->primedFirstFrame) {
                    item->primingConn = QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this,epoch,weakItem](QMediaPlayer::MediaStatus status){
                        auto item = weakItem.lock();
                        if (!item) return;
                        if (epoch != m_sceneEpoch) return;
                        if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) {
                            if (!item->primedFirstFrame) {
                                item->primedFirstFrame = true;
                                QObject::disconnect(item->primingConn);
                                if (!item->playAuthorized && item->player) {
                                    item->player->pause();
                                    item->player->setPosition(0);
                                }
                            }
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
            for (int i=1;i<=5;++i) QTimer::singleShot(i*500, w, [attemptLoadVid]() { attemptLoadVid(); }); 
        }
        
        // Hide the widget container since video renders directly in scene
        w->hide();
    }

    // Display scheduling: only if autoDisplay enabled. Otherwise remain hidden until a future command (not yet implemented).
    if (item->autoDisplay) {
        int delay = item->autoDisplayDelayMs;
        item->displayTimer = new QTimer(this);
        item->displayTimer->setSingleShot(true);
        connect(item->displayTimer, &QTimer::timeout, this, [this,epoch,weakItem]() {
            auto item = weakItem.lock();
            if (!item) return;
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
        connect(item->playTimer, &QTimer::timeout, this, [this,epoch,weakItem]() {
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            item->playAuthorized = true;
            if (!item->player) return;
            item->repeatActive = false;
            item->repeatRemaining = 0;
            // Apply final audio state from host now
            if (item->audio) { item->audio->setMuted(item->muted); item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0)); }
            if (item->loaded) {
                // If we primed, position might already be 0 and paused; just play
                if (item->player->position() != 0) item->player->setPosition(0);
                item->player->play();
            }
            else {
                // Defer until loaded to avoid starting before buffer is ready
                item->deferredStartConn = QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this,epoch,weakItem](QMediaPlayer::MediaStatus s){
                    auto item = weakItem.lock();
                    if (!item) return;
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

void RemoteSceneController::scheduleMediaMulti(const std::shared_ptr<RemoteMediaItem>& item) {
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
        
        // Get the scene for this screen
        QGraphicsScene* scene = winIt.value().scene;
        if (!scene) continue;
        
        if (item->type == "image") {
            // Create QGraphicsPixmapItem for GPU-accelerated image display
            QGraphicsPixmapItem* pixmapItem = new QGraphicsPixmapItem();
            pixmapItem->setPos(px, py);
            pixmapItem->setOpacity(0.0);
            pixmapItem->setTransformationMode(Qt::SmoothTransformation);
            scene->addItem(pixmapItem);
            s.imageLabel = reinterpret_cast<QLabel*>(pixmapItem);
        } else if (item->type == "video") {
            // Create QGraphicsVideoItem for GPU-accelerated video
            QGraphicsVideoItem* videoItem = new QGraphicsVideoItem();
            videoItem->setSize(QSizeF(pw, ph));
            videoItem->setPos(px, py);
            videoItem->setOpacity(0.0);
            scene->addItem(videoItem);
            s.videoItem = videoItem;
        }
        w->hide(); // Hide widget container since items render in scene
        w->raise();
    }

    // Content loading
    std::weak_ptr<RemoteMediaItem> weakItem = item;

    if (item->type == "image") {
        auto attemptLoad = [this, epoch, weakItem]() {
            auto item = weakItem.lock();
            if (!item) return false;
            if (epoch != m_sceneEpoch) return false;
            QString path = FileManager::instance().getFilePathForId(item->fileId);
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                QPixmap pm; 
                if (pm.load(path)) {
                    for (auto& s : item->spans) { 
                        if (s.imageLabel) {
                            QGraphicsPixmapItem* pixmapItem = reinterpret_cast<QGraphicsPixmapItem*>(s.imageLabel);
                            // Scale to span size
                            int pw = int(s.nw * (s.widget ? s.widget->parentWidget()->width() : 100));
                            int ph = int(s.nh * (s.widget ? s.widget->parentWidget()->height() : 100));
                            QPixmap scaled = pm.scaled(QSize(pw, ph), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                            pixmapItem->setPixmap(scaled);
                        }
                    }
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
        // GPU-direct rendering with QGraphicsVideoItem
        // Use first span's video item as the primary output
        QWidget* parentForAv = nullptr; 
        if (!item->spans.isEmpty()) parentForAv = item->spans.first().widget;
        
        item->player = new QMediaPlayer(parentForAv);
        item->audio = new QAudioOutput(parentForAv);
        item->audio->setMuted(item->muted); 
        item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0));
        item->player->setAudioOutput(item->audio);
        
        // Set video output to first span's video item (Qt only supports one output)
        if (!item->spans.isEmpty() && item->spans.first().videoItem) {
            item->player->setVideoOutput(item->spans.first().videoItem);
        }
        QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this,epoch,weakItem](QMediaPlayer::MediaStatus s){
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
                item->loaded = true;
            }
        });
        QObject::connect(item->player, &QMediaPlayer::errorOccurred, item->player, [this,epoch,weakItem](QMediaPlayer::Error e, const QString& err){ auto item = weakItem.lock(); if (!item) return; if (epoch != m_sceneEpoch) return; if (e != QMediaPlayer::NoError) qWarning() << "RemoteSceneController: player error" << int(e) << err << "for" << item->mediaId; });
        auto attemptLoadVid = [this, epoch, weakItem]() {
            auto item = weakItem.lock();
            if (!item) return false;
            if (epoch != m_sceneEpoch) return false;
            QString path = FileManager::instance().getFilePathForId(item->fileId);
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                auto bytes = FileManager::instance().getFileBytes(item->fileId);
                if (!bytes.isNull() && !bytes->isEmpty()) {
                    item->memoryBytes = bytes;
                    if (item->memoryBuffer) {
                        item->memoryBuffer->close();
                        item->memoryBuffer->deleteLater();
                    }
                    item->memoryBuffer = new QBuffer(item->memoryBytes.data(), item->player);
                    if (!item->memoryBuffer->isOpen()) {
                        item->memoryBuffer->open(QIODevice::ReadOnly);
                    }
                    item->player->setSourceDevice(item->memoryBuffer, !path.isEmpty() ? QUrl::fromLocalFile(path) : QUrl());
                    item->usingMemoryBuffer = true;
                } else {
                    item->player->setSource(QUrl::fromLocalFile(path));
                    item->usingMemoryBuffer = false;
                }

                const int loops = (item->repeatEnabled && item->repeatCount > 0)
                                      ? (item->repeatCount + 1)
                                      : 1;
                item->player->setLoops(loops);

                // Prime the first frame if not already done
                if (!item->primedFirstFrame) {
                    item->primingConn = QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this,epoch,weakItem](QMediaPlayer::MediaStatus status){
                        auto item = weakItem.lock();
                        if (!item) return;
                        if (epoch != m_sceneEpoch) return;
                        if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) {
                            if (!item->primedFirstFrame) {
                                item->primedFirstFrame = true;
                                QObject::disconnect(item->primingConn);
                                if (!item->playAuthorized && item->player) {
                                    item->player->pause();
                                    item->player->setPosition(0);
                                }
                            }
                        }
                    });
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
        connect(item->displayTimer, &QTimer::timeout, this, [this,epoch,weakItem]() {
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            fadeIn(item);
        });
        item->displayTimer->start(delay);
    }
    if (item->player && item->autoDisplay && item->autoPlay) {
        int playDelay = item->autoDisplayDelayMs + item->autoPlayDelayMs;
        item->playTimer = new QTimer(this); item->playTimer->setSingleShot(true);
        connect(item->playTimer, &QTimer::timeout, this, [this,epoch,weakItem]() {
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            item->playAuthorized = true; if (!item->player) return;
            item->repeatActive = false;
            item->repeatRemaining = 0;
            if (item->audio) { item->audio->setMuted(item->muted); item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0)); }
            if (item->loaded) { if (item->player->position() != 0) item->player->setPosition(0); item->player->play(); }
            else {
                item->deferredStartConn = QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this,epoch,weakItem](QMediaPlayer::MediaStatus s){ auto item = weakItem.lock(); if (!item) return; if (epoch != m_sceneEpoch || !item->playAuthorized) return; if (s==QMediaPlayer::LoadedMedia || s==QMediaPlayer::BufferedMedia) { QObject::disconnect(item->deferredStartConn); if (item->audio) { item->audio->setMuted(item->muted); item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0)); } if (item->player->position() != 0) item->player->setPosition(0); item->player->play(); } });
            }
        });
        item->playTimer->start(playDelay);
    }
}

void RemoteSceneController::fadeIn(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    const int durMs = int(item->fadeInSeconds * 1000.0);
    // Multi-span path - now uses QGraphicsItem opacity
    if (!item->spans.isEmpty()) {
        if (durMs <= 10) {
            for (auto& s : item->spans) {
                QGraphicsItem* graphicsItem = nullptr;
                if (s.videoItem) graphicsItem = s.videoItem;
                else if (s.imageLabel) graphicsItem = reinterpret_cast<QGraphicsPixmapItem*>(s.imageLabel);
                if (graphicsItem) graphicsItem->setOpacity(item->contentOpacity);
            }
            return;
        }
        for (auto& s : item->spans) {
            QGraphicsItem* graphicsItem = nullptr;
            if (s.videoItem) graphicsItem = s.videoItem;
            else if (s.imageLabel) graphicsItem = reinterpret_cast<QGraphicsPixmapItem*>(s.imageLabel);
            if (!graphicsItem) continue;
            
            auto* anim = new QVariantAnimation(this);
            anim->setStartValue(0.0);
            anim->setEndValue(item->contentOpacity);
            anim->setDuration(durMs);
            anim->setEasingCurve(QEasingCurve::OutCubic);
            connect(anim, &QVariantAnimation::valueChanged, this, [graphicsItem](const QVariant& v){ 
                if (graphicsItem) graphicsItem->setOpacity(v.toDouble()); 
            });
            connect(anim, &QVariantAnimation::finished, anim, [anim](){ anim->deleteLater(); });
            anim->start();
        }
        return;
    }
    // Legacy single-span path - now uses QGraphicsItem opacity
    QGraphicsItem* graphicsItem = nullptr;
    if (item->videoItemSingle) {
        graphicsItem = item->videoItemSingle;
    } else if (item->imageLabelSingle) {
        graphicsItem = reinterpret_cast<QGraphicsPixmapItem*>(item->imageLabelSingle);
    }
    
    if (!graphicsItem) return;
    
    if (durMs <= 10) { 
        graphicsItem->setOpacity(item->contentOpacity); 
        return; 
    }
    
    auto* anim = new QVariantAnimation(this);
    anim->setStartValue(0.0);
    anim->setEndValue(item->contentOpacity);
    anim->setDuration(durMs);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim, &QVariantAnimation::valueChanged, this, [graphicsItem](const QVariant& v){ 
        if (graphicsItem) graphicsItem->setOpacity(v.toDouble()); 
    });
    connect(anim, &QVariantAnimation::finished, anim, [anim](){ anim->deleteLater(); });
    anim->start();
}

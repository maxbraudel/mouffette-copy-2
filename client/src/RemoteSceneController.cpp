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
#include <QVariant>
#include <cmath>
#include "FileManager.h"
#include "MacWindowManager.h"
#include <algorithm>
#include <memory>


namespace {
constexpr qint64 kStartPositionToleranceMs = 120;

qint64 frameTimestampMs(const QVideoFrame& frame) {
    if (!frame.isValid()) {
        return -1;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const qint64 startTimeUs = frame.startTime();
    if (startTimeUs >= 0) {
        return startTimeUs / 1000;
    }
#else
    const qint64 startTimeUs = frame.startTime();
    if (startTimeUs >= 0) {
        return startTimeUs / 1000;
    }
    const QVariant metaTimestamp = frame.metaData(QVideoFrame::StartTime);
    if (metaTimestamp.isValid()) {
        bool ok = false;
        const qint64 micro = metaTimestamp.toLongLong(&ok);
        if (ok) {
            return micro / 1000;
        }
    }
#endif
    return -1;
}
} // namespace

RemoteSceneController::RemoteSceneController(WebSocketClient* ws, QObject* parent)
    : QObject(parent)
    , m_ws(ws) {
    if (m_ws) {
        connect(m_ws, &WebSocketClient::remoteSceneStartReceived, this, &RemoteSceneController::onRemoteSceneStart);
        connect(m_ws, &WebSocketClient::remoteSceneStopReceived, this, &RemoteSceneController::onRemoteSceneStop);
    }
}

RemoteSceneController::~RemoteSceneController() {
	clearScene();
}

void RemoteSceneController::resetSceneSynchronization() {
    if (m_sceneReadyTimeout) {
        m_sceneReadyTimeout->stop();
        QObject::disconnect(m_sceneReadyTimeout, nullptr, this, nullptr);
        m_sceneReadyTimeout->deleteLater();
        m_sceneReadyTimeout = nullptr;
    }
    m_pendingSenderClientId.clear();
    m_totalMediaToPrime = 0;
    m_mediaReadyCount = 0;
    m_sceneActivationRequested = false;
    m_sceneActivated = false;
    m_pendingActivationEpoch = 0;
}

void RemoteSceneController::onRemoteSceneStart(const QString& senderClientId, const QJsonObject& scene) {
    if (!m_enabled) return;

    const QJsonArray screens = scene.value("screens").toArray();
    const QJsonArray media = scene.value("media").toArray();

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
            QString fileName = mediaObj.value("fileName").toString();
            if (fileName.isEmpty()) {
                fileName = fileId;
            }
            missingFileNames.append(fileName);
        }
    }

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

    qDebug() << "RemoteSceneController: validation successful, preparing scene from" << senderClientId;

    ++m_sceneEpoch;
    clearScene();

    m_pendingSenderClientId = senderClientId;
    m_totalMediaToPrime = media.size();
    m_mediaReadyCount = 0;
    m_sceneActivationRequested = false;
    m_sceneActivated = false;

    if (!m_sceneReadyTimeout) {
        m_sceneReadyTimeout = new QTimer(this);
        m_sceneReadyTimeout->setSingleShot(true);
        connect(m_sceneReadyTimeout, &QTimer::timeout, this, &RemoteSceneController::handleSceneReadyTimeout);
    }
    m_sceneReadyTimeout->start(11000);

    buildWindows(screens);
    buildMedia(media);

    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        if (it.value().window) it.value().window->show();
#ifdef Q_OS_MAC
        if (it.value().window) {
            QTimer::singleShot(0, it.value().window, [w = it.value().window]() {
                MacWindowManager::setWindowAsGlobalOverlay(w, /*clickThrough*/ true);
            });
        }
#endif
    }

    startSceneActivationIfReady();
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

    resetSceneSynchronization();
    
    // CRITICAL: Stop all fade animations first to prevent accessing deleted graphics items
    // Find all QVariantAnimation children and stop them immediately
    QList<QVariantAnimation*> animations = findChildren<QVariantAnimation*>();
    for (QVariantAnimation* anim : animations) {
        if (anim) {
            anim->stop();
            QObject::disconnect(anim, nullptr, nullptr, nullptr);
            anim->deleteLater();
        }
    }
    
    // Defensive teardown to handle rapid start/stop without use-after-free
    for (const auto& item : m_mediaItems) {
        teardownMediaItem(item);
    }
    m_mediaItems.clear();
    
    // Close and schedule deletion of screen windows
    // We MUST use deleteLater for all QObjects to avoid Qt event system crashes
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        QWidget* window = it.value().window;
        if (!window) continue;
        
        // Disconnect all signals to avoid callbacks during destruction
        QObject::disconnect(window, nullptr, nullptr, nullptr);
        
        // Hide window first to prevent visual artifacts
        window->hide();
        
        // Clear scene items and disconnect before deletion
        if (it.value().scene) {
            QObject::disconnect(it.value().scene, nullptr, nullptr, nullptr);
            it.value().scene->clear();
            it.value().scene->deleteLater();
        }
        if (it.value().graphicsView) {
            QObject::disconnect(it.value().graphicsView, nullptr, nullptr, nullptr);
            it.value().graphicsView->setScene(nullptr);
            it.value().graphicsView->deleteLater();
        }
        
        // Schedule window deletion (must use deleteLater for QObjects)
        window->deleteLater();
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
    stopAndDeleteTimer(item->pauseTimer);
    stopAndDeleteTimer(item->hideTimer);

    QObject::disconnect(item->deferredStartConn);
    QObject::disconnect(item->primingConn);
    QObject::disconnect(item->mirrorConn);
    item->pausedAtEnd = false;

    if (item->primingSink) {
        QObject::disconnect(item->primingSink, nullptr, nullptr, nullptr);
        item->primingSink->deleteLater();
        item->primingSink = nullptr;
    }

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
        delete item->imageLabelSingle;
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
    item->primedFrame = QVideoFrame();
    item->playAuthorized = false;
    item->hiding = false;
    item->readyNotified = false;
    item->fadeInPending = false;
    item->pendingDisplayDelayMs = -1;
    item->pendingPlayDelayMs = -1;
    item->pendingPauseDelayMs = -1;
    item->startPositionMs = 0;
    item->hasStartPosition = false;
    item->awaitingStartFrame = false;
    item->videoOutputsAttached = false;
}

void RemoteSceneController::markItemReady(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (item->readyNotified) return;
    item->readyNotified = true;
    ++m_mediaReadyCount;
    qDebug() << "RemoteSceneController: media primed" << item->mediaId << "(" << m_mediaReadyCount << "/" << m_totalMediaToPrime << ")";
    startSceneActivationIfReady();
}

void RemoteSceneController::evaluateItemReadiness(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (item->readyNotified) return;
    bool ready = false;
    if (item->type == "image") {
        ready = item->loaded;
    } else if (item->type == "video") {
        ready = item->loaded && item->primedFirstFrame;
    } else {
        ready = true;
    }
    if (ready) {
        markItemReady(item);
        startPendingPauseTimerIfEligible(item);
    }
}

void RemoteSceneController::startSceneActivationIfReady() {
    if (m_sceneActivated || m_sceneActivationRequested) return;
    const quint64 epoch = m_sceneEpoch;
    m_pendingActivationEpoch = epoch;
    if (m_totalMediaToPrime <= 0) {
        m_sceneActivationRequested = true;
        QMetaObject::invokeMethod(this, [this, epoch]() {
            if (epoch != m_pendingActivationEpoch) return;
            activateScene();
        }, Qt::QueuedConnection);
        return;
    }
    if (m_mediaReadyCount < m_totalMediaToPrime) return;
    m_sceneActivationRequested = true;
    QMetaObject::invokeMethod(this, [this, epoch]() {
        if (epoch != m_pendingActivationEpoch) return;
        activateScene();
    }, Qt::QueuedConnection);
}

void RemoteSceneController::startDeferredTimers() {
    for (const auto& item : m_mediaItems) {
        if (!item) continue;
        if (item->displayTimer && item->pendingDisplayDelayMs >= 0) {
            item->displayTimer->start(item->pendingDisplayDelayMs);
            item->pendingDisplayDelayMs = -1;
        }
        if (item->playTimer && item->pendingPlayDelayMs >= 0) {
            if (item->pendingPlayDelayMs == 0) {
                if (item->playTimer->isActive()) item->playTimer->stop();
                triggerAutoPlayNow(item, item->sceneEpoch);
            } else {
                item->playTimer->start(item->pendingPlayDelayMs);
            }
            item->pendingPlayDelayMs = -1;
        }
        startPendingPauseTimerIfEligible(item);
        if (item->fadeInPending && item->displayReady && !item->displayStarted) {
            fadeIn(item);
        }
    }
}

void RemoteSceneController::startPendingPauseTimerIfEligible(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!item->pauseTimer) return;
    if (item->pendingPauseDelayMs < 0) return;
    if (!m_sceneActivated) return;
    if (item->awaitingStartFrame) return;

    item->pauseTimer->start(item->pendingPauseDelayMs);
    item->pendingPauseDelayMs = -1;
}

void RemoteSceneController::triggerAutoPlayNow(const std::shared_ptr<RemoteMediaItem>& item, quint64 epoch) {
    if (!item) return;
    if (epoch != m_sceneEpoch) return;
    if (!item->player) return;

    item->playAuthorized = true;
    item->repeatActive = false;
    if (item->audio) {
        item->audio->setMuted(item->muted);
        item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0));
    }
    item->pausedAtEnd = false;
    item->repeatRemaining = (item->repeatEnabled && item->repeatCount > 0) ? item->repeatCount : 0;

    if (item->loaded) {
        const qint64 startPos = item->hasStartPosition ? effectiveStartPosition(item) : 0;
        if (item->player->position() != startPos) {
            item->player->setPosition(startPos);
        }
        ensureVideoOutputsAttached(item);
        if (item->primedFrame.isValid()) {
            applyPrimedFrameToSinks(item);
        }
        item->player->play();
        return;
    }

    QObject::disconnect(item->deferredStartConn);
    std::weak_ptr<RemoteMediaItem> weakItem = item;
    item->deferredStartConn = QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this, epoch, weakItem](QMediaPlayer::MediaStatus s) {
        auto item = weakItem.lock();
        if (!item) return;
        if (epoch != m_sceneEpoch || !item->playAuthorized) return;
        if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
            QObject::disconnect(item->deferredStartConn);
            if (item->audio) {
                item->audio->setMuted(item->muted);
                item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0));
            }
            if (item->player) {
                const qint64 startPos = item->hasStartPosition ? effectiveStartPosition(item) : 0;
                if (item->player->position() != startPos) {
                    item->player->setPosition(startPos);
                }
                ensureVideoOutputsAttached(item);
                if (item->primedFrame.isValid()) {
                    applyPrimedFrameToSinks(item);
                }
                item->player->play();
            }
            item->pausedAtEnd = false;
            item->repeatRemaining = (item->repeatEnabled && item->repeatCount > 0) ? item->repeatCount : 0;
        }
    });
}

void RemoteSceneController::applyPrimedFrameToSinks(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!item->primedFrame.isValid()) return;

    auto pushFrame = [](QGraphicsVideoItem* videoItem, const QVideoFrame& frame) {
        if (!videoItem) return;
        if (QVideoSink* sink = videoItem->videoSink()) {
            sink->setVideoFrame(frame);
        }
    };

    pushFrame(item->videoItemSingle, item->primedFrame);
    for (auto& span : item->spans) {
        pushFrame(span.videoItem, item->primedFrame);
    }
}

void RemoteSceneController::clearVideoSinks(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;

    auto clearFrame = [](QGraphicsVideoItem* videoItem) {
        if (!videoItem) return;
        if (QVideoSink* sink = videoItem->videoSink()) {
            sink->setVideoFrame(QVideoFrame());
        }
    };

    clearFrame(item->videoItemSingle);
    for (auto& span : item->spans) {
        clearFrame(span.videoItem);
    }
}

void RemoteSceneController::ensureVideoOutputsAttached(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (item->videoOutputsAttached) {
        applyPrimedFrameToSinks(item);
        return;
    }
    if (!item->player) return;

    if (item->spans.isEmpty()) {
        if (item->videoItemSingle) {
            item->player->setVideoOutput(item->videoItemSingle);
            item->videoOutputsAttached = true;
            applyPrimedFrameToSinks(item);
        }
        return;
    }

    QGraphicsVideoItem* primaryVideoItem = !item->spans.isEmpty() ? item->spans.first().videoItem : nullptr;
    if (!primaryVideoItem) return;

    item->player->setVideoOutput(primaryVideoItem);
    QObject::disconnect(item->mirrorConn);
    if (QVideoSink* primarySink = primaryVideoItem->videoSink()) {
        std::weak_ptr<RemoteMediaItem> weakItem = item;
        const quint64 epoch = item->sceneEpoch;
        item->mirrorConn = QObject::connect(primarySink, &QVideoSink::videoFrameChanged, item->player, [this, epoch, weakItem](const QVideoFrame& frame) {
            if (!frame.isValid()) return;
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            for (int idx = 1; idx < item->spans.size(); ++idx) {
                auto& span = item->spans[idx];
                if (!span.videoItem) continue;
                if (QVideoSink* spanSink = span.videoItem->videoSink()) {
                    spanSink->setVideoFrame(frame);
                }
            }
        });
    }

    item->videoOutputsAttached = true;
    applyPrimedFrameToSinks(item);
}

void RemoteSceneController::activateScene() {
    if (m_sceneActivated) return;
    m_sceneActivated = true;
    m_sceneActivationRequested = false;
    m_pendingActivationEpoch = 0;

    if (m_sceneReadyTimeout) {
        m_sceneReadyTimeout->stop();
    }

    startDeferredTimers();

    const QString sender = m_pendingSenderClientId;
    if (m_ws && !sender.isEmpty()) {
        m_ws->sendRemoteSceneValidationResult(sender, true);
        m_ws->sendRemoteSceneLaunched(sender);
    }
    m_pendingSenderClientId.clear();
}

void RemoteSceneController::handleSceneReadyTimeout() {
    const QString sender = m_pendingSenderClientId;
    qWarning() << "RemoteSceneController: timed out waiting for remote media to load" << sender;
    if (m_ws && !sender.isEmpty()) {
        m_ws->sendRemoteSceneValidationResult(sender, false, "Timed out waiting for remote media to load");
    }
    ++m_sceneEpoch;
    clearScene();
}

qint64 RemoteSceneController::effectiveStartPosition(const std::shared_ptr<RemoteMediaItem>& item) const {
    if (!item) return 0;
    if (!item->hasStartPosition) return 0;
    qint64 target = item->startPositionMs;
    if (target < 0) target = 0;
    if (item->player) {
        const qint64 dur = item->player->duration();
        if (dur > 0 && target >= dur) {
            target = std::max<qint64>(qint64(0), dur - 1);
        }
    }
    return target;
}

void RemoteSceneController::seekToConfiguredStart(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!item->player) return;
    const qint64 target = item->hasStartPosition ? effectiveStartPosition(item) : 0;
    const qint64 current = item->player->position();
    const bool needsSeek = qAbs(current - target) > kStartPositionToleranceMs;
    if (needsSeek) {
        item->awaitingStartFrame = item->hasStartPosition && target > 0;
        item->player->setPosition(target);
    } else {
        item->awaitingStartFrame = false;
        if (current != target) {
            item->player->setPosition(target);
        }
    }
    if (!item->awaitingStartFrame) {
        startPendingPauseTimerIfEligible(item);
    }
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
        item->autoPause = m.value("autoPause").toBool(false);
        item->autoPauseDelayMs = m.value("autoPauseDelayMs").toInt(0);
        item->autoHide = m.value("autoHide").toBool(false);
        item->autoHideDelayMs = m.value("autoHideDelayMs").toInt(0);
        item->hideWhenVideoEnds = m.value("hideWhenVideoEnds").toBool(false);
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
            if (m.contains("startPositionMs")) {
                const qint64 startPos = static_cast<qint64>(std::llround(m.value("startPositionMs").toDouble(0.0)));
                item->startPositionMs = std::max<qint64>(0, startPos);
                item->hasStartPosition = true;
                item->awaitingStartFrame = item->startPositionMs > 0;
            } else {
                item->startPositionMs = 0;
                item->hasStartPosition = false;
                item->awaitingStartFrame = false;
            }
            FileManager::instance().preloadFileIntoMemory(item->fileId);
        }
        m_mediaItems.append(item);
        scheduleMedia(item);
    }

    m_totalMediaToPrime = m_mediaItems.size();
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
    item->hiding = false;
    if (item->hideTimer) {
        item->hideTimer->stop();
    }
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
                    item->loaded = true;
                    evaluateItemReadiness(item);
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
    videoItem->setAspectRatioMode(Qt::IgnoreAspectRatio);
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
    item->videoOutputsAttached = false;
        QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this,epoch,weakItem](QMediaPlayer::MediaStatus s){
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            qDebug() << "RemoteSceneController: mediaStatus" << int(s) << "for" << item->mediaId;
            if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
                item->loaded = true;
                seekToConfiguredStart(item);
                evaluateItemReadiness(item);
            } else if (s == QMediaPlayer::EndOfMedia) {
                if (!item->player) return;
                const bool canRepeat = item->repeatEnabled && item->repeatRemaining > 0 && item->playAuthorized;
                if (canRepeat) {
                    --item->repeatRemaining;
                    item->pausedAtEnd = false;
                    if (item->audio) {
                        item->audio->setMuted(item->muted);
                        item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0));
                    }
                    item->player->setPosition(0);
                    item->player->play();
                } else {
                    item->pausedAtEnd = true;
                    if (item->audio) item->audio->setMuted(true);
                    qint64 dur = item->player->duration();
                    qint64 finalPos = dur > 0 ? std::max<qint64>(0, dur - 1) : 0;
                    item->player->pause();
                    if (finalPos != item->player->position()) {
                        item->player->setPosition(finalPos);
                    }
                    if (item->hideWhenVideoEnds) {
                        fadeOutAndHide(item);
                    }
                }
            }
        });
        QObject::connect(item->player, &QMediaPlayer::playbackStateChanged, item->player, [this,epoch,weakItem](QMediaPlayer::PlaybackState st){
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            qDebug() << "RemoteSceneController: playbackState" << int(st) << "for" << item->mediaId;
        });
        QObject::connect(item->player, &QMediaPlayer::positionChanged, item->player, [this,epoch,weakItem](qint64 pos){
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            if (!item->player) return;

            qint64 dur = item->player->duration();
            if (dur <= 0 || pos <= 0) return;

            constexpr qint64 repeatWindowMs = 120;
            if (item->repeatEnabled && item->repeatRemaining > 0) {
                if (!item->repeatActive && (dur - pos) < repeatWindowMs) {
                    item->repeatActive = true;
                    item->pausedAtEnd = false;
                    if (item->audio) {
                        item->audio->setMuted(item->muted);
                        item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0));
                    }
                    item->player->setPosition(0);
                    item->player->play();
                    --item->repeatRemaining;
                    item->repeatActive = false;
                }
                return;
            }

            if (item->pausedAtEnd) return;
            if ((dur - pos) < 100) {
                item->pausedAtEnd = true;
                if (item->audio) item->audio->setMuted(true);
                item->player->pause();
                if (dur > 1) {
                    item->player->setPosition(std::max<qint64>(0, dur - 1));
                }
                if (item->hideWhenVideoEnds) {
                    fadeOutAndHide(item);
                }
            }
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
                item->pausedAtEnd = false;
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

                item->player->setLoops(QMediaPlayer::Once);
                item->repeatRemaining = (item->repeatEnabled && item->repeatCount > 0)
                                             ? item->repeatCount
                                             : 0;

                // Prime the first frame if not already done
                if (!item->primedFirstFrame) {
                    if (!item->primingSink) {
                        item->primingSink = new QVideoSink(item->player);
                    }
                    QVideoSink* sink = item->primingSink;
                    if (item->player && sink) {
                        item->player->setVideoOutput(sink);
                        item->videoOutputsAttached = false;
                    }
                    if (sink) {
                        item->primingConn = QObject::connect(sink, &QVideoSink::videoFrameChanged, item->player, [this,epoch,weakItem,sink](const QVideoFrame& frame){
                            if (!frame.isValid()) {
                                return;
                            }
                            auto item = weakItem.lock();
                            if (!item) return;
                            if (epoch != m_sceneEpoch) return;
                            if (item->primedFirstFrame) return;

                            const qint64 desired = effectiveStartPosition(item);
                            bool frameReady = true;
                            bool overshoot = false;
                            if (item->awaitingStartFrame && desired >= 0) {
                                const qint64 frameTime = frameTimestampMs(frame);
                                if (frameTime >= 0) {
                                    if (frameTime < desired - kStartPositionToleranceMs) {
                                        frameReady = false;
                                    } else if (frameTime > desired + kStartPositionToleranceMs) {
                                        frameReady = false;
                                        overshoot = true;
                                    }
                                } else if (item->player) {
                                    const qint64 current = item->player->position();
                                    if (current < desired - kStartPositionToleranceMs) {
                                        frameReady = false;
                                    } else if (current > desired + kStartPositionToleranceMs) {
                                        frameReady = false;
                                        overshoot = true;
                                    }
                                }
                                if (!frameReady) {
                                    if (item->player) {
                                        if (overshoot) {
                                            item->player->pause();
                                            item->player->setPosition(desired);
                                        }
                                        if (item->player->playbackState() != QMediaPlayer::PlayingState) {
                                            item->player->play();
                                        }
                                    }
                                    item->primedFrame = QVideoFrame();
                                    clearVideoSinks(item);
                                    item->primedFrame = QVideoFrame();
                                    clearVideoSinks(item);
                                    return;
                                }
                            }

                            item->awaitingStartFrame = false;
                            item->primedFirstFrame = true;
                            item->primedFrame = frame;
                            QObject::disconnect(item->primingConn);
                            if (item->primingSink) {
                                QObject::disconnect(item->primingSink, nullptr, nullptr, nullptr);
                            }
                            if (!item->playAuthorized && item->player) {
                                item->player->pause();
                                if (item->player->position() != desired) {
                                    item->player->setPosition(desired);
                                }
                            }
                            if (item->player) {
                                item->player->setVideoOutput(nullptr);
                            }
                            if (item->primingSink) {
                                item->primingSink->deleteLater();
                                item->primingSink = nullptr;
                            }
                            applyPrimedFrameToSinks(item);
                            evaluateItemReadiness(item);
                            if (item->displayReady && !item->displayStarted) {
                                fadeIn(item);
                            }
                        });
                    } else {
                        qWarning() << "RemoteSceneController: video sink unavailable for priming" << item->mediaId;
                    }
                    if (item->audio) item->audio->setMuted(true);
                    item->pausedAtEnd = false;
                    if (item->player && item->player->playbackState() != QMediaPlayer::PlayingState) {
                        item->player->play();
                    }
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
        int delay = std::max(0, item->autoDisplayDelayMs);
        item->displayTimer = new QTimer(this);
        item->displayTimer->setSingleShot(true);
        connect(item->displayTimer, &QTimer::timeout, this, [this,epoch,weakItem]() {
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            item->displayReady = true;
            fadeIn(item);
        });
        item->pendingDisplayDelayMs = delay;
        if (m_sceneActivated) {
            item->displayTimer->start(delay);
            if (delay == 0) qDebug() << "RemoteSceneController: immediate display for" << item->mediaId;
            item->pendingDisplayDelayMs = -1;
        } else {
            qDebug() << "RemoteSceneController: queued display for" << item->mediaId << "delay" << delay;
        }
    } else {
        item->pendingDisplayDelayMs = -1;
        qDebug() << "RemoteSceneController: autoDisplay disabled; media will stay hidden" << item->mediaId;
    }

    // Play scheduling: allow playback timing independent from display timing
    if (item->player) {
        if (item->autoPlay) {
            int playDelay = std::max(0, item->autoPlayDelayMs);
            item->playTimer = new QTimer(this);
            item->playTimer->setSingleShot(true);
            connect(item->playTimer, &QTimer::timeout, this, [this,epoch,weakItem]() {
                triggerAutoPlayNow(weakItem.lock(), epoch);
            });
            item->pendingPlayDelayMs = playDelay;
            if (m_sceneActivated) {
                if (playDelay == 0) {
                    if (item->playTimer->isActive()) item->playTimer->stop();
                    triggerAutoPlayNow(item, epoch);
                    qDebug() << "RemoteSceneController: immediate play for" << item->mediaId;
                } else {
                    item->playTimer->start(playDelay);
                }
                item->pendingPlayDelayMs = -1;
            } else {
                qDebug() << "RemoteSceneController: queued play for" << item->mediaId << "delay" << playDelay;
            }

            // Pause scheduling: pause video after configured delay if autoPause enabled
            if (item->autoPause) {
                int pauseDelay = std::max(0, item->autoPauseDelayMs);
                item->pauseTimer = new QTimer(this);
                item->pauseTimer->setSingleShot(true);
                connect(item->pauseTimer, &QTimer::timeout, this, [this,epoch,weakItem]() {
                    auto item = weakItem.lock();
                    if (!item) return;
                    if (epoch != m_sceneEpoch) return;
                    if (!item->player) return;
                    if (item->player->playbackState() == QMediaPlayer::PlayingState) {
                        item->player->pause();
                        qDebug() << "RemoteSceneController: auto-paused video" << item->mediaId;
                    }
                });
                item->pendingPauseDelayMs = pauseDelay;
                if (m_sceneActivated) {
                    if (item->awaitingStartFrame) {
                        qDebug() << "RemoteSceneController: deferring pause until start frame for" << item->mediaId << "delay" << pauseDelay;
                    } else {
                        startPendingPauseTimerIfEligible(item);
                        if (pauseDelay == 0) {
                            qDebug() << "RemoteSceneController: immediate pause scheduled for" << item->mediaId;
                        }
                    }
                } else {
                    qDebug() << "RemoteSceneController: queued pause for" << item->mediaId << "delay" << pauseDelay;
                }
            } else {
                item->pendingPauseDelayMs = -1;
            }
        } else {
            qDebug() << "RemoteSceneController: autoPlay disabled; video will not start automatically" << item->mediaId;
            item->pendingPlayDelayMs = -1;
            item->pendingPauseDelayMs = -1;
        }
    }
    // Make sure the most recently scheduled item for this screen is on top to match host stacking
    // (widgets created later are naturally on top, but explicit raise ensures correctness after reparenting)
    w->raise();

    evaluateItemReadiness(item);
}

void RemoteSceneController::scheduleMediaMulti(const std::shared_ptr<RemoteMediaItem>& item) {
    if (item->spans.isEmpty()) return;
    const quint64 epoch = item->sceneEpoch;
    item->hiding = false;
    if (item->hideTimer) {
        item->hideTimer->stop();
    }
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
    s.widget = w;
        
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
            videoItem->setAspectRatioMode(Qt::IgnoreAspectRatio);
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
                    item->loaded = true;
                    evaluateItemReadiness(item);
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
        item->videoOutputsAttached = false;
        QGraphicsVideoItem* primaryVideoItem = (!item->spans.isEmpty()) ? item->spans.first().videoItem : nullptr;
        QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this,epoch,weakItem](QMediaPlayer::MediaStatus s){
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
                item->loaded = true;
                seekToConfiguredStart(item);
                evaluateItemReadiness(item);
            } else if (s == QMediaPlayer::EndOfMedia && item->player) {
                const bool canRepeat = item->repeatEnabled && item->repeatRemaining > 0 && item->playAuthorized;
                if (canRepeat) {
                    --item->repeatRemaining;
                    item->pausedAtEnd = false;
                    if (item->audio) {
                        item->audio->setMuted(item->muted);
                        item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0));
                    }
                    item->player->setPosition(0);
                    item->player->play();
                } else {
                    item->pausedAtEnd = true;
                    if (item->audio) item->audio->setMuted(true);
                    qint64 dur = item->player->duration();
                    qint64 finalPos = dur > 0 ? std::max<qint64>(0, dur - 1) : 0;
                    item->player->pause();
                    if (finalPos != item->player->position()) {
                        item->player->setPosition(finalPos);
                    }
                    if (item->hideWhenVideoEnds) {
                        fadeOutAndHide(item);
                    }
                }
            }
        });
        QObject::connect(item->player, &QMediaPlayer::positionChanged, item->player, [this,epoch,weakItem](qint64 pos){
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            if (!item->player) return;

            qint64 dur = item->player->duration();
            if (dur <= 0 || pos <= 0) return;

            constexpr qint64 repeatWindowMs = 120;
            if (item->repeatEnabled && item->repeatRemaining > 0) {
                if (!item->repeatActive && (dur - pos) < repeatWindowMs) {
                    item->repeatActive = true;
                    item->pausedAtEnd = false;
                    if (item->audio) {
                        item->audio->setMuted(item->muted);
                        item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0));
                    }
                    item->player->setPosition(0);
                    item->player->play();
                    --item->repeatRemaining;
                    item->repeatActive = false;
                }
                return;
            }

            if (item->pausedAtEnd) return;
            if ((dur - pos) < 100) {
                item->pausedAtEnd = true;
                if (item->audio) item->audio->setMuted(true);
                item->player->pause();
                if (dur > 1) {
                    item->player->setPosition(std::max<qint64>(0, dur - 1));
                }
                if (item->hideWhenVideoEnds) {
                    fadeOutAndHide(item);
                }
            }
        });
        QObject::connect(item->player, &QMediaPlayer::errorOccurred, item->player, [this,epoch,weakItem](QMediaPlayer::Error e, const QString& err){ auto item = weakItem.lock(); if (!item) return; if (epoch != m_sceneEpoch) return; if (e != QMediaPlayer::NoError) qWarning() << "RemoteSceneController: player error" << int(e) << err << "for" << item->mediaId; });
    auto attemptLoadVid = [this, epoch, weakItem, primaryVideoItem]() {
            auto item = weakItem.lock();
            if (!item) return false;
            if (epoch != m_sceneEpoch) return false;
            QString path = FileManager::instance().getFilePathForId(item->fileId);
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                item->pausedAtEnd = false;
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

                item->player->setLoops(QMediaPlayer::Once);
                item->repeatRemaining = (item->repeatEnabled && item->repeatCount > 0)
                                             ? item->repeatCount
                                             : 0;

                // Prime the first frame if not already done
                if (!item->primedFirstFrame) {
                    if (!item->primingSink) {
                        item->primingSink = new QVideoSink(item->player);
                    }
                    QVideoSink* sink = item->primingSink;
                    if (item->player && sink) {
                        item->player->setVideoOutput(sink);
                        item->videoOutputsAttached = false;
                    }
                    if (sink) {
                        item->primingConn = QObject::connect(sink, &QVideoSink::videoFrameChanged, item->player, [this,epoch,weakItem,primaryVideoItem](const QVideoFrame& frame){
                            if (!frame.isValid()) {
                                return;
                            }
                            auto item = weakItem.lock();
                            if (!item) return;
                            if (epoch != m_sceneEpoch) return;
                            if (item->primedFirstFrame) return;

                            const qint64 desired = effectiveStartPosition(item);
                            bool frameReady = true;
                            bool overshoot = false;
                            if (item->awaitingStartFrame && desired >= 0) {
                                const qint64 frameTime = frameTimestampMs(frame);
                                if (frameTime >= 0) {
                                    if (frameTime < desired - kStartPositionToleranceMs) {
                                        frameReady = false;
                                    } else if (frameTime > desired + kStartPositionToleranceMs) {
                                        frameReady = false;
                                        overshoot = true;
                                    }
                                } else if (item->player) {
                                    const qint64 current = item->player->position();
                                    if (current < desired - kStartPositionToleranceMs) {
                                        frameReady = false;
                                    } else if (current > desired + kStartPositionToleranceMs) {
                                        frameReady = false;
                                        overshoot = true;
                                    }
                                }
                                if (!frameReady) {
                                    if (item->player) {
                                        if (overshoot) {
                                            item->player->pause();
                                            item->player->setPosition(desired);
                                        }
                                        if (item->player->playbackState() != QMediaPlayer::PlayingState) {
                                            item->player->play();
                                        }
                                    }
                                    item->primedFrame = QVideoFrame();
                                    clearVideoSinks(item);
                                    return;
                                }
                            }

                            item->awaitingStartFrame = false;
                            item->primedFirstFrame = true;
                            item->primedFrame = frame;
                            QObject::disconnect(item->primingConn);
                            if (item->primingSink) {
                                QObject::disconnect(item->primingSink, nullptr, nullptr, nullptr);
                            }
                            if (!item->playAuthorized && item->player) {
                                item->player->pause();
                                if (item->player->position() != desired) {
                                    item->player->setPosition(desired);
                                }
                            }
                            if (item->player) {
                                item->player->setVideoOutput(nullptr);
                            }
                            if (item->primingSink) {
                                item->primingSink->deleteLater();
                                item->primingSink = nullptr;
                            }
                            applyPrimedFrameToSinks(item);
                            evaluateItemReadiness(item);
                            if (item->displayReady && !item->displayStarted) {
                                fadeIn(item);
                            }
                        });
                    } else {
                        qWarning() << "RemoteSceneController: primary video sink unavailable for priming" << item->mediaId;
                    }
                    if (item->audio) item->audio->setMuted(true);
                    item->pausedAtEnd = false;
                    if (item->player && item->player->playbackState() != QMediaPlayer::PlayingState) {
                        item->player->play();
                    }
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
        int delay = std::max(0, item->autoDisplayDelayMs);
        item->displayTimer = new QTimer(this); item->displayTimer->setSingleShot(true);
        connect(item->displayTimer, &QTimer::timeout, this, [this,epoch,weakItem]() {
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            item->displayReady = true;
            fadeIn(item);
        });
        item->pendingDisplayDelayMs = delay;
        if (m_sceneActivated) {
            item->displayTimer->start(delay);
            item->pendingDisplayDelayMs = -1;
        }
    } else {
        item->pendingDisplayDelayMs = -1;
    }
    if (item->player && item->autoPlay) {
        int playDelay = std::max(0, item->autoPlayDelayMs);
        item->playTimer = new QTimer(this);
        item->playTimer->setSingleShot(true);
        connect(item->playTimer, &QTimer::timeout, this, [this,epoch,weakItem]() {
            triggerAutoPlayNow(weakItem.lock(), epoch);
        });
        item->pendingPlayDelayMs = playDelay;
        if (m_sceneActivated) {
            if (playDelay == 0) {
                if (item->playTimer->isActive()) item->playTimer->stop();
                triggerAutoPlayNow(item, epoch);
                qDebug() << "RemoteSceneController: immediate play for (multi-span)" << item->mediaId;
            } else {
                item->playTimer->start(playDelay);
            }
            item->pendingPlayDelayMs = -1;
        }
        
        // Pause scheduling: pause video after configured delay if autoPause enabled
        if (item->autoPause) {
            int pauseDelay = std::max(0, item->autoPauseDelayMs);
            item->pauseTimer = new QTimer(this);
            item->pauseTimer->setSingleShot(true);
            connect(item->pauseTimer, &QTimer::timeout, this, [this,epoch,weakItem]() {
                auto item = weakItem.lock();
                if (!item) return;
                if (epoch != m_sceneEpoch) return;
                if (!item->player) return;
                if (item->player->playbackState() == QMediaPlayer::PlayingState) {
                    item->player->pause();
                    qDebug() << "RemoteSceneController: auto-paused video (multi-span)" << item->mediaId;
                }
            });
            item->pendingPauseDelayMs = pauseDelay;
            if (m_sceneActivated) {
                if (item->awaitingStartFrame) {
                    qDebug() << "RemoteSceneController: deferring pause until start frame for (multi-span)" << item->mediaId << "delay" << pauseDelay;
                } else {
                    startPendingPauseTimerIfEligible(item);
                    if (pauseDelay == 0) {
                        qDebug() << "RemoteSceneController: immediate pause scheduled for (multi-span)" << item->mediaId;
                    }
                }
            }
        } else {
            item->pendingPauseDelayMs = -1;
        }
    } else {
        item->pendingPlayDelayMs = -1;
        item->pendingPauseDelayMs = -1;
    }

    evaluateItemReadiness(item);
}

void RemoteSceneController::fadeIn(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!m_sceneActivated) {
        item->fadeInPending = true;
        item->displayReady = true;
        return;
    }
    if (item->displayStarted) return;
    item->fadeInPending = false;
    item->displayStarted = true;
    item->displayReady = true;
    item->hiding = false;
    if (item->hideTimer) {
        item->hideTimer->stop();
    }
    const int durMs = int(item->fadeInSeconds * 1000.0);
    std::weak_ptr<RemoteMediaItem> weakItem = item;
    auto scheduleHideAfterFade = [this, weakItem, durMs]() {
        auto locked = weakItem.lock();
        if (!locked) return;
        if (!locked->autoHide) return;
        if (durMs <= 10) {
            scheduleHideTimer(locked);
        } else {
            QTimer::singleShot(durMs, this, [this, weakItem]() {
                auto lockedInner = weakItem.lock();
                if (!lockedInner) return;
                scheduleHideTimer(lockedInner);
            });
        }
    };
    // Multi-span path - now uses QGraphicsItem opacity
    if (!item->spans.isEmpty()) {
        if (durMs <= 10) {
            for (auto& s : item->spans) {
                QGraphicsItem* graphicsItem = nullptr;
                if (s.videoItem) graphicsItem = s.videoItem;
                else if (s.imageLabel) graphicsItem = reinterpret_cast<QGraphicsPixmapItem*>(s.imageLabel);
                if (graphicsItem) graphicsItem->setOpacity(item->contentOpacity);
            }
            scheduleHideAfterFade();
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
        scheduleHideAfterFade();
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
        scheduleHideAfterFade();
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
    scheduleHideAfterFade();
}

void RemoteSceneController::scheduleHideTimer(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!item->autoHide) return;
    if (item->hiding) return;
    const int delayMs = std::max(0, item->autoHideDelayMs);
    if (delayMs == 0) {
        fadeOutAndHide(item);
        return;
    }
    if (!item->hideTimer) {
        item->hideTimer = new QTimer(this);
        item->hideTimer->setSingleShot(true);
        std::weak_ptr<RemoteMediaItem> weakItem = item;
        connect(item->hideTimer, &QTimer::timeout, this, [this, weakItem]() {
            auto locked = weakItem.lock();
            if (!locked) return;
            fadeOutAndHide(locked);
        });
    }
    if (!item->hideTimer) return;
    item->hideTimer->stop();
    item->hideTimer->start(delayMs);
}

void RemoteSceneController::fadeOutAndHide(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (item->hiding) return;
    item->hiding = true;
    if (item->hideTimer) {
        item->hideTimer->stop();
    }
    const int durMs = int(std::max(0.0, item->fadeOutSeconds) * 1000.0);
    auto finalize = [item]() {
        item->displayStarted = false;
        item->displayReady = false;
        item->hiding = false;
        if (item->widget) item->widget->hide();
        for (auto& span : item->spans) {
            if (span.widget) span.widget->hide();
        }
        if (item->opacity) item->opacity->setOpacity(0.0);
        if (item->videoItemSingle) item->videoItemSingle->setOpacity(0.0);
        if (item->imageLabelSingle) {
            auto* pixmapItem = reinterpret_cast<QGraphicsPixmapItem*>(item->imageLabelSingle);
            if (pixmapItem) pixmapItem->setOpacity(0.0);
        }
        for (auto& span : item->spans) {
            if (span.videoItem) span.videoItem->setOpacity(0.0);
            if (span.imageLabel) {
                auto* pixmapItem = reinterpret_cast<QGraphicsPixmapItem*>(span.imageLabel);
                if (pixmapItem) pixmapItem->setOpacity(0.0);
            }
        }
    };

    if (!item->spans.isEmpty()) {
        if (durMs <= 10) {
            finalize();
            return;
        }
        auto remaining = std::make_shared<int>(0);
        for (auto& span : item->spans) {
            QGraphicsItem* graphicsItem = nullptr;
            if (span.videoItem) graphicsItem = span.videoItem;
            else if (span.imageLabel) graphicsItem = reinterpret_cast<QGraphicsPixmapItem*>(span.imageLabel);
            if (!graphicsItem) continue;
            ++(*remaining);
            auto* anim = new QVariantAnimation(this);
            anim->setStartValue(graphicsItem->opacity());
            anim->setEndValue(0.0);
            anim->setDuration(durMs);
            anim->setEasingCurve(QEasingCurve::OutCubic);
            connect(anim, &QVariantAnimation::valueChanged, this, [graphicsItem](const QVariant& v) {
                if (graphicsItem) graphicsItem->setOpacity(v.toDouble());
            });
            connect(anim, &QVariantAnimation::finished, anim, [anim]() { anim->deleteLater(); });
            connect(anim, &QVariantAnimation::finished, this, [remaining, finalize]() mutable {
                if (!remaining) return;
                if (--(*remaining) == 0) {
                    finalize();
                }
            });
            anim->start();
        }
        if (*remaining == 0) {
            finalize();
        }
        return;
    }

    QGraphicsItem* graphicsItem = nullptr;
    if (item->videoItemSingle) graphicsItem = item->videoItemSingle;
    else if (item->imageLabelSingle) graphicsItem = reinterpret_cast<QGraphicsPixmapItem*>(item->imageLabelSingle);

    if (!graphicsItem) {
        finalize();
        return;
    }

    if (durMs <= 10) {
        graphicsItem->setOpacity(0.0);
        finalize();
        return;
    }

    auto* anim = new QVariantAnimation(this);
    anim->setStartValue(graphicsItem->opacity());
    anim->setEndValue(0.0);
    anim->setDuration(durMs);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim, &QVariantAnimation::valueChanged, this, [graphicsItem](const QVariant& v) {
        if (graphicsItem) graphicsItem->setOpacity(v.toDouble());
    });
    connect(anim, &QVariantAnimation::finished, anim, [anim]() { anim->deleteLater(); });
    connect(anim, &QVariantAnimation::finished, this, [finalize]() mutable {
        finalize();
    });
    anim->start();
}

#include "RemoteSceneController.h"
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
    win->setAttribute(Qt::WA_TranslucentBackground, true);
    win->setObjectName(QString("RemoteScreenWindow_%1").arg(screenId));
    win->setGeometry(x, y, w, h);
    win->setWindowTitle(primary ? "Remote Scene (Primary)" : "Remote Scene");
    auto* layout = new QHBoxLayout(win);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(0);
    ScreenWindow sw; sw.window = win; sw.x=x; sw.y=y; sw.w=w; sw.h=h; m_screenWindows.insert(screenId, sw);
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
    for (const auto& v : mediaArray) {
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
                QPixmap pm(path);
                if (!pm.isNull()) { lbl->setPixmap(pm); return true; }
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
        item->videoSink = new QVideoSink(w);
        QLabel* videoLabel = new QLabel(w);
        videoLabel->setScaledContents(true);
        videoLabel->setGeometry(0,0,pw,ph);
        QObject::connect(item->videoSink, &QVideoSink::videoFrameChanged, videoLabel, [videoLabel](const QVideoFrame& frame){
            if (!frame.isValid()) return;
            QImage img = frame.toImage();
            if (!img.isNull()) videoLabel->setPixmap(QPixmap::fromImage(img));
        });
        item->player->setVideoSink(item->videoSink);
        auto attemptLoadVid = [item]() {
            QString path = FileManager::instance().getFilePathForId(item->fileId);
            if (!path.isEmpty() && QFileInfo::exists(path)) { item->player->setSource(QUrl::fromLocalFile(path)); return true; }
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
        int playDelay = item->autoPlayDelayMs;
        item->playTimer = new QTimer(this);
        item->playTimer->setSingleShot(true);
        connect(item->playTimer, &QTimer::timeout, this, [item]() { if (item->player) item->player->play(); });
        item->playTimer->start(playDelay);
        if (playDelay == 0) qDebug() << "RemoteSceneController: immediate play for" << item->mediaId;
    } else if (item->player && !item->autoPlay) {
        qDebug() << "RemoteSceneController: autoPlay disabled; video will not start automatically" << item->mediaId;
    }
}

void RemoteSceneController::fadeIn(RemoteMediaItem* item) {
    if (!item || !item->widget || !item->opacity) return;
    item->widget->show();
    const int durMs = int(item->fadeInSeconds * 1000.0);
    if (durMs <= 10) { item->opacity->setOpacity(1.0); return; }
    auto* anim = new QVariantAnimation(item->widget);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setDuration(durMs);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim, &QVariantAnimation::valueChanged, item->widget, [item](const QVariant& v){ if (item->opacity) item->opacity->setOpacity(v.toDouble()); });
    connect(anim, &QVariantAnimation::finished, item->widget, [item,anim](){ anim->deleteLater(); });
    anim->start();
}

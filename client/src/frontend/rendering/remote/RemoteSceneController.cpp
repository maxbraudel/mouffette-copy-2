#include "frontend/rendering/remote/RemoteSceneController.h"
#include "backend/network/WebSocketClient.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
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
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QVideoSink>
#include <QMetaObject>
#include <QUrl>
#include <QThread>
#include <QIODevice>
#include <QGraphicsView>
#include <QGraphicsPixmapItem>
#include <QGraphicsTextItem>
#include <QGraphicsScene>
#include <QTextOption>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextLayout>
#include <QGlyphRun>
#include <QRawFont>
#include <QPainterPath>
#include <QPaintDevice>
#include <QFontMetricsF>
#include <QPen>
#include <QPointer>
#include <QHash>
#include <QVariant>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QAccessible>
#include <cmath>
#include "backend/files/FileManager.h"
#include "backend/platform/macos/MacWindowManager.h"
#include <algorithm>
#include <memory>
#include <QVideoFrameFormat>
#include <array>
#include <limits>


namespace {
constexpr qint64 kStartPositionToleranceMs = 120;
constexpr qint64 kDecoderSyncToleranceMs = 25;
constexpr int kLivePlaybackWarmupFrames = 2;

struct GlyphPathKey {
    QString family;
    QString style;
    qint64 pixelSizeScaled = 0;
    quint32 glyphIndex = 0;

    friend bool operator==(const GlyphPathKey& a, const GlyphPathKey& b) {
        return a.glyphIndex == b.glyphIndex &&
               a.pixelSizeScaled == b.pixelSizeScaled &&
               a.family == b.family &&
               a.style == b.style;
    }
};

uint qHash(const GlyphPathKey& key, uint seed = 0) {
    seed = ::qHash(key.family, seed);
    seed = ::qHash(key.style, seed);
    seed = ::qHash(static_cast<quint64>(key.pixelSizeScaled), seed);
    return ::qHash(key.glyphIndex, seed);
}

QPainterPath cachedGlyphPath(const QRawFont& font, quint32 glyphIndex) {
    static QHash<GlyphPathKey, QPainterPath> s_cache;

    const qreal pixelSize = font.pixelSize();
    const qint64 pixelSizeScaled = static_cast<qint64>(std::llround(pixelSize * 1024.0));
    const GlyphPathKey cacheKey{font.familyName(), font.styleName(), pixelSizeScaled, glyphIndex};
    const auto it = s_cache.constFind(cacheKey);
    if (it != s_cache.cend()) {
        return *it;
    }

    QPainterPath path;
    if (font.isValid()) {
        path = font.pathForGlyph(glyphIndex);
    }
    s_cache.insert(cacheKey, path);
    return path;
}

QFont::Weight qFontWeightFromCss(int cssWeight) {
    struct WeightMapping {
        int css;
        QFont::Weight qt;
    };

    static constexpr std::array<WeightMapping, 9> kMappings = {{
        {100, QFont::Thin},
        {200, QFont::ExtraLight},
        {300, QFont::Light},
        {400, QFont::Normal},
        {500, QFont::Medium},
        {600, QFont::DemiBold},
        {700, QFont::Bold},
        {800, QFont::ExtraBold},
        {900, QFont::Black}
    }};

    int clamped = std::clamp(cssWeight, 1, 1000);
    clamped = ((clamped + 50) / 100) * 100;
    clamped = std::clamp(clamped, 100, 900);

    const WeightMapping* best = &kMappings.front();
    int bestDiff = std::numeric_limits<int>::max();
    for (const auto& mapping : kMappings) {
        const int diff = std::abs(clamped - mapping.css);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = &mapping;
        }
    }

    return best->qt;
}

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

QImage convertFrameToImage(const QVideoFrame& frame) {
    if (!frame.isValid()) {
        return {};
    }

    QImage direct = frame.toImage();
    if (!direct.isNull()) {
        if (direct.format() != QImage::Format_RGBA8888 && direct.format() != QImage::Format_ARGB32_Premultiplied) {
            direct = direct.convertToFormat(QImage::Format_RGBA8888);
        }
        return direct;
    }

    QVideoFrame copy(frame);
    if (!copy.isValid()) {
        return {};
    }

    if (!copy.map(QVideoFrame::ReadOnly)) {
        return {};
    }

    QImage mapped;
    const QVideoFrameFormat format = copy.surfaceFormat();
    const int width = format.frameWidth();
    const int height = format.frameHeight();
    const int stride = copy.bytesPerLine(0);
    const QImage::Format imgFormat = QVideoFrameFormat::imageFormatFromPixelFormat(format.pixelFormat());
    if (imgFormat != QImage::Format_Invalid && width > 0 && height > 0 && stride > 0) {
        mapped = QImage(copy.bits(0), width, height, stride, imgFormat).copy();
    }

    copy.unmap();

    if (!mapped.isNull() && mapped.format() != QImage::Format_RGBA8888 && mapped.format() != QImage::Format_ARGB32_Premultiplied) {
        mapped = mapped.convertToFormat(QImage::Format_RGBA8888);
    }

    return mapped;
}

class RemoteOutlineTextItem : public QGraphicsTextItem {
public:
    RemoteOutlineTextItem() : QGraphicsTextItem() {
        // Enable device coordinate caching to pre-compose all layers before applying opacity
        // This prevents highlight/border/text from blending during fade while maintaining sharpness
        setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    }
    
    explicit RemoteOutlineTextItem(QGraphicsItem* parent) : QGraphicsTextItem(parent) {
        setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    }
    
    RemoteOutlineTextItem(const QString& text, QGraphicsItem* parent) : QGraphicsTextItem(text, parent) {
        setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    }

    void setOutlineParameters(const QColor& fillColor, const QColor& outlineColor, qreal strokeWidthPx) {
        m_fillColor = fillColor;
        m_outlineColor = outlineColor.isValid() ? outlineColor : fillColor;
        m_strokeWidth = std::max<qreal>(0.0, strokeWidthPx);
        
        // Calculate overflow allowance to match host logic
        qreal overflow = 0.0;
        if (m_strokeWidth > 0.0) {
            constexpr qreal kOverflowScale = 0.45;
            constexpr qreal kOverflowMinPx = 2.0;
            overflow = std::ceil(std::max<qreal>(m_strokeWidth * kOverflowScale, kOverflowMinPx));
        }
        m_totalPadding = m_strokeWidth + overflow;
        
        setDefaultTextColor(m_fillColor);
        update();
    }

    void setHighlightParameters(bool enabled, const QColor& color) {
        QColor resolved = color;
        if (!resolved.isValid()) {
            resolved = QColor(255, 255, 0, 160);
        }
        const bool active = enabled && resolved.alpha() > 0;
        if (m_highlightEnabled != active || m_highlightColor != resolved) {
            m_highlightEnabled = active;
            m_highlightColor = active ? resolved : QColor(Qt::transparent);
            update();
        }
    }

protected:
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override {
        if (!painter) {
            return;
        }
        Q_UNUSED(widget);

        QTextDocument* doc = document();
        if (!doc) {
            QGraphicsTextItem::paint(painter, option, widget);
            return;
        }

        painter->save();

        const qreal strokeWidth = m_strokeWidth;
        QColor outlineColor = m_outlineColor.isValid() ? m_outlineColor : m_fillColor;
        const bool highlightActive = m_highlightEnabled && m_highlightColor.isValid() && m_highlightColor.alpha() > 0;

        if (highlightActive) {
            if (QAbstractTextDocumentLayout* docLayout = doc->documentLayout()) {
                const QSizeF docSize = docLayout->documentSize();

                painter->save();
                painter->setPen(Qt::NoPen);
                painter->setBrush(m_highlightColor);

                const qreal docWidth = std::max<qreal>(docSize.width(), 1.0);
                const Qt::Alignment docAlign = doc->defaultTextOption().alignment();
                for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
                    QTextLayout* textLayout = block.layout();
                    if (!textLayout) {
                        continue;
                    }

                    const QRectF blockRect = docLayout->blockBoundingRect(block);
                    for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
                        QTextLine line = textLayout->lineAt(lineIndex);
                        if (!line.isValid()) {
                            continue;
                        }

                        const qreal lineWidth = line.naturalTextWidth();
                        const qreal width = std::max<qreal>(lineWidth, 1.0);
                        qreal alignedX = line.x();
                        if (std::abs(alignedX) < 1e-4 && width < docWidth - 1e-4) {
                            const qreal horizontalSpace = std::max<qreal>(0.0, docWidth - width);
                            if (docAlign.testFlag(Qt::AlignRight)) {
                                alignedX += horizontalSpace;
                            } else if (docAlign.testFlag(Qt::AlignHCenter)) {
                                alignedX += horizontalSpace * 0.5;
                            }
                        }
                        const qreal height = std::max<qreal>(line.height(), 1.0);
                        const QPointF topLeft = blockRect.topLeft() + QPointF(alignedX, line.y());
                        painter->drawRect(QRectF(topLeft, QSizeF(width, height)));
                    }
                }

                painter->restore();
            }
        }

        // Clear outline formatting
        {
            QTextCursor cursor(doc);
            cursor.select(QTextCursor::Document);
            QTextCharFormat format;
            format.setForeground(m_fillColor);
            format.clearProperty(QTextFormat::TextOutline);
            cursor.mergeCharFormat(format);
        }

        if (strokeWidth > 0.0) {
            // Build glyph paths
            QPainterPath textPath;
            textPath.setFillRule(Qt::WindingFill); // Preserve glyph counters
            QAbstractTextDocumentLayout* docLayout = doc->documentLayout();
            for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
                QTextLayout* textLayout = block.layout();
                if (!textLayout) continue;
                
                const QRectF blockRect = docLayout->blockBoundingRect(block);
                for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
                    QTextLine line = textLayout->lineAt(lineIndex);
                    if (!line.isValid()) continue;
                    
                    QList<QGlyphRun> glyphRuns = line.glyphRuns();
                    for (const QGlyphRun& run : glyphRuns) {
                        const QVector<quint32> indexes = run.glyphIndexes();
                        const QVector<QPointF> positions = run.positions();
                        if (indexes.size() != positions.size()) continue;
                        const QRawFont rawFont = run.rawFont();
                        for (int gi = 0; gi < indexes.size(); ++gi) {
                            const QPainterPath glyphPath = cachedGlyphPath(rawFont, indexes[gi]);
                            const QPointF glyphPos = blockRect.topLeft() + positions[gi];
                            textPath.addPath(glyphPath.translated(glyphPos));
                        }
                    }
                }
            }
            
            // Draw outside stroke
            painter->save();
            painter->setPen(QPen(outlineColor, strokeWidth * 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(textPath);
            painter->restore();
            
            // Fill glyphs
            painter->save();
            painter->setPen(Qt::NoPen);
            painter->setBrush(m_fillColor);
            painter->drawPath(textPath);
            painter->restore();
        } else {
            QAbstractTextDocumentLayout::PaintContext ctx;
            ctx.palette.setColor(QPalette::Text, m_fillColor);
            doc->documentLayout()->draw(painter, ctx);
        }

        painter->restore();
    }

    QRectF boundingRect() const override {
        QRectF bounds = QGraphicsTextItem::boundingRect();
        if (m_totalPadding > 0.0) {
            // Expand bounds to include stroke overflow
            bounds.adjust(-m_totalPadding, -m_totalPadding, m_totalPadding, m_totalPadding);
        }
        return bounds;
    }

private:
    QColor m_fillColor = Qt::white;
    QColor m_outlineColor = Qt::white;
    qreal m_strokeWidth = 0.0;
    qreal m_totalPadding = 0.0;
    bool m_highlightEnabled = false;
    QColor m_highlightColor = Qt::transparent;
};
} // namespace

RemoteSceneController::RemoteSceneController(FileManager* fileManager, WebSocketClient* ws, QObject* parent)
    : QObject(parent)
    , m_fileManager(fileManager)
    , m_ws(ws) {
    if (m_ws) {
        connect(m_ws, &WebSocketClient::remoteSceneStartReceived, this, &RemoteSceneController::onRemoteSceneStart);
        connect(m_ws, &WebSocketClient::remoteSceneStopReceived, this, &RemoteSceneController::onRemoteSceneStop);
    }
}

RemoteSceneController::~RemoteSceneController() {
    clearScene();
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        ScreenWindow& sw = it.value();
        if (sw.window) {
            sw.window->deleteLater();
            sw.window = nullptr;
        }
    }
    m_screenWindows.clear();
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

    if (m_sceneStartInProgress || m_teardownInProgress) {
        qDebug() << "RemoteSceneController: deferring remote scene start while teardown is pending";
        m_deferredSceneStart.senderId = senderClientId;
        m_deferredSceneStart.scene = scene;
        m_deferredSceneStart.valid = true;
        return;
    }

    m_sceneStartInProgress = true;
    auto cleanup = std::shared_ptr<void>(nullptr, [this](void*) {
        m_sceneStartInProgress = false;
        dispatchDeferredSceneStart();
    });

    const QJsonArray screens = scene.value("screens").toArray();
    const QJsonArray media = scene.value("media").toArray();

    auto failWithMessage = [&](const QString& errorMsg) {
        qWarning() << "RemoteSceneController: validation failed -" << errorMsg;
        if (m_ws) {
            m_ws->sendRemoteSceneValidationResult(senderClientId, false, errorMsg);
        }
    };

    if (screens.isEmpty()) {
        failWithMessage(QStringLiteral("Scene has no screen configuration"));
        return;
    }

    if (media.isEmpty()) {
        failWithMessage(QStringLiteral("Scene has no media items"));
        return;
    }

    QStringList missingFileNames;
    for (const QJsonValue& val : media) {
        const QJsonObject mediaObj = val.toObject();
        const QString type = mediaObj.value("type").toString();
        
        // Text items don't have files, skip validation for them
        if (type == "text") {
            continue;
        }
        
        const QString fileId = mediaObj.value("fileId").toString();
        if (fileId.isEmpty()) {
            qWarning() << "RemoteSceneController: media item has no fileId";
            continue;
        }
        const QString path = m_fileManager->getFilePathForId(fileId);
        if (path.isEmpty() || !QFile::exists(path)) {
            QString fileName = mediaObj.value("fileName").toString();
            if (fileName.isEmpty()) {
                fileName = fileId;
            }
            missingFileNames.append(fileName);
        }
    }

    if (!missingFileNames.isEmpty()) {
        const QString fileList = missingFileNames.size() <= 3
                                     ? missingFileNames.join(", ")
                                     : QString("%1, %2, and %3 more")
                                           .arg(missingFileNames[0])
                                           .arg(missingFileNames[1])
                                           .arg(missingFileNames.size() - 2);
        failWithMessage(QStringLiteral("Missing %1 file%2: %3")
                            .arg(missingFileNames.size())
                            .arg(missingFileNames.size() > 1 ? "s" : "")
                            .arg(fileList));
        return;
    }

    qDebug() << "RemoteSceneController: validation successful, preparing scene from" << senderClientId;

    ++m_sceneEpoch;
    clearScene();
    // Flush deferred deletions multiple times to ensure ALL nested widget deletions complete
    drainDeferredDeletes(5, true);

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

    // Cancel any pending window show timer from previous scene
    if (m_windowShowTimer) {
        m_windowShowTimer->stop();
        m_windowShowTimer->deleteLater();
        m_windowShowTimer = nullptr;
    }

    // Capture current epoch to prevent stale deferred callbacks from operating on wrong scene
    const quint64 epoch = m_sceneEpoch;
    
    // Capture screen IDs (not pointers!) to look up windows at execution time
    // This avoids dangling pointer issues when widgets are deleted and memory reused
    QList<int> screenIds;
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        screenIds.append(it.key());
    }
    
    // Create a tracked timer (not singleShot) so we can cancel it in clearScene
    m_windowShowTimer = new QTimer(this);
    m_windowShowTimer->setSingleShot(true);
    m_windowShowTimer->setInterval(10);
    
    // Use a slightly longer delay (10ms) to ensure all deferred widget deletions have completed
    // before showing new windows. This prevents crashes in macOS accessibility code when
    // windows are rapidly created/destroyed. The delay is imperceptible to users but critical
    // for avoiding race conditions in Qt's widget deletion machinery.
    connect(m_windowShowTimer, &QTimer::timeout, this, [this, epoch, screenIds]() {
        // Abort if scene changed (stop/start happened during deferral)
        if (epoch != m_sceneEpoch) return;
        
        // Process any remaining deferred deletions before showing windows
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        
        // Look up windows by screen ID at execution time to ensure they're still valid
        for (int screenId : screenIds) {
            if (epoch != m_sceneEpoch) return;

            auto it = m_screenWindows.find(screenId);
            if (it == m_screenWindows.end()) continue;

            ScreenWindow& sw = it.value();
            if (sw.sceneEpoch != epoch) continue;

            QWidget* window = sw.window;
            if (!window) continue;

            window->show();
#ifdef Q_OS_MAC
            QTimer::singleShot(0, this, [this, epoch, screenId]() {
                if (epoch != m_sceneEpoch) return;

                auto macIt = m_screenWindows.find(screenId);
                if (macIt == m_screenWindows.end()) return;

                ScreenWindow& macWindow = macIt.value();
                if (macWindow.sceneEpoch != epoch) return;

                QWidget* w = macWindow.window;
                if (!w) return;

                MacWindowManager::setWindowAsGlobalOverlay(w, /*clickThrough*/ true);
            });
#endif
        }

        startSceneActivationIfReady();
    });
    
    m_windowShowTimer->start();
}

void RemoteSceneController::onRemoteSceneStop(const QString& senderClientId) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, senderClientId]() { onRemoteSceneStop(senderClientId); }, Qt::QueuedConnection);
        return;
    }

    ++m_sceneEpoch;
    clearScene();

    if (m_ws) {
        m_ws->sendRemoteSceneStopResult(senderClientId, true);
    }
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

    m_teardownInProgress = true;

    // CRITICAL: Cancel pending window show timer to prevent showing windows after scene cleared
    if (m_windowShowTimer) {
        m_windowShowTimer->stop();
        m_windowShowTimer->deleteLater();
        m_windowShowTimer = nullptr;
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
    
    // Close remote screen windows so overlays disappear immediately after stop.
    // This releases their native cocoa windows while coordinating with Qt's
    // accessibility bridge to avoid macOS crashes when rapidly restarting scenes.
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        ScreenWindow& sw = it.value();
        if (!sw.window) {
            continue;
        }

        QWidget* window = sw.window;
        sw.sceneEpoch = 0;

        QObject::disconnect(window, nullptr, nullptr, nullptr);
        window->hide();

        // Notify accessibility clients that the overlay is no longer visible.
        QAccessibleEvent hideEvent(window, QAccessible::ObjectHide);
        QAccessible::updateAccessibility(&hideEvent);

#ifdef Q_OS_MAC
        MacWindowManager::orderOutWindow(window);
#endif

        // Manually purge Qt's accessibility cache for this widget (fix for QTBUG-95134)
        QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(window);
        if (iface) {
            QAccessible::Id id = QAccessible::uniqueId(iface);
            QAccessible::deleteAccessibleInterface(id);
        }

        // Detach and destroy the graphics scene so it will be rebuilt cleanly.
        if (sw.graphicsView) {
            QObject::disconnect(sw.graphicsView, nullptr, nullptr, nullptr);
            sw.graphicsView->setScene(nullptr);
            sw.graphicsView->deleteLater();
            sw.graphicsView = nullptr;
        }
        if (sw.scene) {
            QObject::disconnect(sw.scene, nullptr, nullptr, nullptr);
            sw.scene->clear();
            sw.scene->deleteLater();
            sw.scene = nullptr;
        }

        window->close();
        window->lower();

        QAccessibleEvent destroyEvent(window, QAccessible::ObjectDestroyed);
        QAccessible::updateAccessibility(&destroyEvent);

        window->setParent(nullptr);
        window->deleteLater();

        sw.window = nullptr;
    }

    m_screenWindows.clear();

    // Make sure deferred deletions run to completion before allowing another scene start
    // On macOS, process more cycles to ensure accessibility cleanup (QTBUG-95134)
#ifdef Q_OS_MAC
    drainDeferredDeletes(6, true);
#else
    drainDeferredDeletes(4, true);
#endif

    m_teardownInProgress = false;

    // Cancel any pending restart cooldown timer and restart if we still have a deferred request
    if (m_sceneRestartDelayTimer) {
        m_sceneRestartDelayTimer->stop();
        m_sceneRestartDelayTimer->deleteLater();
        m_sceneRestartDelayTimer = nullptr;
    }

    if (!m_sceneStartInProgress) {
        if (m_deferredSceneStart.valid) {
            m_restartCooldownActive = true;
            scheduleSceneRestartCooldown();
        } else {
            dispatchDeferredSceneStart();
        }
    }
}

void RemoteSceneController::dispatchDeferredSceneStart() {
    if (!m_deferredSceneStart.valid) {
        return;
    }

    if (m_restartCooldownActive) {
        return;
    }

    if (!m_enabled) {
        m_deferredSceneStart.valid = false;
        return;
    }

    if (m_sceneStartInProgress || m_teardownInProgress) {
        return;
    }

    PendingSceneRequest request = m_deferredSceneStart;
    m_deferredSceneStart.valid = false;

    QMetaObject::invokeMethod(this, [this, request]() {
        if (!m_enabled) {
            return;
        }
        onRemoteSceneStart(request.senderId, request.scene);
    }, Qt::QueuedConnection);
}

void RemoteSceneController::drainDeferredDeletes(int passes, bool allowEventProcessing) {
    if (passes <= 0) {
        return;
    }

    for (int i = 0; i < passes; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (allowEventProcessing) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
    }
}

void RemoteSceneController::scheduleSceneRestartCooldown() {
    // Increase cooldown on macOS to give accessibility bridge more time to clear (QTBUG-95134)
#ifdef Q_OS_MAC
    constexpr int kRestartCooldownMs = 150;
#else
    constexpr int kRestartCooldownMs = 60;
#endif

    if (!m_sceneRestartDelayTimer) {
        m_sceneRestartDelayTimer = new QTimer(this);
        m_sceneRestartDelayTimer->setSingleShot(true);
        connect(m_sceneRestartDelayTimer, &QTimer::timeout, this, [this]() {
            m_restartCooldownActive = false;
            if (m_sceneRestartDelayTimer) {
                m_sceneRestartDelayTimer->deleteLater();
                m_sceneRestartDelayTimer = nullptr;
            }
            dispatchDeferredSceneStart();
        });
    }

    if (m_sceneRestartDelayTimer->isActive()) {
        m_sceneRestartDelayTimer->stop();
    }
    m_sceneRestartDelayTimer->start(kRestartCooldownMs);
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
    stopAndDeleteTimer(item->muteTimer);
    stopAndDeleteTimer(item->hideEndDelayTimer);
    stopAndDeleteTimer(item->muteEndDelayTimer);

    cancelAudioFade(item, false);

    QObject::disconnect(item->deferredStartConn);
    QObject::disconnect(item->primingConn);
    QObject::disconnect(item->mirrorConn);
    item->pausedAtEnd = false;
    item->hideEndTriggered = false;
    item->muteEndTriggered = false;

    if (item->primingSink) {
        QObject::disconnect(item->primingSink, nullptr, nullptr, nullptr);
        item->primingSink->deleteLater();
        item->primingSink = nullptr;
    }

    if (item->liveSink) {
        QObject::disconnect(item->liveSink, nullptr, nullptr, nullptr);
        item->liveSink->deleteLater();
        item->liveSink = nullptr;
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
        item->audio->setVolume(0.0);
    }
    item->muted = true;

    for (auto& span : item->spans) {
        if (span.textItem) {
            if (span.textItem->scene()) {
                span.textItem->scene()->removeItem(span.textItem);
            }
            delete span.textItem;
            span.textItem = nullptr;
        }
        if (span.imageItem) {
            if (span.imageItem->scene()) {
                span.imageItem->scene()->removeItem(span.imageItem);
            }
            delete span.imageItem;
            span.imageItem = nullptr;
        }
        if (span.widget) {
            span.widget->hide();
            span.widget->deleteLater();
            span.widget = nullptr;
        }
    }
    item->spans.clear();

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
    item->primedFrameSticky = false;
    item->lastFrameImage = QImage();
    item->lastFramePixmap = QPixmap();
    item->playAuthorized = false;
    item->hiding = false;
    item->readyNotified = false;
    item->fadeInPending = false;
    item->pendingDisplayDelayMs = -1;
    item->pendingPlayDelayMs = -1;
    item->pendingPauseDelayMs = -1;
    item->startPositionMs = 0;
    item->hasStartPosition = false;
    item->displayTimestampMs = -1;
    item->hasDisplayTimestamp = false;
    item->awaitingStartFrame = false;
    item->awaitingDecoderSync = false;
    item->decoderSyncTargetMs = -1;
    item->awaitingLivePlayback = false;
    item->livePlaybackStarted = false;
    item->liveWarmupFramesRemaining = 0;
    item->lastLiveFrameTimestampMs = -1;
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
    if (item->awaitingDecoderSync) return;
    if (item->awaitingLivePlayback && !item->livePlaybackStarted) return;

    item->pauseTimer->start(item->pendingPauseDelayMs);
    item->pendingPauseDelayMs = -1;
}

void RemoteSceneController::triggerAutoPlayNow(const std::shared_ptr<RemoteMediaItem>& item, quint64 epoch) {
    if (!item) return;
    if (epoch != m_sceneEpoch) return;
    if (!item->player) return;

    item->playAuthorized = true;
    item->repeatActive = false;
    restoreVideoOutput(item);
    if (item->audio) {
        // Don't override mute state - it's managed by scene activation and unmute automation
        // Only set volume here
        item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0));
    }
    item->pausedAtEnd = false;
    item->repeatRemaining = (item->repeatEnabled && item->repeatCount > 0) ? item->repeatCount : 0;
    item->awaitingLivePlayback = true;
    item->livePlaybackStarted = false;
    item->liveWarmupFramesRemaining = kLivePlaybackWarmupFrames;
    item->lastLiveFrameTimestampMs = -1;

    if (item->loaded) {
        const qint64 startPos = item->hasStartPosition ? effectiveStartPosition(item) : 0;
        if (item->player->position() != startPos) {
            item->player->setPosition(startPos);
        }

        const bool canGate = item->primedFirstFrame && item->primingSink;
        if (canGate) {
            item->awaitingDecoderSync = true;
            if (item->decoderSyncTargetMs < 0) {
                item->decoderSyncTargetMs = targetDisplayTimestamp(item);
            }
            item->player->setVideoSink(item->primingSink);
            item->videoOutputsAttached = false;
            if (item->primedFrame.isValid()) {
                applyPrimedFrameToSinks(item);
            }
        } else {
            ensureVideoOutputsAttached(item);
            if (item->primedFrame.isValid()) {
                applyPrimedFrameToSinks(item);
            }
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
                    item->awaitingLivePlayback = true;
                    item->livePlaybackStarted = false;
                    item->liveWarmupFramesRemaining = kLivePlaybackWarmupFrames;
                    item->lastLiveFrameTimestampMs = -1;
                const bool canGate = item->primedFirstFrame && item->primingSink;
                if (canGate) {
                    item->awaitingDecoderSync = true;
                    if (item->decoderSyncTargetMs < 0) {
                        item->decoderSyncTargetMs = targetDisplayTimestamp(item);
                    }
                    item->player->setVideoSink(item->primingSink);
                    item->videoOutputsAttached = false;
                    if (item->primedFrame.isValid()) {
                        applyPrimedFrameToSinks(item);
                    }
                } else {
                    ensureVideoOutputsAttached(item);
                    if (item->primedFrame.isValid()) {
                        applyPrimedFrameToSinks(item);
                    }
                }
                item->player->play();
            }
            item->pausedAtEnd = false;
            item->repeatRemaining = (item->repeatEnabled && item->repeatCount > 0) ? item->repeatCount : 0;
        }
    });
}

void RemoteSceneController::applyPixmapToSpans(const std::shared_ptr<RemoteMediaItem>& item, const QPixmap& pixmap) const {
    if (!item) return;
    if (pixmap.isNull()) return;

    for (auto& span : item->spans) {
        if (!span.imageItem) continue;
        // Safety check: verify scene still contains the item before updating pixmap
        if (span.imageItem->scene() == nullptr) continue;
        const int targetW = span.widget ? std::max(1, span.widget->width()) : std::max(1, pixmap.width());
        const int targetH = span.widget ? std::max(1, span.widget->height()) : std::max(1, pixmap.height());
        span.imageItem->setPixmap(pixmap.scaled(QSize(targetW, targetH), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    }
}

void RemoteSceneController::applyPrimedFrameToSinks(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!item->primedFrame.isValid()) return;
    if (!item->primedFrameSticky) return;

    // Only block applying primed frame if awaiting playback AND autoDisplay is false
    // (autoDisplay videos should show their primed frame immediately)
    if (item->awaitingLivePlayback && !item->livePlaybackStarted && !item->autoDisplay) return;

    QImage image = convertFrameToImage(item->primedFrame);
    if (image.isNull()) return;

    item->lastFrameImage = image;
    item->lastFramePixmap = QPixmap::fromImage(image);
    applyPixmapToSpans(item, item->lastFramePixmap);
}

void RemoteSceneController::clearRenderedFrames(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (item->awaitingLivePlayback && !item->livePlaybackStarted) return;

    item->lastFrameImage = QImage();
    item->lastFramePixmap = QPixmap();

    for (auto& span : item->spans) {
        if (span.imageItem) {
            span.imageItem->setPixmap(QPixmap());
        }
    }
}

void RemoteSceneController::ensureVideoOutputsAttached(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (item->videoOutputsAttached) {
        applyPrimedFrameToSinks(item);
        return;
    }
    if (!item->player) return;

    if (!item->liveSink) {
        item->liveSink = new QVideoSink(item->player);
    }

    item->player->setVideoSink(item->liveSink);
    QObject::disconnect(item->mirrorConn);

    if (item->liveSink) {
        std::weak_ptr<RemoteMediaItem> weakItem = item;
        const quint64 epoch = item->sceneEpoch;
        item->mirrorConn = QObject::connect(item->liveSink, &QVideoSink::videoFrameChanged, item->liveSink, [this, epoch, weakItem](const QVideoFrame& frame) {
            if (!frame.isValid()) return;
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            // Safety check: verify live sink still exists
            if (!item->liveSink) return;

            if (item->holdLastFrameAtEnd) {
                return;
            }

            item->primedFrame = frame;

            QImage converted = convertFrameToImage(frame);
            if (!converted.isNull()) {
                item->lastFrameImage = converted;
                item->lastFramePixmap = QPixmap::fromImage(converted);
            }

            const qint64 ts = frameTimestampMs(frame);
            if (item->awaitingLivePlayback && !item->livePlaybackStarted) {
                bool advancedFrame = false;
                if (ts >= 0 && ts != item->lastLiveFrameTimestampMs) {
                    item->lastLiveFrameTimestampMs = ts;
                    advancedFrame = true;
                } else if (ts < 0) {
                    advancedFrame = true;
                }
                if (advancedFrame && item->liveWarmupFramesRemaining > 0) {
                    --item->liveWarmupFramesRemaining;
                }
                if (item->liveWarmupFramesRemaining <= 0) {
                    finalizeLivePlaybackStart(item, frame);
                }
            }

            if (!item->lastFramePixmap.isNull()) {
                applyPixmapToSpans(item, item->lastFramePixmap);
            }
        });
    }

    item->videoOutputsAttached = true;
    applyPrimedFrameToSinks(item);
}

void RemoteSceneController::finalizeLivePlaybackStart(const std::shared_ptr<RemoteMediaItem>& item, const QVideoFrame& frame) {
    if (!item) return;
    if (item->livePlaybackStarted) return;
    item->awaitingLivePlayback = false;
    item->livePlaybackStarted = true;
    item->liveWarmupFramesRemaining = 0;
    if (frame.isValid()) {
        item->primedFrame = frame;
        item->primedFrameSticky = true;
        const qint64 ts = frameTimestampMs(frame);
        if (ts >= 0) {
            item->displayTimestampMs = ts;
            item->hasDisplayTimestamp = true;
            item->lastLiveFrameTimestampMs = ts;
        }
    }
    if (item->fadeInPending && item->displayReady && !item->displayStarted) {
        fadeIn(item);
    } else if (!item->displayStarted && item->displayReady) {
        fadeIn(item);
    }
    startPendingPauseTimerIfEligible(item);
}

void RemoteSceneController::activateScene() {
    if (m_sceneActivated) return;
    m_sceneActivated = true;
    m_sceneActivationRequested = false;
    m_pendingActivationEpoch = 0;

    if (m_sceneReadyTimeout) {
        m_sceneReadyTimeout->stop();
    }

    // Mute all videos at scene start and schedule automatic unmute if enabled
    const quint64 epoch = m_sceneEpoch;
    for (const auto& item : m_mediaItems) {
        if (!item || item->type != "video") continue;
        if (!item->audio) continue;
        
        // Only mute at scene start if NOT using mute-when-video-ends automation
        if (!item->muteWhenVideoEnds) {
            applyAudioMuteState(item, true, true);
        }
        
        // Schedule automatic unmute if enabled
        if (item->autoUnmute) {
            const int unmuteDelayMs = std::max(0, item->autoUnmuteDelayMs);
            auto unmuteCallback = [this, item, epoch]() {
                if (epoch != m_sceneEpoch) return; // Scene changed
                if (!item || !item->audio) return; // Item deleted
                if (!m_sceneActivated) return; // Scene stopped
                applyAudioMuteState(item, false);
            };
            
            if (unmuteDelayMs > 0) {
                QTimer::singleShot(unmuteDelayMs, this, unmuteCallback);
            } else {
                QTimer::singleShot(0, this, unmuteCallback);
            }
        }

        item->hideEndTriggered = false;
        item->muteEndTriggered = false;

        if (item->autoMute && !item->muteWhenVideoEnds) {
            scheduleMuteTimer(item);
        } else if (item->muteTimer) {
            item->muteTimer->stop();
        }
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

qint64 RemoteSceneController::targetDisplayTimestamp(const std::shared_ptr<RemoteMediaItem>& item) const {
    if (!item) return 0;
    if (item->hasDisplayTimestamp && item->displayTimestampMs >= 0) {
        qint64 ts = item->displayTimestampMs;
        if (item->player) {
            const qint64 dur = item->player->duration();
            if (dur > 0 && ts >= dur) {
                ts = std::max<qint64>(qint64(0), dur - 1);
            }
        }
        return std::max<qint64>(0, ts);
    }
    return effectiveStartPosition(item);
}

void RemoteSceneController::freezeVideoOutput(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item || item->type != "video") return;
    if (item->holdLastFrameAtEnd) return;
    if (!item->primedFrame.isValid()) {
        return;
    }

    QImage image = convertFrameToImage(item->primedFrame);
    if (image.isNull()) {
        qWarning() << "RemoteSceneController: unable to convert final video frame for" << item->mediaId;
    } else {
        item->lastFrameImage = image;
        item->lastFramePixmap = QPixmap::fromImage(image);
    }

    item->holdLastFrameAtEnd = true;

    if (!item->lastFramePixmap.isNull()) {
        applyPixmapToSpans(item, item->lastFramePixmap);
        for (auto& span : item->spans) {
            if (!span.imageItem) continue;
            span.imageItem->setOpacity(item->contentOpacity);
            span.imageItem->setVisible(true);
        }
    }
    // Handle mute-on-end with optional delay
    if (item->muteWhenVideoEnds && item->audio && !item->muteEndTriggered) {
        const int muteDelayMs = item->autoMuteDelayMs;
        if (muteDelayMs > 0) {
            if (!item->muteEndDelayTimer) {
                item->muteEndDelayTimer = new QTimer(this);
                item->muteEndDelayTimer->setSingleShot(true);
                std::weak_ptr<RemoteMediaItem> weakItem = item;
                QObject::connect(item->muteEndDelayTimer, &QTimer::timeout, this, [this, weakItem]() {
                    auto locked = weakItem.lock();
                    if (!locked || !locked->audio) return;
                    if (locked->sceneEpoch != m_sceneEpoch) return;
                    applyAudioMuteState(locked, true);
                    locked->muteEndTriggered = true;
                });
            }
            item->muteEndDelayTimer->start(muteDelayMs);
        } else {
            applyAudioMuteState(item, true);
            item->muteEndTriggered = true;
        }
    }

    // Handle hide-on-end with optional delay
    if (item->hideWhenVideoEnds && !item->hideEndTriggered) {
        const int hideDelayMs = item->autoHideDelayMs;
        if (hideDelayMs > 0) {
            if (!item->hideEndDelayTimer) {
                item->hideEndDelayTimer = new QTimer(this);
                item->hideEndDelayTimer->setSingleShot(true);
                std::weak_ptr<RemoteMediaItem> weakItem = item;
                QObject::connect(item->hideEndDelayTimer, &QTimer::timeout, this, [this, weakItem]() {
                    auto locked = weakItem.lock();
                    if (!locked) return;
                    if (locked->sceneEpoch != m_sceneEpoch) return;
                    locked->hideEndTriggered = true;
                    fadeOutAndHide(locked);
                });
            }
            item->hideEndDelayTimer->start(hideDelayMs);
        } else {
            item->hideEndTriggered = true;
            fadeOutAndHide(item);
        }
    }
}

void RemoteSceneController::restoreVideoOutput(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item || item->type != "video") return;

    item->holdLastFrameAtEnd = false;
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
        qDebug() << "RemoteSceneController: seekToConfiguredStart" << item->mediaId
                 << "target" << target
                 << "current" << current
                 << "awaiting" << item->awaitingStartFrame;
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

void RemoteSceneController::resetWindowForNewScene(ScreenWindow& sw, int screenId, int x, int y, int w, int h, bool primary) {
    if (!sw.window || !sw.graphicsView) return;

    sw.x = x;
    sw.y = y;
    sw.w = w;
    sw.h = h;
    sw.sceneEpoch = m_sceneEpoch;

    sw.window->hide();
    sw.window->setGeometry(x, y, w, h);
    sw.window->setWindowTitle(primary ? "Remote Scene (Primary)" : "Remote Scene");

    // Replace graphics scene to ensure a clean slate for the new remote scene
    QGraphicsScene* oldScene = sw.scene;
    if (sw.graphicsView->scene()) {
        sw.graphicsView->setScene(nullptr);
    }
    sw.scene = new QGraphicsScene(sw.graphicsView);
    sw.scene->setSceneRect(0, 0, w, h);
    sw.graphicsView->setScene(sw.scene);

    if (oldScene) {
        oldScene->clear();
        oldScene->deleteLater();
    }

#ifdef Q_OS_MAC
    MacWindowManager::setWindowAsGlobalOverlay(sw.window, /*clickThrough*/ true);
#endif
}

QWidget* RemoteSceneController::ensureScreenWindow(int screenId, int x, int y, int w, int h, bool primary) {
    ScreenWindow& sw = m_screenWindows[screenId];

    if (!sw.window) {
        sw.window = new QWidget();
        
        // Force native window on macOS to avoid accessibility crashes (QTBUG-95134)
        
        sw.window->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        sw.window->setWindowFlag(Qt::FramelessWindowHint, true);
        sw.window->setWindowFlag(Qt::WindowStaysOnTopHint, true);
#ifdef Q_OS_WIN
        sw.window->setWindowFlag(Qt::Tool, true);
        sw.window->setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
        sw.window->setAttribute(Qt::WA_ShowWithoutActivating, true);
#endif
        sw.window->setAttribute(Qt::WA_TranslucentBackground, true);
        sw.window->setAttribute(Qt::WA_NoSystemBackground, true);
        sw.window->setAttribute(Qt::WA_OpaquePaintEvent, false);
        sw.window->setObjectName(QString("RemoteScreenWindow_%1").arg(screenId));

        sw.graphicsView = new QGraphicsView(sw.window);
        sw.graphicsView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        sw.graphicsView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        sw.graphicsView->setFrameStyle(QFrame::NoFrame);
        sw.graphicsView->setAttribute(Qt::WA_TranslucentBackground, true);
        sw.graphicsView->setStyleSheet("background: transparent;");
        sw.graphicsView->setRenderHint(QPainter::Antialiasing, true);
        sw.graphicsView->setRenderHint(QPainter::SmoothPixmapTransform, true);
        if (sw.graphicsView->viewport()) {
            sw.graphicsView->viewport()->setAutoFillBackground(false);
            sw.graphicsView->viewport()->setAttribute(Qt::WA_TranslucentBackground, true);
        }

        auto* layout = new QHBoxLayout(sw.window);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(sw.graphicsView);
    }

    resetWindowForNewScene(sw, screenId, x, y, w, h, primary);
    return sw.window;
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
    
    // Parse base dimensions for all media types (needed for scaling)
    item->baseWidth = m.value("baseWidth").toInt(0);
    item->baseHeight = m.value("baseHeight").toInt(0);
    
    // Parse text-specific properties if this is a text item
    if (item->type == "text") {
        item->text = m.value("text").toString();
        item->fontFamily = m.value("fontFamily").toString("Arial");
        item->fontSize = m.value("fontSize").toInt(12);
        item->fontBold = m.value("fontBold").toBool(false);
        item->fontItalic = m.value("fontItalic").toBool(false);
    item->fontWeight = m.value("fontWeight").toInt(0);
        item->textColor = m.value("textColor").toString("#FFFFFF");
    item->textBorderWidthPercent = m.value("textBorderWidthPercent").toDouble(0.0);
    item->textBorderColor = m.value("textBorderColor").toString();
    item->fitToTextEnabled = m.value("textFitToTextEnabled").toBool(false);
    item->highlightEnabled = m.value("textHighlightEnabled").toBool(false);
        item->textHighlightColor = m.value("textHighlightColor").toString();
        double uniformScale = m.value("uniformScale").toDouble(1.0);
        if (!std::isfinite(uniformScale) || std::abs(uniformScale) < 1e-6) {
            uniformScale = 1.0;
        }
        item->uniformScale = uniformScale;

        const QString hAlign = m.value("horizontalAlignment").toString("center").toLower();
        if (hAlign == QLatin1String("left")) {
            item->horizontalAlignment = RemoteMediaItem::HorizontalAlignment::Left;
        } else if (hAlign == QLatin1String("right")) {
            item->horizontalAlignment = RemoteMediaItem::HorizontalAlignment::Right;
        } else {
            item->horizontalAlignment = RemoteMediaItem::HorizontalAlignment::Center;
        }

        const QString vAlign = m.value("verticalAlignment").toString("center").toLower();
        if (vAlign == QLatin1String("top")) {
            item->verticalAlignment = RemoteMediaItem::VerticalAlignment::Top;
        } else if (vAlign == QLatin1String("bottom")) {
            item->verticalAlignment = RemoteMediaItem::VerticalAlignment::Bottom;
        } else {
            item->verticalAlignment = RemoteMediaItem::VerticalAlignment::Center;
        }
    }
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
        if (item->spans.isEmpty()) {
            qWarning() << "RemoteSceneController: media item" << item->mediaId << "missing spans; skipping placement";
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
            item->autoUnmute = m.value("autoUnmute").toBool(false);
            item->autoUnmuteDelayMs = m.value("autoUnmuteDelayMs").toInt(0);
            item->autoMute = m.value("autoMute").toBool(false);
            item->autoMuteDelayMs = m.value("autoMuteDelayMs").toInt(0);
            item->muteWhenVideoEnds = m.value("muteWhenVideoEnds").toBool(false);
            item->audioFadeInSeconds = std::max(0.0, m.value("audioFadeInSeconds").toDouble(0.0));
            item->audioFadeOutSeconds = std::max(0.0, m.value("audioFadeOutSeconds").toDouble(0.0));
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
            if (m.contains("displayedFrameTimestampMs")) {
                const qint64 displayTs = static_cast<qint64>(std::llround(m.value("displayedFrameTimestampMs").toDouble(-1.0)));
                if (displayTs >= 0) {
                    item->displayTimestampMs = displayTs;
                    item->hasDisplayTimestamp = true;
                }
            }
            m_fileManager->preloadFileIntoMemory(item->fileId);
        }
        m_mediaItems.append(item);
        scheduleMedia(item);
    }

    m_totalMediaToPrime = m_mediaItems.size();
}

void RemoteSceneController::scheduleMedia(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (item->spans.isEmpty()) {
        qWarning() << "RemoteSceneController: ignoring media with no spans" << item->mediaId << item->type;
        return;
    }
    scheduleMediaMulti(item);
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
    s.widget = w;
        
        // Get the scene for this screen
        QGraphicsScene* scene = winIt.value().scene;
        if (!scene) continue;
        
        if (item->type == "text") {
            // Create a text item for text media
            RemoteOutlineTextItem* textItem = new RemoteOutlineTextItem();
            textItem->setPos(px, py);
            textItem->setOpacity(0.0);
            if (QTextDocument* doc = textItem->document()) {
                doc->setDocumentMargin(0.0);
            }

            // Set up font
            QFont font(item->fontFamily, item->fontSize);
            font.setItalic(item->fontItalic);
            if (item->fontWeight > 0) {
                font.setWeight(qFontWeightFromCss(item->fontWeight));
            } else if (item->fontBold) {
                font.setWeight(QFont::Bold);
            }
            textItem->setFont(font);

            // Set text color
            QColor color(item->textColor);
            if (!color.isValid()) {
                color = QColor(Qt::white);
            }

            auto computeOutlineWidth = [](double percent, const QFont& baseFont) -> qreal {
                if (percent <= 0.0) {
                    return 0.0;
                }
                QFontMetricsF metrics(baseFont);
                qreal reference = metrics.height();
                if (reference <= 0.0) {
                    if (baseFont.pixelSize() > 0) {
                        reference = static_cast<qreal>(baseFont.pixelSize());
                    } else {
                        reference = baseFont.pointSizeF();
                    }
                }
                if (reference <= 0.0) {
                    reference = 16.0;
                }
                constexpr qreal kMaxOutlineThicknessFactor = 0.35;
                const qreal normalized = std::clamp(percent / 100.0, 0.0, 1.0);
                const qreal eased = std::pow(normalized, 1.1);
                return eased * kMaxOutlineThicknessFactor * reference;
            };

            const qreal strokeWidth = computeOutlineWidth(item->textBorderWidthPercent, font);
            auto outlineOverflowAllowance = [](qreal stroke) -> qreal {
                if (stroke <= 0.0) {
                    return 0.0;
                }
                constexpr qreal kOverflowScale = 0.45;
                constexpr qreal kOverflowMinPx = 2.0;
                return std::ceil(std::max<qreal>(stroke * kOverflowScale, kOverflowMinPx));
            };

            const qreal padding = std::max<qreal>(0.0, strokeWidth + outlineOverflowAllowance(strokeWidth));
            QColor outlineColor(item->textBorderColor);
            if (!outlineColor.isValid()) {
                outlineColor = color;
            }

            // Set text content
            textItem->setPlainText(item->text);

            if (QTextDocument* doc = textItem->document()) {
                QTextCursor cursor(doc);
                cursor.select(QTextCursor::Document);
                QTextCharFormat format;
                format.setForeground(color);
                format.clearProperty(QTextFormat::TextOutline);
                cursor.mergeCharFormat(format);
            }

            textItem->setOutlineParameters(color, outlineColor, strokeWidth);
            QColor highlightColor(item->textHighlightColor);
            if (!highlightColor.isValid()) {
                highlightColor = QColor(255, 255, 0, 160);
            }
            textItem->setHighlightParameters(item->highlightEnabled, highlightColor);

            // Center alignment
            QTextDocument* doc = textItem->document();
            QTextOption textOption = doc ? doc->defaultTextOption() : QTextOption();
            textOption.setWrapMode(item->fitToTextEnabled ? QTextOption::NoWrap : QTextOption::WordWrap);
            Qt::Alignment hAlign = Qt::AlignHCenter;
            switch (item->horizontalAlignment) {
                case RemoteMediaItem::HorizontalAlignment::Left:
                    hAlign = Qt::AlignLeft;
                    break;
                case RemoteMediaItem::HorizontalAlignment::Center:
                    hAlign = Qt::AlignHCenter;
                    break;
                case RemoteMediaItem::HorizontalAlignment::Right:
                    hAlign = Qt::AlignRight;
                    break;
            }
            textOption.setAlignment(hAlign);
            if (doc) {
                doc->setDefaultTextOption(textOption);
            }
            
            // Reconstruct host layout: host renders text into a logical width that is
            // reduced by the uniform scale factor, then applies that factor visually.
            const qreal baseWidth = static_cast<qreal>(item->baseWidth > 0 ? item->baseWidth : 200);
            const qreal baseHeight = static_cast<qreal>(item->baseHeight > 0 ? item->baseHeight : 100);
            const qreal uniformScale = std::max<qreal>(static_cast<qreal>(std::abs(item->uniformScale)), 1e-4);

            // Match host calculation: divide base width by scale first, then subtract padding
            // Host: logicalWidth = (baseWidth / uniformScale) - 2*padding
            const qreal logicalWidth = std::max<qreal>(1.0, (baseWidth / uniformScale) - 2.0 * padding);
            if (item->fitToTextEnabled) {
                textItem->setTextWidth(-1.0);
            } else {
                textItem->setTextWidth(logicalWidth);
            }

            QSizeF docSize;
            if (doc && doc->documentLayout()) {
                docSize = doc->documentLayout()->documentSize();
            } else {
                const qreal logicalHeight = std::max<qreal>(1.0, (baseHeight - 2.0 * padding) / uniformScale);
                docSize = QSizeF(logicalWidth, logicalHeight);
            }

            const qreal safeBaseWidth = std::max<qreal>(baseWidth, 1.0);
            const qreal safeBaseHeight = std::max<qreal>(baseHeight, 1.0);
            const qreal scaleX = static_cast<qreal>(pw) / safeBaseWidth;
            const qreal scaleY = static_cast<qreal>(ph) / safeBaseHeight;
            const qreal appliedScale = scaleX * uniformScale;
            textItem->setScale(appliedScale);
            
            // Apply padding offsets to match host-side margin handling
            const qreal paddingX = padding * scaleX;
            const qreal paddingY = padding * scaleY;

            // Center the text vertically within the padded height (matches host offset logic)
            const qreal scaledDocHeight = docSize.height() * appliedScale;
            const qreal availableHeightScene = std::max<qreal>(0.0, static_cast<qreal>(ph) - 2.0 * paddingY);
            qreal verticalOffset = paddingY;
            switch (item->verticalAlignment) {
                case RemoteMediaItem::VerticalAlignment::Top:
                    verticalOffset = paddingY;
                    break;
                case RemoteMediaItem::VerticalAlignment::Center:
                    verticalOffset = paddingY + std::max<qreal>(0.0, (availableHeightScene - scaledDocHeight) * 0.5);
                    break;
                case RemoteMediaItem::VerticalAlignment::Bottom:
                    verticalOffset = paddingY + std::max<qreal>(0.0, availableHeightScene - scaledDocHeight);
                    break;
            }
            
            // Position the text with horizontal padding as well
            const qreal horizontalOffset = paddingX;
            textItem->setPos(px + horizontalOffset, py + verticalOffset);
            
            scene->addItem(textItem);
            s.textItem = textItem;
        } else if (item->type == "image") {
            // Create a pixmap item for host-provided still images
            QGraphicsPixmapItem* pixmapItem = new QGraphicsPixmapItem();
            pixmapItem->setPos(px, py);
            pixmapItem->setOpacity(0.0);
            pixmapItem->setTransformationMode(Qt::SmoothTransformation);
            scene->addItem(pixmapItem);
            s.imageItem = pixmapItem;
        } else if (item->type == "video") {
            // Create a pixmap item to display CPU-rendered video frames
            QGraphicsPixmapItem* frameItem = new QGraphicsPixmapItem();
            frameItem->setPos(px, py);
            frameItem->setOpacity(0.0);
            frameItem->setTransformationMode(Qt::SmoothTransformation);
            scene->addItem(frameItem);
            s.imageItem = frameItem;
        }
        w->hide(); // Hide widget container since items render in scene
        w->raise();
    }

    // Content loading
    std::weak_ptr<RemoteMediaItem> weakItem = item;

    if (item->type == "text") {
        // Text items are immediately ready (no file to load)
        item->loaded = true;
        evaluateItemReadiness(item);
    } else if (item->type == "image") {
        auto attemptLoad = [this, epoch, weakItem]() {
            auto item = weakItem.lock();
            if (!item) return false;
            if (epoch != m_sceneEpoch) return false;
            QString path = m_fileManager->getFilePathForId(item->fileId);
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                QPixmap pm; 
                if (pm.load(path)) {
                    for (auto& s : item->spans) { 
                        if (s.imageItem) {
                            const QWidget* parentWidget = s.widget ? s.widget->parentWidget() : nullptr;
                            const int containerWidth = parentWidget ? parentWidget->width() : 100;
                            const int containerHeight = parentWidget ? parentWidget->height() : 100;
                            const int pw = int(s.nw * containerWidth);
                            const int ph = int(s.nh * containerHeight);
                            QPixmap scaled = pm.scaled(QSize(std::max(1, pw), std::max(1, ph)), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                            s.imageItem->setPixmap(scaled);
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
        // CPU-rendered video playback driven by a shared QVideoSink
        QWidget* parentForAv = nullptr; 
        if (!item->spans.isEmpty()) parentForAv = item->spans.first().widget;
        
        item->player = new QMediaPlayer(parentForAv);
        item->audio = new QAudioOutput(parentForAv);
        item->audio->setMuted(item->muted); 
        item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0));
        item->player->setAudioOutput(item->audio);
        item->videoOutputsAttached = false;
        QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this,epoch,weakItem](QMediaPlayer::MediaStatus s){
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
                item->loaded = true;
                seekToConfiguredStart(item);
                evaluateItemReadiness(item);
            } else if (s == QMediaPlayer::EndOfMedia && item->player) {
                // EndOfMedia reached: ensure we freeze on the final frame when not looping.
                const bool canRepeat = item->repeatEnabled && item->repeatRemaining > 0 && item->playAuthorized;
                if (canRepeat) {
                    --item->repeatRemaining;
                    item->pausedAtEnd = false;
                    item->holdLastFrameAtEnd = false;
                    if (item->audio) {
                        item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0));
                    }
                    restoreVideoOutput(item);
                    item->player->setPosition(0);
                    item->player->play();
                } else {
                    if (!item->pausedAtEnd) {
                        item->pausedAtEnd = true;
                        item->player->pause();
                    }
                    freezeVideoOutput(item);
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

            // Handle pre-end mute trigger for negative delays
            if (item->muteWhenVideoEnds && !item->muteEndTriggered && item->autoMuteDelayMs < 0 && item->audio) {
                const qint64 offsetMs = -static_cast<qint64>(item->autoMuteDelayMs);
                if ((dur - pos) <= offsetMs) {
                    applyAudioMuteState(item, true);
                    item->muteEndTriggered = true;
                }
            }

            // Handle pre-end hide trigger for negative delays
            if (item->hideWhenVideoEnds && !item->hideEndTriggered && item->autoHideDelayMs < 0) {
                const qint64 offsetMs = -static_cast<qint64>(item->autoHideDelayMs);
                if ((dur - pos) <= offsetMs) {
                    item->hideEndTriggered = true;
                    fadeOutAndHide(item);
                }
            }

            if (item->pausedAtEnd) return;
        });
        QObject::connect(item->player, &QMediaPlayer::errorOccurred, item->player, [this,epoch,weakItem](QMediaPlayer::Error e, const QString& err){ auto item = weakItem.lock(); if (!item) return; if (epoch != m_sceneEpoch) return; if (e != QMediaPlayer::NoError) qWarning() << "RemoteSceneController: player error" << int(e) << err << "for" << item->mediaId; });
    auto attemptLoadVid = [this, epoch, weakItem]() {
            auto item = weakItem.lock();
            if (!item) return false;
            if (epoch != m_sceneEpoch) return false;
            QString path = m_fileManager->getFilePathForId(item->fileId);
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                item->pausedAtEnd = false;
                auto bytes = m_fileManager->getFileBytes(item->fileId);
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
                        item->player->setVideoSink(sink);
                        item->videoOutputsAttached = false;
                    }
                    if (sink) {
                        qDebug() << "RemoteSceneController: start priming(multi)" << item->mediaId
                                 << "startMs" << (item->hasStartPosition ? item->startPositionMs : qint64(-1))
                                 << "displayTs" << (item->hasDisplayTimestamp ? item->displayTimestampMs : qint64(-1))
                                 << "awaitingStart" << item->awaitingStartFrame;
                        item->primingConn = QObject::connect(sink, &QVideoSink::videoFrameChanged, sink, [this,epoch,weakItem](const QVideoFrame& frame){
                            if (!frame.isValid()) {
                                return;
                            }
                            auto item = weakItem.lock();
                            if (!item) return;
                            if (epoch != m_sceneEpoch) return;
                            // Safety check: verify priming sink still exists
                            if (!item->primingSink) return;
                            // Safety check: verify player still exists
                            if (!item->player) return;
                            const qint64 desired = targetDisplayTimestamp(item);
                            const qint64 frameTime = frameTimestampMs(frame);
                            const bool hasFrameTimestamp = frameTime >= 0;
                            const qint64 playerPos = item->player ? item->player->position() : -1;
                            const qint64 reference = hasFrameTimestamp ? frameTime : playerPos;

                            auto logDecision = [&](const QString& stage, const QString& reason, bool accepted) {
                                qDebug() << "RemoteSceneController: priming(multi)" << stage
                                         << "media" << item->mediaId
                                         << "reason" << reason
                                         << "desired" << desired
                                         << "frameTs" << (hasFrameTimestamp ? frameTime : qint64(-1))
                                         << "playerPos" << playerPos
                                         << "delta" << ((desired >= 0 && (hasFrameTimestamp || playerPos >= 0)) ? ((hasFrameTimestamp ? frameTime : playerPos) - desired) : qint64(0))
                                         << "displayTs" << (item->hasDisplayTimestamp ? item->displayTimestampMs : qint64(-1))
                                         << "startMs" << (item->hasStartPosition ? item->startPositionMs : qint64(-1))
                                         << "awaiting" << item->awaitingStartFrame
                                         << "accepted" << accepted;
                            };

                            if (!item->primedFirstFrame) {
                                bool frameReady = true;
                                bool overshoot = false;
                                if (item->awaitingStartFrame && desired >= 0) {
                                    if (reference >= 0) {
                                        if (reference < desired - kStartPositionToleranceMs) {
                                            frameReady = false;
                                        } else if (reference > desired + kStartPositionToleranceMs) {
                                            frameReady = false;
                                            overshoot = true;
                                        }
                                    }
                                    if (!frameReady) {
                                        logDecision(QStringLiteral("reject"), overshoot ? QStringLiteral("overshoot") : QStringLiteral("pre-start"), false);
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
                                        item->primedFrameSticky = false;
                                        clearRenderedFrames(item);
                                        return;
                                    }
                                }

                                logDecision(QStringLiteral("accept"), QStringLiteral("frame within tolerance"), true);

                                item->awaitingStartFrame = false;
                                item->primedFirstFrame = true;
                                item->primedFrame = frame;
                                item->primedFrameSticky = true;
                                if (hasFrameTimestamp) {
                                    item->displayTimestampMs = frameTime;
                                    item->hasDisplayTimestamp = true;
                                }
                                item->decoderSyncTargetMs = desired;
                                item->livePlaybackStarted = false;
                                item->lastLiveFrameTimestampMs = -1;
                                if (item->autoPlay) {
                                    item->awaitingLivePlayback = true;
                                    item->liveWarmupFramesRemaining = kLivePlaybackWarmupFrames;
                                } else {
                                    item->awaitingLivePlayback = false;
                                    item->liveWarmupFramesRemaining = 0;
                                }
                                if (item->player) {
                                    item->player->pause();
                                    if (item->player->position() != desired) {
                                        item->player->setPosition(desired >= 0 ? desired : 0);
                                    }
                                }
                                applyPrimedFrameToSinks(item);
                                evaluateItemReadiness(item);
                                // Allow display even if awaiting live playback, so autoDisplay works immediately
                                if (item->autoDisplay && item->displayReady && !item->displayStarted) {
                                    fadeIn(item);
                                }
                                return;
                            }

                            if (!item->awaitingDecoderSync) {
                                return;
                            }

                            const qint64 target = (item->decoderSyncTargetMs >= 0) ? item->decoderSyncTargetMs : desired;
                            if (target < 0) {
                                return;
                            }
                            const qint64 gateReference = reference;
                            if (gateReference >= 0 && gateReference >= target - kDecoderSyncToleranceMs) {
                                qDebug() << "RemoteSceneController: decoder sync reached" << item->mediaId
                                         << "target" << target
                                         << "frameTs" << (hasFrameTimestamp ? frameTime : qint64(-1))
                                         << "playerPos" << playerPos;
                                item->awaitingDecoderSync = false;
                                item->decoderSyncTargetMs = -1;
                                if (hasFrameTimestamp) {
                                    item->displayTimestampMs = frameTime;
                                    item->hasDisplayTimestamp = true;
                                }
                                item->primedFrame = frame;
                                item->primedFrameSticky = false;
                                QObject::disconnect(item->primingConn);
                                item->primingConn = {};
                                if (item->primingSink) {
                                    QObject::disconnect(item->primingSink, nullptr, nullptr, nullptr);
                                    auto* sinkToDelete = item->primingSink;
                                    item->primingSink = nullptr;
                                    sinkToDelete->deleteLater();
                                }
                                if (item->liveWarmupFramesRemaining <= 0) {
                                    item->liveWarmupFramesRemaining = kLivePlaybackWarmupFrames;
                                }
                                ensureVideoOutputsAttached(item);
                                if (item->player && item->player->playbackState() != QMediaPlayer::PlayingState) {
                                    item->player->play();
                                }
                                startPendingPauseTimerIfEligible(item);
                            }
                        });
                    } else {
                        qWarning() << "RemoteSceneController: primary video sink unavailable for priming" << item->mediaId;
                    }
                    if (item->audio) applyAudioMuteState(item, true, true);
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
        // Mark displayReady immediately so primed frame can be shown
        item->displayReady = true;
        item->displayTimer = new QTimer(this); item->displayTimer->setSingleShot(true);
        connect(item->displayTimer, &QTimer::timeout, this, [this,epoch,weakItem]() {
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
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
    // Only block fade-in if awaiting live playback AND autoDisplay is false
    // (autoDisplay=true should show primed frame immediately, even before play starts)
    if (item->awaitingLivePlayback && !item->livePlaybackStarted && !item->autoDisplay) {
        item->fadeInPending = true;
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
        if (locked->hideWhenVideoEnds) return;
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
    if (item->spans.isEmpty()) {
        qWarning() << "RemoteSceneController: fadeIn requested with no spans" << item->mediaId;
        scheduleHideAfterFade();
        return;
    }
    if (durMs <= 10) {
        for (auto& s : item->spans) {
            if (s.textItem) {
                s.textItem->setOpacity(item->contentOpacity);
                s.textItem->setVisible(true);
            } else if (s.imageItem) {
                s.imageItem->setOpacity(item->contentOpacity);
                s.imageItem->setVisible(true);
            }
        }
        scheduleHideAfterFade();
        return;
    }
    for (auto& s : item->spans) {
        QGraphicsItem* graphicsItem = nullptr;
        if (s.textItem) {
            graphicsItem = static_cast<QGraphicsItem*>(s.textItem);
        } else if (s.imageItem) {
            graphicsItem = static_cast<QGraphicsItem*>(s.imageItem);
        }
        if (!graphicsItem) continue;
        graphicsItem->setVisible(true);
        auto* anim = new QVariantAnimation(this);
        anim->setStartValue(0.0);
        anim->setEndValue(item->contentOpacity);
        anim->setDuration(durMs);
        anim->setEasingCurve(QEasingCurve::Linear);
        connect(anim, &QVariantAnimation::valueChanged, this, [graphicsItem](const QVariant& v){
            if (graphicsItem) graphicsItem->setOpacity(v.toDouble());
        });
        connect(anim, &QVariantAnimation::finished, anim, [anim]() { anim->deleteLater(); });
        anim->start();
    }
    scheduleHideAfterFade();
}

void RemoteSceneController::scheduleHideTimer(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!item->autoHide) return;
    if (item->hideWhenVideoEnds) return;
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

void RemoteSceneController::cancelAudioFade(const std::shared_ptr<RemoteMediaItem>& item, bool applyFinalState) {
    if (!item) return;
    if (!item->audioFadeAnimation) return;
    QVariantAnimation* anim = item->audioFadeAnimation.data();
    QObject::disconnect(anim, nullptr, this, nullptr);
    anim->stop();
    anim->deleteLater();
    item->audioFadeAnimation = nullptr;
    if (applyFinalState && item->audio) {
        const qreal targetVolume = item->muted ? 0.0 : std::clamp<qreal>(item->volume, 0.0, 1.0);
        item->audio->setMuted(item->muted);
        item->audio->setVolume(targetVolume);
    }
}

void RemoteSceneController::applyAudioMuteState(const std::shared_ptr<RemoteMediaItem>& item, bool muted, bool skipFade) {
    if (!item) return;
    if (!item->audio) return;

    const qreal clampedTargetVolume = muted ? 0.0 : std::clamp<qreal>(item->volume, 0.0, 1.0);
    const double fadeSeconds = skipFade ? 0.0 : (muted ? item->audioFadeOutSeconds : item->audioFadeInSeconds);

    const bool deviceMuted = item->audio->isMuted();
    const qreal deviceVolume = std::clamp<qreal>(item->audio->volume(), 0.0, 1.0);

    if (muted == item->muted && !item->audioFadeAnimation) {
        if (deviceMuted == muted && std::abs(deviceVolume - clampedTargetVolume) < 0.0001) {
            item->audio->setMuted(muted);
            item->audio->setVolume(clampedTargetVolume);
            return;
        }
    }

    cancelAudioFade(item, false);

    if (fadeSeconds <= 0.0) {
        item->audio->setMuted(muted);
        item->audio->setVolume(clampedTargetVolume);
        item->muted = muted;
        return;
    }

    qreal startVolume = deviceVolume;
    if (!muted && (deviceMuted || item->muted)) {
        startVolume = 0.0;
    }
    const qreal endVolume = muted ? 0.0 : clampedTargetVolume;

    if (std::abs(startVolume - endVolume) < 0.0001) {
        item->audio->setMuted(muted);
        item->audio->setVolume(endVolume);
        item->muted = muted;
        return;
    }

    item->audio->setMuted(false);
    item->audio->setVolume(startVolume);

    std::weak_ptr<RemoteMediaItem> weakItem = item;
    auto* anim = new QVariantAnimation(this);
    anim->setDuration(static_cast<int>(fadeSeconds * 1000.0));
    anim->setStartValue(startVolume);
    anim->setEndValue(endVolume);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim, &QVariantAnimation::valueChanged, this, [weakItem](const QVariant& v) {
        auto locked = weakItem.lock();
        if (!locked || !locked->audio) return;
        const qreal value = std::clamp<qreal>(v.toDouble(), 0.0, 1.0);
        locked->audio->setVolume(value);
    });
    connect(anim, &QVariantAnimation::finished, this, [weakItem, muted, endVolume, anim]() {
        auto locked = weakItem.lock();
        if (!locked || !locked->audio) {
            anim->deleteLater();
            return;
        }
        locked->audio->setMuted(muted);
        locked->audio->setVolume(endVolume);
        if (locked->audioFadeAnimation == anim) {
            locked->audioFadeAnimation = nullptr;
        }
        anim->deleteLater();
    });
    connect(anim, &QObject::destroyed, this, [weakItem, anim]() {
        auto locked = weakItem.lock();
        if (!locked) return;
        if (locked->audioFadeAnimation == anim) {
            locked->audioFadeAnimation = nullptr;
        }
    });

    item->audioFadeAnimation = anim;
    item->muted = muted;
    anim->start();
}

void RemoteSceneController::scheduleMuteTimer(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!item->autoMute) return;
    if (item->muteWhenVideoEnds) return;
    if (!item->audio) return;
    const int delayMs = std::max(0, item->autoMuteDelayMs);
    if (item->muteTimer) {
        item->muteTimer->stop();
    }
    if (delayMs == 0) {
        applyAudioMuteState(item, true);
        return;
    }
    if (!item->muteTimer) {
        item->muteTimer = new QTimer(this);
        item->muteTimer->setSingleShot(true);
        std::weak_ptr<RemoteMediaItem> weakItem = item;
        connect(item->muteTimer, &QTimer::timeout, this, [this, weakItem]() {
            auto locked = weakItem.lock();
            if (!locked) return;
            if (locked->sceneEpoch != m_sceneEpoch) return;
            if (!locked->audio) return;
            applyAudioMuteState(locked, true);
        });
    }
    item->muteTimer->start(delayMs);
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
        for (auto& span : item->spans) {
            if (span.widget) span.widget->hide();
            if (span.textItem) span.textItem->setOpacity(0.0);
            if (span.imageItem) span.imageItem->setOpacity(0.0);
        }
    };

    if (item->spans.isEmpty()) {
        qWarning() << "RemoteSceneController: fadeOut requested with no spans" << item->mediaId;
        finalize();
        return;
    }
    if (durMs <= 10) {
        finalize();
        return;
    }
    auto remaining = std::make_shared<int>(0);
    for (auto& span : item->spans) {
        QGraphicsItem* graphicsItem = nullptr;
        if (span.textItem) {
            graphicsItem = static_cast<QGraphicsItem*>(span.textItem);
        } else if (span.imageItem) {
            graphicsItem = static_cast<QGraphicsItem*>(span.imageItem);
        }
        if (!graphicsItem) continue;
        ++(*remaining);
        auto* anim = new QVariantAnimation(this);
        anim->setStartValue(graphicsItem->opacity());
        anim->setEndValue(0.0);
        anim->setDuration(durMs);
        anim->setEasingCurve(QEasingCurve::Linear);
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
}

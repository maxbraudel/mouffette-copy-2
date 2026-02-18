// TextMediaItem.cpp - Implementation of text media item
#include "TextMediaItem.h"
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QGraphicsSceneMouseEvent>
#include <algorithm>
#include <cmath>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QTransform>
#include <QTextDocument>
#include <QTextOption>
#include <QAbstractTextDocumentLayout>
#include <QTextCursor>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QColor>
#include <QtGlobal>
#include <QTextBlock>
#include <QTextLayout>
#include <QGlyphRun>
#include <QRawFont>
#include <QPainterPath>
#include <QBrush>
#include <QFontMetricsF>
#include <QClipboard>
#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QCursor>
#include <QHashFunctions>
#include <QPen>
#include <QFontDatabase>
#include <QDebug>
#include <QElapsedTimer>
#include <QCache>
#include <QMutex>
#include <QMutexLocker>
#include <array>
#include <limits>
#include <functional>
#include "frontend/rendering/canvas/OverlayPanels.h"
#include <QObject>
#include <QScopedValueRollback>
#include <QTimer>
#include <QPalette>
#include <chrono>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <memory>
#include <utility>

// TextMediaDefaults namespace implementation
namespace TextMediaDefaults {
    const QString FONT_FAMILY = QStringLiteral("Arial");
    const int FONT_SIZE = 48;
    const QFont::Weight FONT_WEIGHT = QFont::Normal;
    const int FONT_WEIGHT_VALUE = 400;
    const bool FONT_ITALIC = false;
    const bool FONT_UNDERLINE = false;
    const bool FONT_ALL_CAPS = false;
    const QColor TEXT_COLOR = Qt::white;
    const qreal TEXT_BORDER_WIDTH_PERCENT = 0.0;
    const QColor TEXT_BORDER_COLOR = Qt::black;
    const bool TEXT_HIGHLIGHT_ENABLED = false;
    const QColor TEXT_HIGHLIGHT_COLOR = QColor(255, 255, 0, 128);
    const QString DEFAULT_TEXT = QStringLiteral("Text");
    const int DEFAULT_WIDTH = 400;
    const int DEFAULT_HEIGHT = 200;
    const qreal DEFAULT_VIEWPORT_SCALE = 1.0;
}

// Static member definition
int TextMediaItem::s_maxRasterDimension = 100000;

void TextMediaItem::setMaxRasterDimension(int pixels) {
    s_maxRasterDimension = std::max(256, std::min(pixels, 100000));
}

int TextMediaItem::maxRasterDimension() {
    return s_maxRasterDimension;
}

namespace {

bool envFlagEnabled(const char* primary, const char* fallback = nullptr) {
    const auto parse = [](const QByteArray& raw) {
        if (raw.isEmpty()) {
            return false;
        }
        const QByteArray lowered = raw.trimmed().toLower();
        return lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on";
    };

    if (qEnvironmentVariableIsSet(primary) && parse(qgetenv(primary))) {
        return true;
    }
    if (fallback && qEnvironmentVariableIsSet(fallback) && parse(qgetenv(fallback))) {
        return true;
    }
    return false;
}

constexpr qreal kContentPadding = 4.0;
constexpr qreal kStrokeOverflowScale = 0.75;
constexpr qreal kStrokeOverflowMinPx = 1.5;
constexpr int kScaledRasterThrottleIntervalMs = 45;
constexpr qreal kFitToTextMinWidth = 24.0;
constexpr int kFitToTextSizeStabilizationPx = 1;
constexpr qreal kMaxOutlineThicknessFactor = 0.35;
constexpr int kMaxCachedGlyphPaths = 60000;
constexpr int kMaxRenderedGlyphPixmaps = 500;  // Phase 2: Rendered glyph cache limit
constexpr int kFallbackMaxDimensionPx = 2048;
constexpr qreal kFallbackMinScale = 0.50;      // Increased from 0.35 to 0.50 for better low-zoom preview quality
constexpr qreal kInitialPreviewScaleMax = 0.45;
constexpr qreal kPreviewStrokePercentCap = 35.0;
constexpr qreal kMinPreviewStrokePercent = 8.0;
constexpr int kStrokeHeavyGlyphThreshold = 1800;
constexpr qint64 kStrokeHeavyPixelThreshold = 2800000;
constexpr int kFallbackFontPixelSize = 12;

bool textProfilingEnabled() {
    static const bool enabled = envFlagEnabled("MOUFFETTE_TEXT_PROFILING");
    return enabled;
}

bool textHotLogsEnabled() {
    static const bool enabled = envFlagEnabled("MOUFFETTE_TEXT_HOT_LOGS");
    return enabled;
}

bool textSchedulerV2Enabled() {
    static const bool enabled = envFlagEnabled("MOUFFETTE_TEXT_RENDER_SCHEDULER_V2", "text.render.scheduler.v2");
    return enabled;
}

bool textCachePolicyV2Enabled() {
    static const bool enabled = envFlagEnabled("MOUFFETTE_TEXT_CACHE_POLICY_V2", "text.cache.policy.v2");
    return enabled;
}

bool textRendererGpuEnabled() {
    static const bool enabled = envFlagEnabled("MOUFFETTE_TEXT_RENDERER_GPU", "text.renderer.gpu");
    return enabled;
}

QString alignmentPanelStyleSignature(const OverlayStyle& style) {
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10")
        .arg(style.cornerRadius)
        .arg(style.paddingX)
        .arg(style.paddingY)
        .arg(style.gap)
        .arg(style.itemSpacing)
        .arg(style.defaultHeight)
        .arg(style.maxWidth)
        .arg(style.backgroundColor.rgba())
        .arg(style.activeBackgroundColor.rgba())
        .arg(style.textColor.rgba());
}

struct TextPerfStats {
    quint64 started = 0;
    quint64 completed = 0;
    quint64 dropped = 0;
    quint64 stale = 0;
    quint64 queueDepthPeak = 0;
    qint64 totalDurationMs = 0;
    QVector<qint64> recentDurationsMs;
    QElapsedTimer window;
};

TextPerfStats& textPerfStats() {
    static TextPerfStats stats;
    return stats;
}

void recordTextRasterStart(quint64 queueDepth) {
    if (!textProfilingEnabled()) {
        return;
    }

    TextPerfStats& stats = textPerfStats();
    ++stats.started;
    stats.queueDepthPeak = std::max(stats.queueDepthPeak, queueDepth);
    if (!stats.window.isValid()) {
        stats.window.start();
    }
}

void recordTextRasterResult(qint64 durationMs, bool stale, bool dropped) {
    if (!textProfilingEnabled()) {
        return;
    }

    TextPerfStats& stats = textPerfStats();
    if (stale) {
        ++stats.stale;
    }
    if (dropped) {
        ++stats.dropped;
    }
    if (!stale && !dropped) {
        ++stats.completed;
        stats.totalDurationMs += std::max<qint64>(0, durationMs);
        stats.recentDurationsMs.append(std::max<qint64>(0, durationMs));
        if (stats.recentDurationsMs.size() > 256) {
            stats.recentDurationsMs.remove(0, stats.recentDurationsMs.size() - 256);
        }
    }

    if (!stats.window.isValid() || stats.window.elapsed() < 1000) {
        return;
    }

    qint64 p95 = 0;
    if (!stats.recentDurationsMs.isEmpty()) {
        QVector<qint64> sorted = stats.recentDurationsMs;
        std::sort(sorted.begin(), sorted.end());
        const int upperBound = static_cast<int>(sorted.size()) - 1;
        const int candidateIdx = static_cast<int>(std::ceil(static_cast<double>(upperBound) * 0.95));
        const int idx = std::clamp(candidateIdx, 0, upperBound);
        p95 = sorted.at(idx);
    }

    const qint64 avg = (stats.completed > 0)
        ? static_cast<qint64>(std::llround(static_cast<double>(stats.totalDurationMs) / static_cast<double>(stats.completed)))
        : 0;

    qInfo() << "[TextPerf]"
            << "started" << stats.started
            << "completed" << stats.completed
            << "dropped" << stats.dropped
            << "stale" << stats.stale
            << "queuePeak" << stats.queueDepthPeak
            << "avgMs" << avg
            << "p95Ms" << p95;

    stats.started = 0;
    stats.completed = 0;
    stats.dropped = 0;
    stats.stale = 0;
    stats.queueDepthPeak = 0;
    stats.totalDurationMs = 0;
    stats.recentDurationsMs.clear();
    stats.window.restart();
}

struct CssWeightMapping {
    int cssWeight;
    QFont::Weight qtWeight;
};

constexpr std::array<CssWeightMapping, 9> kWeightMappings = {
    CssWeightMapping{100, QFont::Thin},
    CssWeightMapping{200, QFont::ExtraLight},
    CssWeightMapping{300, QFont::Light},
    CssWeightMapping{400, QFont::Normal},
    CssWeightMapping{500, QFont::Medium},
    CssWeightMapping{600, QFont::DemiBold},
    CssWeightMapping{700, QFont::Bold},
    CssWeightMapping{800, QFont::ExtraBold},
    CssWeightMapping{900, QFont::Black},
};

int clampCssWeight(int weight) {
    const int clamped = std::clamp(weight, 100, 900);
    const int rounded = ((clamped + 50) / 100) * 100;
    return std::clamp(rounded, 100, 900);
}

QFont::Weight cssWeightToQtWeight(int cssWeight) {
    const int normalized = clampCssWeight(cssWeight);
    for (const auto& mapping : kWeightMappings) {
        if (mapping.cssWeight == normalized) {
            return mapping.qtWeight;
        }
    }
    return QFont::Normal;
}

int qtWeightToCssWeight(QFont::Weight weight) {
    int bestCss = 400;
    int bestDelta = std::numeric_limits<int>::max();
    for (const auto& mapping : kWeightMappings) {
        const int delta = std::abs(static_cast<int>(weight) - static_cast<int>(mapping.qtWeight));
        if (delta < bestDelta) {
            bestDelta = delta;
            bestCss = mapping.cssWeight;
        }
    }
    return bestCss;
}

int canonicalCssWeight(const QFont& font) {
    return qtWeightToCssWeight(font.weight());
}

QFont fontAdjustedForWeight(QFont font, int cssWeight) {
    font.setWeight(cssWeightToQtWeight(cssWeight));
    return font;
}

QString previewTextForLog(const QString& text, int maxLen = 120) {
    if (text.isEmpty()) {
        return QStringLiteral("<empty>");
    }

    QString sanitized = text;
    sanitized.replace('\n', QLatin1Char(' '));
    sanitized.replace('\r', QLatin1Char(' '));
    sanitized = sanitized.simplified();
    if (sanitized.size() > maxLen) {
        return sanitized.left(maxLen) + QStringLiteral("... (%1 chars)").arg(sanitized.size());
    }
    return sanitized;
}

qreal computeStrokeWidthFromFont(const QFont& font, qreal widthPercent) {
    if (widthPercent <= 0.0) {
        return 0.0;
    }

    QFontMetricsF metrics(font);
    qreal reference = metrics.height();
    if (reference <= 0.0) {
        if (font.pixelSize() > 0) {
            reference = static_cast<qreal>(font.pixelSize());
        } else {
            reference = font.pointSizeF();
        }
    }
    if (reference <= 0.0) {
        reference = 16.0;
    }

    const qreal normalized = std::clamp(widthPercent / 100.0, 0.0, 1.0);
    const qreal eased = std::pow(normalized, 1.1);
    return eased * kMaxOutlineThicknessFactor * reference;
}

QRectF computeDocumentTextBounds(const QTextDocument& doc, QAbstractTextDocumentLayout* layout) {
    if (!layout) {
        return QRectF();
    }

    QRectF bounds;
    bool hasBounds = false;

    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        QTextLayout* textLayout = block.layout();
        if (!textLayout) {
            continue;
        }

        const QRectF blockRect = layout->blockBoundingRect(block);
        for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
            QTextLine line = textLayout->lineAt(lineIndex);
            if (!line.isValid()) {
                continue;
            }

            QRectF lineRect = line.naturalTextRect();
            if (lineRect.isEmpty()) {
                lineRect = QRectF(0.0, 0.0,
                                  std::max<qreal>(line.naturalTextWidth(), 1.0),
                                  std::max<qreal>(line.height(), 1.0));
            }

            const QPointF lineOrigin(blockRect.left() + line.x(), blockRect.top() + line.y());
            const QRectF translated = lineRect.translated(lineOrigin);
            bounds = hasBounds ? bounds.united(translated) : translated;
            hasBounds = true;
        }
    }

    if (!hasBounds) {
        const QSizeF fallbackSize = layout->documentSize();
        return QRectF(0.0, 0.0,
                      std::max<qreal>(fallbackSize.width(), 1.0),
                      std::max<qreal>(fallbackSize.height(), 1.0));
    }

    return bounds;
}

QFont ensureRenderableFont(QFont font, const QString& itemId, const char* callerTag) {
    const int pixelSize = font.pixelSize();
    const qreal pointSize = font.pointSizeF();
    if (pixelSize <= 0 && pointSize <= 0.0) {
        qWarning() << "[TextMedia][FontWarning]" << callerTag
                   << "invalid size detected for item" << itemId
                   << "pixelSize" << pixelSize
                   << "pointSize" << pointSize
                   << "- clamping to" << kFallbackFontPixelSize << "px";
        font.setPixelSize(kFallbackFontPixelSize);
    }
    return font;
}

void logStrokeDiagnostics(const char* context,
                          qreal strokePercent,
                          qreal strokeWidthPx,
                          int glyphCount,
                          qint64 outlineBuildMs,
                          qint64 totalMs,
                          const QSizeF& docSize,
                          const QSize& targetSize,
                          qreal scaleFactor,
                          const QString& previewText) {
    if (!textHotLogsEnabled()) {
        return;
    }

    qDebug() << "[TextMediaItem][Stroke]"
             << context
             << "percent" << strokePercent
             << "px" << strokeWidthPx
             << "glyphs" << glyphCount
             << "outlineMs" << outlineBuildMs
             << "totalMs" << totalMs
             << "docSize" << docSize
             << "targetPx" << targetSize
             << "scale" << scaleFactor
             << "preview" << previewText;
}

quint64 makeGlyphCacheKey(const QRawFont& rawFont, quint32 glyphIndex) {
    QString descriptor;
    descriptor.reserve(80);
    descriptor.append(rawFont.familyName());
    descriptor.append(QLatin1Char('|'));
    descriptor.append(rawFont.styleName());
    descriptor.append(QLatin1Char('|'));
    descriptor.append(QString::number(rawFont.pixelSize()));
    descriptor.append(QLatin1Char(':'));
    descriptor.append(QString::number(static_cast<quint64>(glyphIndex)));
    return static_cast<quint64>(qHash(descriptor));
}

QPainterPath cachedGlyphPath(const QRawFont& rawFont, quint32 glyphIndex) {
    if (!rawFont.isValid()) {
        return QPainterPath();
    }

    static QCache<quint64, QPainterPath> s_glyphCache(kMaxCachedGlyphPaths);
    static QMutex s_cacheMutex;

    const quint64 combinedKey = makeGlyphCacheKey(rawFont, glyphIndex);

    {
        QMutexLocker locker(&s_cacheMutex);
        if (const QPainterPath* cached = s_glyphCache.object(combinedKey)) {
            return *cached;
        }
    }

    QPainterPath path = rawFont.pathForGlyph(glyphIndex);
    if (path.isEmpty()) {
        return path;
    }

    {
        QMutexLocker locker(&s_cacheMutex);
        if (!s_glyphCache.object(combinedKey)) {
            s_glyphCache.insert(combinedKey, new QPainterPath(path), 1);
        }
    }

    return path;
}

// Phase 2: Rendered Glyph Cache - caches pre-rendered glyphs with stroke+fill applied
struct RenderedGlyphKey {
    quint64 outlinePathKey;      // Reuse outline cache key
    QRgb fillColor;              // Packed ARGB
    QRgb strokeColor;            // Packed ARGB
    qint32 strokeWidthScaled;    // Fixed-point: width * 1024
    qint32 scaleFactorScaled;    // Fixed-point: scale * 256 (coarser for better cache hits)
    
    bool operator==(const RenderedGlyphKey& other) const {
        return outlinePathKey == other.outlinePathKey &&
               fillColor == other.fillColor &&
               strokeColor == other.strokeColor &&
               strokeWidthScaled == other.strokeWidthScaled &&
               scaleFactorScaled == other.scaleFactorScaled;
    }
};

inline uint qHash(const RenderedGlyphKey& key, uint seed = 0) {
    seed = ::qHash(key.outlinePathKey, seed);
    seed = ::qHash(key.fillColor, seed);
    seed = ::qHash(key.strokeColor, seed);
    seed = ::qHash(key.strokeWidthScaled, seed);
    return ::qHash(key.scaleFactorScaled, seed);
}

struct RenderedGlyphBitmap {
    QPixmap pixmap;
    QPointF originOffset;
};

RenderedGlyphKey makeRenderedGlyphKey(const QRawFont& rawFont, quint32 glyphIndex,
                                      const QColor& fillColor, const QColor& strokeColor,
                                      qreal strokeWidth, qreal scaleFactor) {
    RenderedGlyphKey key;
    key.outlinePathKey = makeGlyphCacheKey(rawFont, glyphIndex);
    key.fillColor = fillColor.rgba();
    key.strokeColor = strokeColor.rgba();
    key.strokeWidthScaled = static_cast<qint32>(std::round(strokeWidth * 1024.0));
    // Quantize scale to 1/256 increments for better cache hits
    key.scaleFactorScaled = static_cast<qint32>(std::round(scaleFactor * 256.0));
    return key;
}

RenderedGlyphBitmap renderGlyphToPixmap(const QPainterPath& glyphPath, const QColor& fillColor,
                                        const QColor& strokeColor, qreal strokeWidth) {
    RenderedGlyphBitmap result;
    if (glyphPath.isEmpty()) {
        return result;
    }
    
    // Calculate tight bounds with stroke overflow
    const QRectF pathBounds = glyphPath.boundingRect();
    const qreal padding = std::ceil(strokeWidth * 2.0) + 2.0;
    const QRectF renderBounds = pathBounds.adjusted(-padding, -padding, padding, padding);
    
    const int width = std::max(1, static_cast<int>(std::ceil(renderBounds.width())));
    const int height = std::max(1, static_cast<int>(std::ceil(renderBounds.height())));
    
    QImage img(width, height, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(-renderBounds.left(), -renderBounds.top());
    
    // Draw stroke
    if (strokeWidth > 0.0) {
        p.setPen(QPen(strokeColor, strokeWidth * 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(glyphPath);
    }
    
    // Draw fill
    p.setPen(Qt::NoPen);
    p.setBrush(fillColor);
    p.drawPath(glyphPath);
    
    p.end();
    
    result.pixmap = QPixmap::fromImage(img);
    result.originOffset = renderBounds.topLeft();
    return result;
}

    RenderedGlyphBitmap cachedRenderedGlyph(const QRawFont& rawFont, quint32 glyphIndex,
                                            const QColor& fillColor, const QColor& strokeColor,
                                            qreal strokeWidth, qreal scaleFactor) {
        RenderedGlyphBitmap result;
        if (!rawFont.isValid() || strokeWidth < 0.0) {
            return result;
        }
    
        static QCache<RenderedGlyphKey, RenderedGlyphBitmap> s_renderedGlyphCache(kMaxRenderedGlyphPixmaps);
        static QMutex s_renderedCacheMutex;
    
        const RenderedGlyphKey cacheKey = makeRenderedGlyphKey(rawFont, glyphIndex, fillColor, strokeColor, strokeWidth, scaleFactor);
    
        {
            QMutexLocker locker(&s_renderedCacheMutex);
            if (const RenderedGlyphBitmap* cached = s_renderedGlyphCache.object(cacheKey)) {
                return *cached;
            }
        }
    
        // Cache miss - render the glyph
        const QPainterPath glyphPath = cachedGlyphPath(rawFont, glyphIndex);
        if (glyphPath.isEmpty()) {
            return result;
        }
    
        RenderedGlyphBitmap rendered = renderGlyphToPixmap(glyphPath, fillColor, strokeColor, strokeWidth);
        if (rendered.pixmap.isNull()) {
            return rendered;
        }
    
        {
            QMutexLocker locker(&s_renderedCacheMutex);
            if (!s_renderedGlyphCache.object(cacheKey)) {
                // Cost based on pixmap size in KB
                const int costKB = (rendered.pixmap.width() * rendered.pixmap.height() * 4) / 1024;
                s_renderedGlyphCache.insert(cacheKey, new RenderedGlyphBitmap(rendered), std::max(1, costKB));
            }
        }
    
        return rendered;
    }

} // anonymous namespace

namespace {

class InlineTextEditor : public QGraphicsTextItem {
public:
    explicit InlineTextEditor(TextMediaItem* owner)
        : QGraphicsTextItem(owner)
        , m_owner(owner) {
        setAcceptHoverEvents(false);
        setFlag(QGraphicsItem::ItemIsSelectable, false);
        setFlag(QGraphicsItem::ItemIsFocusable, true);
        setTextInteractionFlags(Qt::NoTextInteraction);

        m_caretBlinkTimer.setSingleShot(false);
        m_caretBlinkTimer.setTimerType(Qt::CoarseTimer);
        const int interval = caretBlinkInterval();
        if (interval > 0) {
            m_caretBlinkTimer.setInterval(interval);
        }
        QObject::connect(&m_caretBlinkTimer, &QTimer::timeout, this, [this]() {
            m_caretVisible = !m_caretVisible;
            update();
        });
    }

    void invalidateCache(bool resetCaret = false) {
        m_cacheDirty = true;
        if (resetCaret) {
            startCaretBlink(true);
        }
        update();
    }

    void disableCaretBlink() {
        stopCaretBlink(true);
    }

protected:
    void focusInEvent(QFocusEvent* event) override {
        QGraphicsTextItem::focusInEvent(event);
        startCaretBlink(false);
    }

    void focusOutEvent(QFocusEvent* event) override {
        QGraphicsTextItem::focusOutEvent(event);

        if (!m_owner) {
            stopCaretBlink(true);
            return;
        }

        const Qt::FocusReason reason = event ? event->reason() : Qt::OtherFocusReason;

        if (!m_owner->isEditing()) {
            stopCaretBlink(true);
            return;
        }

        if (reason == Qt::MouseFocusReason && QApplication::mouseButtons() != Qt::NoButton) {
            stopCaretBlink(true);
            m_owner->commitInlineEditing();
            return;
        }

        // Keep caret blinking while we regain focus asynchronously
        QTimer::singleShot(0, this, [this]() {
            if (m_owner && m_owner->isEditing()) {
                this->setFocus(Qt::OtherFocusReason);
            }
        });
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (!m_owner) {
            QGraphicsTextItem::keyPressEvent(event);
            invalidateCache();
            return;
        }

        if (event->key() == Qt::Key_Escape) {
            m_owner->cancelInlineEditing();
            event->accept();
            return;
        }

        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            (event->modifiers() & Qt::ControlModifier)) {
            m_owner->commitInlineEditing();
            event->accept();
            invalidateCache();
            return;
        }

        // Intercept paste to strip formatting
        if (event->matches(QKeySequence::Paste)) {
            QClipboard* clipboard = QApplication::clipboard();
            if (clipboard) {
                QString plainText = clipboard->text();
                if (!plainText.isEmpty()) {
                    textCursor().insertText(plainText);
                    invalidateCache(true);
                    event->accept();
                    return;
                }
            }
        }

        QGraphicsTextItem::keyPressEvent(event);
        invalidateCache(true);
        
        // After any text insertion, normalize formatting
        if (m_owner) {
            QTimer::singleShot(0, this, [this]() {
                if (m_owner) {
                    m_owner->normalizeEditorFormatting();
                }
            });
        }
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override {
        if (!painter) {
            return;
        }

        QStyleOptionGraphicsItem opt(*option);
        opt.state &= ~QStyle::State_HasFocus;

        const QRectF bounds = boundingRect();

        const bool skipDocRendering = (m_owner && m_owner->isEditing());

        if (!skipDocRendering) {
            const QTransform world = painter->worldTransform();
            qreal scaleX = std::hypot(world.m11(), world.m21());
            qreal scaleY = std::hypot(world.m22(), world.m12());
            if (scaleX <= 1e-6) scaleX = 1.0;
            if (scaleY <= 1e-6) scaleY = 1.0;

            qreal dpr = 1.0;
            if (QPaintDevice* device = painter->device()) {
                dpr = std::max<qreal>(1.0, device->devicePixelRatioF());
            }

            const qreal renderScale = std::max<qreal>(1.0, std::max(scaleX, scaleY) * dpr);
            const int maxDimension = 8192;
            const int width = std::clamp(static_cast<int>(std::ceil(bounds.width() * renderScale)), 1, maxDimension);
            const int height = std::clamp(static_cast<int>(std::ceil(bounds.height() * renderScale)), 1, maxDimension);
            const bool renderScaleChanged = std::abs(renderScale - m_cachedRenderScale) > 0.02;

            if (m_cacheDirty || renderScaleChanged || m_cachedImage.size() != QSize(width, height)) {
                m_cachedImage = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
                m_cachedImage.fill(Qt::transparent);

                QPainter bufferPainter(&m_cachedImage);
                bufferPainter.setRenderHint(QPainter::Antialiasing, true);
                bufferPainter.setRenderHint(QPainter::TextAntialiasing, true);
                bufferPainter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                bufferPainter.scale(renderScale, renderScale);
                bufferPainter.translate(-bounds.topLeft());
                // Render only the base text content; caret and selection are drawn as overlays
                if (QTextDocument* doc = document()) {
                    const QColor fillColor = m_owner ? m_owner->textColor() : defaultTextColor();
                    if (m_owner && m_owner->highlightEnabled()) {
                        QColor highlight = m_owner->highlightColor();
                        if (!highlight.isValid()) {
                            highlight = TextMediaDefaults::TEXT_HIGHLIGHT_COLOR;
                        }
                        if (highlight.alpha() > 0) {
                            bufferPainter.save();
                            bufferPainter.setPen(Qt::NoPen);
                            bufferPainter.setBrush(highlight);
                            
                            QAbstractTextDocumentLayout* docLayout = doc->documentLayout();
                            if (docLayout) {
                                // Draw highlight per-line, respecting document alignment
                                const qreal docWidth = std::max<qreal>(docLayout->documentSize().width(), 1.0);
                                const Qt::Alignment align = doc->defaultTextOption().alignment();
                                for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
                                    QTextLayout* textLayout = block.layout();
                                    if (!textLayout) continue;
                                    
                                    const QRectF blockRect = docLayout->blockBoundingRect(block);
                                    for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
                                        QTextLine line = textLayout->lineAt(lineIndex);
                                        if (!line.isValid()) continue;
                                        
                                        const qreal lineWidth = line.naturalTextWidth();
                                        const qreal width = std::max<qreal>(lineWidth, 1.0);
                                        qreal alignedX = line.x();
                                        if (std::abs(alignedX) < 1e-4 && width < docWidth - 1e-4) {
                                            const qreal horizontalSpace = std::max<qreal>(0.0, docWidth - width);
                                            if (align.testFlag(Qt::AlignRight)) {
                                                alignedX += horizontalSpace;
                                            } else if (align.testFlag(Qt::AlignHCenter)) {
                                                alignedX += horizontalSpace * 0.5;
                                            }
                                        }
                                        const qreal height = std::max<qreal>(line.height(), 1.0);
                                        // line.x() already contains the alignment offset within the document
                                        const QPointF topLeft = blockRect.topLeft() + QPointF(alignedX, line.y());
                                        bufferPainter.drawRect(QRectF(topLeft, QSizeF(width, height)));
                                    }
                                }
                            }
                            
                            bufferPainter.restore();
                        }
                    }
                    QColor outlineColor = fillColor;
                    if (m_owner) {
                        const QColor ownerOutline = m_owner->textBorderColor();
                        if (ownerOutline.isValid()) {
                            outlineColor = ownerOutline;
                        }
                    }

                    const QFont ownerFont = m_owner ? m_owner->font() : font();
                    const qreal strokeWidth = m_owner ? computeStrokeWidthFromFont(ownerFont, m_owner->textBorderWidth()) : 0.0;

                    // Clear outline formatting
                    {
                        QTextCursor cursor(doc);
                        cursor.select(QTextCursor::Document);
                        QTextCharFormat format;
                        format.setForeground(fillColor);
                        format.clearProperty(QTextFormat::TextOutline);
                        cursor.mergeCharFormat(format);
                    }

                    if (strokeWidth > 0.0) {
                        // Phase 2: Strict two-pass rendering in InlineTextEditor
                        QAbstractTextDocumentLayout* docLayout = doc->documentLayout();
                        QElapsedTimer strokeTimer;
                        strokeTimer.start();
                        int glyphCount = 0;
                        
                        // Pass 1: Render all strokes first (background)
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
                                        const QPointF glyphPos = blockRect.topLeft() + positions[gi];
                                        const QPainterPath glyphPath = cachedGlyphPath(rawFont, indexes[gi]);                                        // Draw stroke for ALL glyphs
                                        bufferPainter.save();
                                        bufferPainter.translate(glyphPos);
                                        bufferPainter.setPen(QPen(outlineColor, strokeWidth * 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                                        bufferPainter.setBrush(Qt::NoBrush);
                                        bufferPainter.drawPath(glyphPath);
                                        bufferPainter.restore();
                                    }
                                    glyphCount += indexes.size();
                                }
                            }
                        }
                        
                        // Pass 2: Render all fills on top (foreground)
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
                                        const QPointF glyphPos = blockRect.topLeft() + positions[gi];
                                        const QPainterPath glyphPath = cachedGlyphPath(rawFont, indexes[gi]);                                        // Draw fill for ALL glyphs
                                        bufferPainter.save();
                                        bufferPainter.translate(glyphPos);
                                        bufferPainter.setPen(Qt::NoPen);
                                        bufferPainter.setBrush(fillColor);
                                        bufferPainter.drawPath(glyphPath);
                                        bufferPainter.restore();
                                    }
                                }
                            }
                        }
                        
                        const qint64 outlineMs = strokeTimer.elapsed();
                        logStrokeDiagnostics("InlineEditorStroke",
                                              m_owner ? m_owner->textBorderWidth() : 0.0,
                                              strokeWidth,
                                              glyphCount,
                                              outlineMs,
                                              outlineMs,
                                              docLayout ? docLayout->documentSize() : QSizeF(),
                                              QSize(width, height),
                                              1.0,
                                              previewTextForLog(doc->toPlainText()));
                    } else {
                        QAbstractTextDocumentLayout::PaintContext ctx;
                        ctx.cursorPosition = -1;
                        ctx.palette.setColor(QPalette::Text, fillColor);
                        doc->documentLayout()->draw(&bufferPainter, ctx);
                    }
                } else {
                    QGraphicsTextItem::paint(&bufferPainter, &opt, widget);
                }
                bufferPainter.end();

                m_cachedRenderScale = renderScale;
                m_cacheDirty = false;
            }

            const QRectF sourceRect(0.0, 0.0, static_cast<qreal>(m_cachedImage.width()), static_cast<qreal>(m_cachedImage.height()));
            painter->drawImage(bounds, m_cachedImage, sourceRect);
        }

        // Draw active selection highlight as an overlay so it stays responsive to cursor movement
        drawSelectionOverlay(painter, bounds);

        const bool editing = (m_owner && m_owner->isEditing());

        // Draw the caret on top using a high-contrast vector rectangle so it stays sharp
        if (editing && (textInteractionFlags() & Qt::TextEditable) && m_caretVisible) {
            QTextCursor cursor = textCursor();
            if (!cursor.hasSelection()) {
                const QRectF caretRect = cursorRectForPosition(cursor).translated(bounds.topLeft());
                if (!caretRect.isEmpty()) {
                    const qreal desiredSceneWidth = 3.0; // constant thickness in canvas pixels
                    const QTransform world = painter->worldTransform();
                    qreal scaleX = std::hypot(world.m11(), world.m21());
                    if (scaleX <= 1e-6) {
                        scaleX = 1.0;
                    }
                    const qreal caretHalfWidth = (desiredSceneWidth / scaleX) * 0.5;
                    QRectF adjustedCaretRect = caretRect;
                    const qreal caretCenterX = adjustedCaretRect.center().x();
                    adjustedCaretRect.setLeft(caretCenterX - caretHalfWidth);
                    adjustedCaretRect.setRight(caretCenterX + caretHalfWidth);

                    painter->save();
                    painter->setRenderHint(QPainter::Antialiasing, false);
                    painter->setRenderHint(QPainter::TextAntialiasing, false);
                    painter->setPen(Qt::NoPen);

                    QColor caretColor = defaultTextColor();
                    caretColor.setAlpha(255);

                    // Add a subtle outline that contrasts with the text color for better visibility
                    const int luminance = qGray(caretColor.rgb());
                    QColor outlineColor = luminance > 128 ? QColor(0, 0, 0, 160) : QColor(255, 255, 255, 160);

                    const qreal outlineInset = 1.0 / scaleX;
                    QRectF outlineRect = adjustedCaretRect.adjusted(-outlineInset, 0.0, outlineInset, 0.0);
                    painter->fillRect(outlineRect, outlineColor);
                    painter->fillRect(adjustedCaretRect, caretColor);

                    painter->restore();
                }
            }
        }
    }

    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        QGraphicsTextItem::mousePressEvent(event);
        invalidateCache(true);
    }

    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override {
        if (event->buttons() == Qt::NoButton) {
            event->ignore();
            return;
        }
        // Store cursor position before event
        int oldPosition = textCursor().position();
        bool hadSelection = textCursor().hasSelection();
        
        QGraphicsTextItem::mouseMoveEvent(event);
        
        // Only invalidate if cursor position or selection changed
        int newPosition = textCursor().position();
        bool hasSelection = textCursor().hasSelection();
        
        if (oldPosition != newPosition || hadSelection != hasSelection) {
            invalidateCache(false);
        }
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        QGraphicsTextItem::mouseReleaseEvent(event);
        invalidateCache(false);
    }

private:
    void startCaretBlink(bool resetVisible) {
        const int interval = caretBlinkInterval();
        if (resetVisible || !m_caretBlinkTimer.isActive()) {
            m_caretVisible = true;
        }

        if (interval > 0) {
            if (m_caretBlinkTimer.interval() != interval) {
                m_caretBlinkTimer.setInterval(interval);
            }

            if (!m_caretBlinkTimer.isActive()) {
                m_caretBlinkTimer.start();
            } else if (resetVisible) {
                m_caretBlinkTimer.start();
            }
        } else {
            m_caretBlinkTimer.stop();
        }

        update();
    }

    void stopCaretBlink(bool resetVisible) {
        if (m_caretBlinkTimer.isActive()) {
            m_caretBlinkTimer.stop();
        }

        if (resetVisible) {
            m_caretVisible = true;
        }

        update();
    }

    int caretBlinkInterval() const {
        const int flashTime = QApplication::cursorFlashTime();
        if (flashTime <= 0) {
            return 0;
        }
        return std::max(100, flashTime / 2);
    }

    QRectF cursorRectForPosition(const QTextCursor& cursor) const {
        QTextDocument* doc = document();
        if (!doc) {
            return QRectF();
        }

        QTextBlock block = cursor.block();
        QTextLayout* layout = block.layout();
        if (!layout) {
            return QRectF();
        }

        const int posInBlock = cursor.position() - block.position();
        QTextLine line = layout->lineForTextPosition(posInBlock);
        if (!line.isValid()) {
            return QRectF();
        }

        const qreal caretWidth = 1.0;
        const qreal x = line.cursorToX(posInBlock);
        const qreal y = line.y();
        const qreal height = line.height();

        const QPointF blockOrigin = doc->documentLayout()->blockBoundingRect(block).topLeft();
        const qreal left = blockOrigin.x() + x - (caretWidth * 0.5);
        const qreal alignedLeft = std::round(left);

        return QRectF(alignedLeft, blockOrigin.y() + y, caretWidth, height);
    }

    void drawSelectionOverlay(QPainter* painter, const QRectF& bounds) {
        if (!painter) {
            return;
        }

        if (!(textInteractionFlags() & Qt::TextEditable)) {
            return;
        }

        if (!m_owner || !m_owner->isEditing()) {
            return;
        }

        QTextCursor cursor = textCursor();
        if (!cursor.hasSelection()) {
            return;
        }

        QTextDocument* doc = document();
        if (!doc) {
            return;
        }

        const int selectionStart = cursor.selectionStart();
        const int selectionEnd = cursor.selectionEnd();
        if (selectionStart == selectionEnd) {
            return;
        }

        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setRenderHint(QPainter::Antialiasing, false);
        painter->setRenderHint(QPainter::TextAntialiasing, false);

        const QColor highlightColor(100, 149, 237, 128);
        const QPointF offset = bounds.topLeft();

        QTextBlock block = doc->findBlock(selectionStart);
        while (block.isValid() && block.position() < selectionEnd) {
            QTextLayout* layout = block.layout();
            if (!layout) {
                block = block.next();
                continue;
            }

            const int blockStart = block.position();
            const int blockEnd = blockStart + block.length();
            const int rangeStart = std::max(selectionStart, blockStart);
            const int rangeEnd = std::min(selectionEnd, blockEnd);
            if (rangeStart >= rangeEnd) {
                block = block.next();
                continue;
            }

            const QPointF blockPos = doc->documentLayout()->blockBoundingRect(block).topLeft();

            for (int i = 0; i < layout->lineCount(); ++i) {
                QTextLine line = layout->lineAt(i);
                if (!line.isValid()) {
                    continue;
                }

                const int lineStart = blockStart + line.textStart();
                const int lineEnd = lineStart + line.textLength();
                if (lineEnd <= rangeStart) {
                    continue;
                }
                if (lineStart >= rangeEnd) {
                    break;
                }

                const int selStart = std::max(rangeStart, lineStart);
                int selEnd = std::min(rangeEnd, lineEnd);

                // Include visible newline selection by extending to line width
                if (selStart == selEnd && lineEnd < rangeEnd) {
                    selEnd = rangeEnd;
                }

                const qreal x1 = line.cursorToX(selStart - blockStart);
                qreal x2 = line.cursorToX(selEnd - blockStart);
                if (std::abs(x2 - x1) < 0.5) {
                    // Fallback to cover at least a thin caret width
                    x2 = x1 + 2.0;
                }

                const QPointF topLeft = offset + blockPos + QPointF(std::min(x1, x2), line.y());
                const qreal width = std::max(std::abs(x2 - x1), 1.0);
                const QRectF highlightRect(topLeft, QSizeF(width, line.height()));
                painter->fillRect(highlightRect, highlightColor);
            }

            if (blockEnd >= selectionEnd) {
                break;
            }
            block = block.next();
        }

        painter->restore();
    }

    TextMediaItem* m_owner = nullptr;
    mutable QImage m_cachedImage;
    mutable qreal m_cachedRenderScale = 0.0;
    mutable bool m_cacheDirty = true;
    QTimer m_caretBlinkTimer;
    bool m_caretVisible = true;
};


static void normalizeTextFormatting(
    QGraphicsTextItem* editor,
    const QFont& currentFont,
    const QColor& currentColor,
    const QColor& currentOutlineColor,
    qreal currentOutlineWidth
) {
    if (!editor || !editor->document()) {
        return;
    }

    QTextDocument* doc = editor->document();

    // Preserve the user's selection/cursor before normalizing
    QTextCursor restoreCursor = editor->textCursor();

    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);

    // Create standard character format using the current font settings
    // Note: We clear TextOutline because the inline editor uses glyph paths for outside strokes
    QTextCharFormat standardFormat;
    standardFormat.setFont(currentFont);
    standardFormat.setForeground(QBrush(currentColor));
    standardFormat.clearProperty(QTextFormat::TextOutline);

    // Apply to all text
    cursor.mergeCharFormat(standardFormat);
    cursor.endEditBlock();

    editor->setTextCursor(restoreCursor);
}

static void applyTextAlignment(QGraphicsTextItem* editor, Qt::Alignment alignment) {
    if (!editor) {
        return;
    }

    if (QTextDocument* doc = editor->document()) {
        QTextCursor activeCursor = editor->textCursor();
        QTextCursor cursor(doc);
        cursor.select(QTextCursor::Document);
        QTextBlockFormat blockFormat = cursor.blockFormat();
        blockFormat.setAlignment(alignment);
        cursor.mergeBlockFormat(blockFormat);
        cursor.clearSelection();
        editor->setTextCursor(activeCursor);
    }
}

static void applyCenterAlignment(QGraphicsTextItem* editor) {
    applyTextAlignment(editor, Qt::AlignHCenter);
}

static InlineTextEditor* toInlineEditor(QGraphicsTextItem* item) {
    return item ? dynamic_cast<InlineTextEditor*>(item) : nullptr;
}

} // anonymous namespace (InlineTextEditor and helpers)

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// TIER 2 OPTIMIZATION IMPLEMENTATIONS
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Phase 6: Glyph Layout Pre-computation
quint64 TextMediaItem::computeLayoutFingerprint(const QString& text, const QFont& font, qreal width) const {
    // Generate stable hash from text content + font properties + layout width
    quint64 h = qHash(text);
    h ^= qHash(font.family()) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= static_cast<quint64>(font.pointSizeF() * 1000.0) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= static_cast<quint64>(font.weight()) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= static_cast<quint64>(font.italic() ? 1 : 0) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= static_cast<quint64>(width * 100.0) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

void TextMediaItem::updateGlyphLayoutCache(const VectorDrawSnapshot& snapshot, const QSize& targetSize) {
    // Phase 6: Simplified - just track fingerprint for future optimization
    // Full glyph position caching deferred to avoid QRawFont storage complexity
    if (snapshot.outlineWidthPercent <= 0.0) {
        m_glyphLayoutCache.valid = false;
        return;
    }
    
    const quint64 fingerprint = computeLayoutFingerprint(snapshot.text, snapshot.font, static_cast<qreal>(targetSize.width()));
    if (m_glyphLayoutCache.valid && m_glyphLayoutCache.fingerprint == fingerprint) {
        return; // Already cached
    }
    
    m_glyphLayoutCache.clear();
    m_glyphLayoutCache.fingerprint = fingerprint;
    m_glyphLayoutCache.valid = true;
}

bool TextMediaItem::isGlyphLayoutCacheValid(const VectorDrawSnapshot& snapshot, const QSize& targetSize) const {
    if (!m_glyphLayoutCache.valid) return false;
    const quint64 fingerprint = computeLayoutFingerprint(snapshot.text, snapshot.font, static_cast<qreal>(targetSize.width()));
    return m_glyphLayoutCache.fingerprint == fingerprint;
}

// Phase 7: Viewport Culling
void TextMediaItem::updateBlockVisibilityCache(const QRectF& viewport, QTextDocument& doc, QAbstractTextDocumentLayout* layout) {
    if (!layout) {
        m_blockVisibilityCacheValid = false;
        return;
    }
    
    // Skip update if viewport unchanged
    if (m_blockVisibilityCacheValid && m_lastCullingViewport == viewport) {
        return;
    }
    
    m_blockVisibilityCache.clear();
    m_lastCullingViewport = viewport;
    
    int blockIdx = 0;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next(), ++blockIdx) {
        const QRectF blockBounds = layout->blockBoundingRect(block);
        
        BlockVisibility vis;
        vis.blockIndex = blockIdx;
        vis.blockBounds = blockBounds;
        
        if (viewport.isEmpty() || !viewport.isValid()) {
            // No viewport culling - all blocks visible
            vis.fullyVisible = true;
            vis.fullyInvisible = false;
            vis.partiallyVisible = false;
        } else if (viewport.contains(blockBounds)) {
            // Block completely inside viewport
            vis.fullyVisible = true;
            vis.fullyInvisible = false;
            vis.partiallyVisible = false;
        } else if (!viewport.intersects(blockBounds)) {
            // Block completely outside viewport
            vis.fullyVisible = false;
            vis.fullyInvisible = true;
            vis.partiallyVisible = false;
        } else {
            // Block partially visible
            vis.fullyVisible = false;
            vis.fullyInvisible = false;
            vis.partiallyVisible = true;
            vis.visibleIntersection = viewport.intersected(blockBounds);
        }
        
        m_blockVisibilityCache.append(vis);
    }
    
    m_blockVisibilityCacheValid = true;
}

bool TextMediaItem::shouldSkipBlock(int blockIndex) const {
    if (!m_blockVisibilityCacheValid || blockIndex < 0 || blockIndex >= m_blockVisibilityCache.size()) {
        return false; // Conservative: render if cache invalid
    }
    return m_blockVisibilityCache[blockIndex].fullyInvisible;
}

// Phase 8: Stroke Quality LOD
TextMediaItem::StrokeQuality TextMediaItem::selectStrokeQuality(qreal canvasZoom) const {
    // Hysteresis to prevent thrashing when zoom hovers near threshold
    constexpr qreal kFullQualityThreshold = 0.20;    // >= 20% zoom
    constexpr qreal kMediumQualityThreshold = 0.05;  // 5-20% zoom
    constexpr qreal kHysteresisMargin = 0.02;        // 2% hysteresis band
    
    // If zoom direction changed, apply hysteresis
    const bool zoomingIn = canvasZoom > m_lastStrokeQualityZoom;
    const bool zoomingOut = canvasZoom < m_lastStrokeQualityZoom;
    
    if (canvasZoom >= kFullQualityThreshold) {
        return StrokeQuality::Full;
    } else if (canvasZoom >= kMediumQualityThreshold) {
        // Apply hysteresis: if transitioning from Full, require zoom to drop below (threshold - margin)
        if (m_lastStrokeQuality == StrokeQuality::Full && zoomingOut) {
            if (canvasZoom >= (kFullQualityThreshold - kHysteresisMargin)) {
                return StrokeQuality::Full; // Stay in Full
            }
        }
        return StrokeQuality::Medium;
    } else {
        // Apply hysteresis: if transitioning from Medium, require zoom to drop below (threshold - margin)
        if (m_lastStrokeQuality == StrokeQuality::Medium && zoomingOut) {
            if (canvasZoom >= (kMediumQualityThreshold - kHysteresisMargin)) {
                return StrokeQuality::Medium; // Stay in Medium
            }
        }
        return StrokeQuality::Low;
    }
}

void TextMediaItem::paintSimplifiedStroke(QPainter* painter, const VectorDrawSnapshot& snapshot, const QSize& targetSize, qreal scaleFactor, qreal canvasZoom) {
    // Phase 8: Low-quality mode - draw fills + unified outline around text bounds
    // This is 80%+ faster at very small zoom levels where per-glyph detail is imperceptible
    
    const int targetWidth = std::max(1, targetSize.width());
    const int targetHeight = std::max(1, targetSize.height());
    
    const qreal epsilon = 1e-4;
    const qreal effectiveScale = std::max(std::abs(scaleFactor), epsilon);
    
    QTextDocument doc;
    doc.setDocumentMargin(0.0);
    QFont renderFont = ensureRenderableFont(snapshot.font, mediaId(), "simplifiedStroke");
    doc.setDefaultFont(renderFont);
    doc.setPlainText(snapshot.text);
    
    // Set alignment option so Qt's layout engine positions lines correctly
    Qt::Alignment qtHAlign = Qt::AlignCenter;
    switch (snapshot.horizontalAlignment) {
        case HorizontalAlignment::Left:
            qtHAlign = Qt::AlignLeft;
            break;
        case HorizontalAlignment::Center:
            qtHAlign = Qt::AlignHCenter;
            break;
        case HorizontalAlignment::Right:
            qtHAlign = Qt::AlignRight;
            break;
    }
    
    QTextOption option;
    option.setWrapMode(snapshot.fitToTextEnabled ? QTextOption::NoWrap : QTextOption::WordWrap);
    option.setAlignment(qtHAlign);
    doc.setDefaultTextOption(option);
    
    // CRITICAL: Use availableWidth (excluding margins) to match main rendering
    const qreal logicalWidth = static_cast<qreal>(targetWidth) / effectiveScale;
    const qreal margin = snapshot.contentPaddingPx;
    const qreal availableWidth = std::max<qreal>(1.0, logicalWidth - 2.0 * margin);
    
    if (snapshot.fitToTextEnabled) {
        doc.setTextWidth(-1.0);
    } else {
        doc.setTextWidth(availableWidth);
    }
    
    QAbstractTextDocumentLayout* layout = doc.documentLayout();
    if (!layout) return;
    
    const QSizeF docSize = layout->documentSize();
    const QRectF docBounds = computeDocumentTextBounds(doc, layout);
    const qreal logicalHeight = static_cast<qreal>(targetHeight) / effectiveScale;
    const qreal availableHeight = std::max<qreal>(1.0, logicalHeight - 2.0 * margin);
    const qreal docVisualTop = docBounds.top();
    const qreal docVisualHeight = std::max<qreal>(1.0, docBounds.height());
    
    // Calculate offsets to match main rendering
    qreal offsetX = margin;
    if (!snapshot.fitToTextEnabled) {
        const qreal horizontalSpace = std::max<qreal>(0.0, availableWidth - docSize.width());
        switch (snapshot.horizontalAlignment) {
            case HorizontalAlignment::Left:
                offsetX = margin;
                break;
            case HorizontalAlignment::Center:
                offsetX = margin + horizontalSpace * 0.5;
                break;
            case HorizontalAlignment::Right:
                offsetX = margin + horizontalSpace;
                break;
        }
    }
    
    qreal offsetY = margin;
    switch (snapshot.verticalAlignment) {
        case VerticalAlignment::Top:
            offsetY = margin - docVisualTop;
            break;
        case VerticalAlignment::Center:
            offsetY = margin + (availableHeight - docVisualHeight) / 2.0 - docVisualTop;
            break;
        case VerticalAlignment::Bottom:
            offsetY = margin + (availableHeight - docVisualHeight) - docVisualTop;
            break;
    }
    
    painter->save();
    painter->scale(effectiveScale, effectiveScale);
    painter->translate(offsetX, offsetY);
    
    const QColor fillColor = snapshot.fillColor;
    const QColor outlineColor = snapshot.outlineColor;
    const qreal strokeWidth = computeStrokeWidthFromFont(snapshot.font, snapshot.outlineWidthPercent);
    
    // Step 1: Draw all fills normally (fast path)
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.cursorPosition = -1;
    ctx.palette.setColor(QPalette::Text, fillColor);
    layout->draw(painter, ctx);
    
    // Step 2: Create unified outline path around all glyphs
    QPainterPath unifiedOutline;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        QTextLayout* textLayout = block.layout();
        if (!textLayout) continue;
        
        const QRectF blockRect = layout->blockBoundingRect(block);
        for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
            QTextLine line = textLayout->lineAt(lineIndex);
            if (!line.isValid()) continue;
            
            // Use line.x() from Qt's layout engine which already handles alignment
            const qreal lineOffsetX = line.x();
            
            const QList<QGlyphRun> glyphRuns = line.glyphRuns();
            for (const QGlyphRun& run : glyphRuns) {
                const QVector<quint32> indexes = run.glyphIndexes();
                const QVector<QPointF> positions = run.positions();
                if (indexes.size() != positions.size()) continue;
                const QRawFont rawFont = run.rawFont();
                
                for (int gi = 0; gi < indexes.size(); ++gi) {
                    const QPointF glyphPos = blockRect.topLeft() + QPointF(lineOffsetX, line.y()) + positions[gi];
                    QPainterPath glyphPath = cachedGlyphPath(rawFont, indexes[gi]);
                    glyphPath.translate(glyphPos);
                    unifiedOutline.addPath(glyphPath);
                }
            }
        }
    }
    
    // Step 3: Stroke the unified outline (single operation instead of per-glyph)
    painter->setPen(QPen(outlineColor, strokeWidth * 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(unifiedOutline);
    
    painter->restore();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// TIER 3 OPTIMIZATION IMPLEMENTATIONS
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Phase 3: Texture Atlas Management
quint64 TextMediaItem::getAtlasKey(const QPixmap& glyph, const QColor& fill, const QColor& stroke, qreal strokeWidth, qreal scale) const {
    // Generate unique key for atlas lookup - combines glyph visual properties
    quint64 h = qHash(glyph.cacheKey());
    h ^= static_cast<quint64>(fill.rgba()) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= static_cast<quint64>(stroke.rgba()) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= static_cast<quint64>(strokeWidth * 1000.0) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= static_cast<quint64>(scale * 1000.0) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

bool TextMediaItem::insertGlyphIntoAtlas(quint64 key, const QPixmap& glyph) {
    // Already in atlas?
    if (m_atlasGlyphMap.contains(key)) {
        return true;
    }
    
    const QImage glyphImg = glyph.toImage();
    const QSize glyphSize = glyphImg.size();
    
    // Try to insert into existing atlases
    for (int i = 0; i < m_glyphAtlases.size(); ++i) {
        if (m_glyphAtlases[i].isFull) continue;
        
        std::optional<QRect> rect = m_glyphAtlases[i].tryInsert(glyphSize);
        if (rect.has_value()) {
            // Blit glyph into atlas
            QPainter p(&m_glyphAtlases[i].sheet);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(rect->topLeft(), glyphImg);
            p.end();
            
            // Record location
            AtlasGlyphEntry entry;
            entry.atlasRect = *rect;
            entry.atlasSheetIndex = i;
            entry.offset = QPointF(0, 0); // Offset handled during rendering
            m_atlasGlyphMap.insert(key, entry);
            return true;
        }
    }
    
    // No space - create new atlas
    if (m_glyphAtlases.size() >= 4) {
        // Atlas pool full - evict oldest sheet (LRU-like)
        if (textHotLogsEnabled()) {
            qDebug() << "TextMediaItem: Atlas pool full, clearing oldest sheet";
        }
        m_glyphAtlases.removeFirst();
        
        // Remove stale map entries pointing to deleted sheet
        QMutableHashIterator<quint64, AtlasGlyphEntry> it(m_atlasGlyphMap);
        while (it.hasNext()) {
            it.next();
            if (it.value().atlasSheetIndex == 0) {
                it.remove();
            } else {
                // Decrement sheet indices after removal
                it.value().atlasSheetIndex--;
            }
        }
    }
    
    // Add new atlas and retry
    m_glyphAtlases.append(TextureAtlas());
    return insertGlyphIntoAtlas(key, glyph); // Recursive retry
}

std::optional<TextMediaItem::AtlasGlyphEntry> TextMediaItem::getGlyphFromAtlas(quint64 key) const {
    auto it = m_atlasGlyphMap.constFind(key);
    if (it != m_atlasGlyphMap.constEnd()) {
        return it.value();
    }
    return std::nullopt;
}

void TextMediaItem::clearAtlases() {
    m_glyphAtlases.clear();
    m_atlasGlyphMap.clear();
}

// Phase 4: Text Diff Algorithm
TextMediaItem::TextEditDiff TextMediaItem::computeTextDiff(const QString& newText, const QFont& newFont, int newWidth) const {
    TextEditDiff diff;
    
    // Check if font or layout width changed (geometric change)
    const bool fontChanged = (newFont != m_previousFont);
    const bool widthChanged = (newWidth != m_previousTextWidth);
    diff.geometryChanged = fontChanged || widthChanged;
    
    if (diff.geometryChanged) {
        // Full re-render required - geometry invalidates all caches
        diff.changeStartIdx = 0;
        diff.changeEndIdx = newText.length();
        diff.insertedCount = newText.length();
        diff.deletedCount = m_previousText.length();
        return diff;
    }
    
    // Compute text diff using longest common subsequence approach
    const int oldLen = m_previousText.length();
    const int newLen = newText.length();
    
    if (oldLen == 0) {
        // First render - all text is new
        diff.changeStartIdx = 0;
        diff.changeEndIdx = newLen;
        diff.insertedCount = newLen;
        diff.deletedCount = 0;
        return diff;
    }
    
    // Find common prefix
    int prefixLen = 0;
    while (prefixLen < oldLen && prefixLen < newLen && m_previousText[prefixLen] == newText[prefixLen]) {
        ++prefixLen;
    }
    
    // Find common suffix
    int suffixLen = 0;
    while (suffixLen < (oldLen - prefixLen) && suffixLen < (newLen - prefixLen) &&
           m_previousText[oldLen - 1 - suffixLen] == newText[newLen - 1 - suffixLen]) {
        ++suffixLen;
    }
    
    // Calculate change region
    diff.changeStartIdx = prefixLen;
    diff.changeEndIdx = newLen - suffixLen;
    diff.deletedCount = (oldLen - prefixLen - suffixLen);
    diff.insertedCount = (newLen - prefixLen - suffixLen);
    
    // If change is too large (>30% of text), treat as geometric change for simplicity
    const qreal changeRatio = static_cast<qreal>(diff.insertedCount + diff.deletedCount) / std::max(1, oldLen + newLen);
    if (changeRatio > 0.3) {
        diff.geometryChanged = true;
    }
    
    return diff;
}

void TextMediaItem::applyTextDiff(const TextEditDiff& diff) {
    // Update tracked state
    m_previousText = textForRendering();
    m_previousFont = m_font;
    m_previousTextWidth = static_cast<int>(boundingRect().width());
    
    // Invalidate affected caches
    if (diff.geometryChanged) {
        // Full invalidation
        m_glyphLayoutCache.clear();
        m_blockVisibilityCacheValid = false;
        m_baseRasterValid = false;
        m_incrementalRenderValid = false;
    } else if (diff.hasChanges()) {
        // Partial invalidation - glyph cache stays valid for unchanged glyphs
        m_blockVisibilityCacheValid = false;
        m_incrementalRenderValid = false;
    }
}

// Phase 5: Partial Compositing
void TextMediaItem::initializeBaseRaster(const QSize& size) {
    if (m_baseRaster.size() != size || m_baseRaster.format() != QImage::Format_ARGB32_Premultiplied) {
        m_baseRaster = QImage(size, QImage::Format_ARGB32_Premultiplied);
        m_baseRaster.fill(Qt::transparent);
        m_baseRasterValid = false;
    }
}

void TextMediaItem::renderDirtyRegion(const TextEditDiff& diff, const VectorDrawSnapshot& snapshot, const QSize& targetSize, qreal scaleFactor) {
    // Phase 5: Render only the changed region to overlay
    if (!diff.hasChanges()) {
        return;
    }
    
    // Initialize overlay if needed
    if (m_dirtyOverlay.size() != targetSize || m_dirtyOverlay.format() != QImage::Format_ARGB32_Premultiplied) {
        m_dirtyOverlay = QImage(targetSize, QImage::Format_ARGB32_Premultiplied);
    }
    m_dirtyOverlay.fill(Qt::transparent);
    
    // TODO: Compute bounding box of changed glyphs
    // For now, conservatively mark entire image as dirty
    m_dirtyBounds = QRect(0, 0, targetSize.width(), targetSize.height());
    
    // Render changed region to overlay
    QPainter overlayPainter(&m_dirtyOverlay);
    overlayPainter.setRenderHint(QPainter::Antialiasing, true);
    overlayPainter.setRenderHint(QPainter::TextAntialiasing, true);
    overlayPainter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    
    // Use existing paintVectorSnapshot for overlay rendering
    // Note: This is simplified - full implementation would render only diff.changeStartIdx to diff.changeEndIdx
    paintVectorSnapshot(&overlayPainter, snapshot, targetSize, scaleFactor, 1.0, QRectF());
    
    m_incrementalRenderValid = true;
}

QImage TextMediaItem::compositeRasterLayers() const {
    if (!m_baseRasterValid || !m_incrementalRenderValid) {
        // Fallback to base raster only
        return m_baseRaster.copy();
    }
    
    // Composite base + overlay
    QImage result = m_baseRaster.copy();
    QPainter p(&result);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.drawImage(m_dirtyBounds.topLeft(), m_dirtyOverlay, m_dirtyBounds);
    p.end();
    
    return result;
}

void TextMediaItem::flattenCompositeToBase() {
    if (!m_incrementalRenderValid) {
        return;
    }
    
    // Merge overlay into base raster
    QPainter p(&m_baseRaster);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.drawImage(m_dirtyBounds.topLeft(), m_dirtyOverlay, m_dirtyBounds);
    p.end();
    
    // Clear overlay
    m_dirtyOverlay.fill(Qt::transparent);
    m_incrementalRenderValid = false;
    m_baseRasterValid = true;
}

bool TextMediaItem::shouldUseIncrementalRender(const TextEditDiff& diff) const {
    // Use incremental rendering for small text edits
    if (diff.geometryChanged) {
        return false; // Geometric changes require full re-render
    }
    
    if (!diff.hasChanges()) {
        return false; // No changes - no need for incremental
    }
    
    // Threshold: use incremental if change affects <20% of text
    const int totalChars = m_previousText.length();
    if (totalChars == 0) return false;
    
    const int changedChars = diff.insertedCount + diff.deletedCount;
    const qreal changeRatio = static_cast<qreal>(changedChars) / static_cast<qreal>(totalChars);
    
    return changeRatio < 0.2;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

TextMediaItem::VectorDrawSnapshot TextMediaItem::captureVectorSnapshot(StrokeRenderMode mode) const {
    VectorDrawSnapshot snapshot;
    snapshot.text = textForRendering();
    snapshot.font = m_font;
    snapshot.font = ensureRenderableFont(snapshot.font, mediaId(), "snapshot");
    snapshot.fillColor = m_textColor;
    snapshot.outlineColor = m_textBorderColor;
    const bool usePreviewStroke = (mode == StrokeRenderMode::Preview) && m_previewStrokeActive;
    snapshot.outlineWidthPercent = usePreviewStroke ? m_previewStrokePercent : m_textBorderWidthPercent;
    snapshot.highlightEnabled = m_highlightEnabled;
    snapshot.highlightColor = m_highlightColor;
    snapshot.contentPaddingPx = contentPaddingPx();
    snapshot.fitToTextEnabled = m_fitToTextEnabled;
    snapshot.horizontalAlignment = m_horizontalAlignment;
    snapshot.verticalAlignment = m_verticalAlignment;
    snapshot.uniformScaleFactor = m_uniformScaleFactor;
    return snapshot;
}

void TextMediaItem::invalidateRenderPipeline(InvalidationReason reason, bool invalidateLayout) {
    ++m_contentRevision;
    m_renderScheduler.invalidate(reason);
    m_needsRasterization = true;
    m_scaledRasterDirty = true;
    m_forceScaledRasterRefresh = true;
    m_scaledRasterContentRevision = 0;
    m_frozenFallbackContentRevision = 0;
    m_baseRasterContentRevision = 0;
    m_pendingAsyncRasterRequest.reset();
    m_pendingRasterRequestId = 0;
    m_activeHighResCacheKeyValid = false;
    m_previewCacheKeyValid = false;
    if (invalidateLayout) {
        m_layoutSnapshot.valid = false;
        m_documentMetricsDirty = true;
    }
}

TextMediaItem::TextRenderCacheKey TextMediaItem::makeCacheKey(const VectorDrawSnapshot& snapshot,
                                                              const QSize& targetSize,
                                                              qreal scaleFactor,
                                                              qreal dpr) const {
    TextRenderCacheKey key;
    key.textHash = qHash(snapshot.text);

    const QString styleSignature = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10")
        .arg(snapshot.font.family())
        .arg(snapshot.font.pixelSize())
        .arg(snapshot.font.weight())
        .arg(snapshot.fillColor.rgba())
        .arg(snapshot.outlineColor.rgba())
        .arg(snapshot.outlineWidthPercent, 0, 'f', 3)
        .arg(snapshot.highlightEnabled ? 1 : 0)
        .arg(snapshot.highlightColor.rgba())
        .arg(static_cast<int>(snapshot.horizontalAlignment))
        .arg(static_cast<int>(snapshot.verticalAlignment));
    key.styleHash = qHash(styleSignature);

    key.wrapWidthPx = std::max(1, static_cast<int>(std::llround(std::max<qreal>(1.0, static_cast<qreal>(m_baseSize.width()) - 2.0 * snapshot.contentPaddingPx))));
    key.targetWidthPx = std::max(1, targetSize.width());
    key.targetHeightPx = std::max(1, targetSize.height());
    key.scalePermille = std::max(1, static_cast<int>(std::llround(std::max(scaleFactor, 1e-4) * 1000.0)));
    key.dprPermille = std::max(1, static_cast<int>(std::llround(std::max(dpr, 1e-4) * 1000.0)));
    key.fitToText = snapshot.fitToTextEnabled;
    return key;
}

bool TextMediaItem::cacheKeyMatches(const TextRenderCacheKey& lhs, const TextRenderCacheKey& rhs) const {
    return lhs == rhs;
}

void TextMediaItem::enforceCacheBudget() {
    if (!textCachePolicyV2Enabled()) {
        return;
    }

    auto imageBytes = [](const QImage& image) -> qint64 {
        if (image.isNull()) {
            return 0;
        }
        return static_cast<qint64>(image.bytesPerLine()) * static_cast<qint64>(image.height());
    };

    const qint64 total = imageBytes(m_scaledRasterizedText) +
                         imageBytes(m_rasterizedText) +
                         imageBytes(m_baseRaster) +
                         imageBytes(m_dirtyOverlay);
    if (total <= kPerItemCacheBudgetBytes) {
        return;
    }

    m_frozenFallbackValid = false;
    m_frozenFallbackPixmap = QPixmap();

    if (total > (kPerItemCacheBudgetBytes * 2)) {
        m_scaledRasterPixmapValid = false;
        m_scaledRasterPixmap = QPixmap();
        m_scaledRasterizedText = QImage();
        m_previewCacheKeyValid = false;
    }
}

quint64 TextMediaItem::computeLayoutSnapshotKey(const QString& text, const QFont& font, qreal wrapWidth) const {
    QString signature;
    signature.reserve(text.size() + 128);
    signature.append(text);
    signature.append(QLatin1Char('|'));
    signature.append(font.family());
    signature.append(QLatin1Char('|'));
    signature.append(QString::number(font.pixelSize()));
    signature.append(QLatin1Char('|'));
    signature.append(QString::number(font.weight()));
    signature.append(QLatin1Char('|'));
    signature.append(QString::number(wrapWidth, 'f', 3));
    signature.append(QLatin1Char('|'));
    signature.append(QString::number(static_cast<int>(m_horizontalAlignment)));
    signature.append(QLatin1Char('|'));
    signature.append(QString::number(m_fitToTextEnabled ? 1 : 0));
    return static_cast<quint64>(qHash(signature));
}

bool TextMediaItem::canReuseLayoutSnapshot(const QString& text, const QFont& font, qreal wrapWidth) const {
    if (!m_layoutSnapshot.valid) {
        return false;
    }
    const quint64 key = computeLayoutSnapshotKey(text, font, wrapWidth);
    return key == m_layoutSnapshot.key;
}

void TextMediaItem::updateLayoutSnapshot(const QString& text,
                                         const QFont& font,
                                         qreal wrapWidth,
                                         const QSizeF& docSize,
                                         qreal idealWidth,
                                         int lineCount) {
    m_layoutSnapshot.key = computeLayoutSnapshotKey(text, font, wrapWidth);
    m_layoutSnapshot.wrapWidth = wrapWidth;
    m_layoutSnapshot.docSize = docSize;
    m_layoutSnapshot.idealWidth = idealWidth;
    m_layoutSnapshot.lineCount = lineCount;
    m_layoutSnapshot.valid = true;
}

void TextMediaItem::ensureTextRenderer() {
    if (m_textRenderer) {
        return;
    }

    struct CpuTextRenderer final : ITextRenderer {
        QImage render(const TextRasterJob& job) const override {
            return job.execute();
        }
        const char* backendName() const override {
            return "cpu";
        }
    };

    struct GpuTextRendererStub final : ITextRenderer {
        QImage render(const TextRasterJob& job) const override {
            return job.execute();
        }
        const char* backendName() const override {
            return "gpu-stub";
        }
    };

    if (textRendererGpuEnabled()) {
        m_textRenderer = std::make_unique<GpuTextRendererStub>();
    } else {
        m_textRenderer = std::make_unique<CpuTextRenderer>();
    }
    m_textRendererBackend = QString::fromLatin1(m_textRenderer->backendName());
}

QImage TextMediaItem::renderRasterJobWithBackend(const TextRasterJob& job) const {
    if (m_textRenderer) {
        return m_textRenderer->render(job);
    }
    return job.execute();
}

void TextMediaItem::paintVectorSnapshot(QPainter* painter, const VectorDrawSnapshot& snapshot, const QSize& targetSize, qreal scaleFactor, qreal canvasZoom, const QRectF& viewport) {
    if (!painter) {
        return;
    }

    QElapsedTimer snapshotTimer;
    snapshotTimer.start();

    const int targetWidth = std::max(1, targetSize.width());
    const int targetHeight = std::max(1, targetSize.height());

    const qreal epsilon = 1e-4;
    const qreal effectiveScale = std::max(std::abs(scaleFactor), epsilon);

    QTextDocument doc;
    doc.setDocumentMargin(0.0);
    doc.setDefaultFont(snapshot.font);
    doc.setPlainText(snapshot.text);

    Qt::Alignment qtHAlign = Qt::AlignCenter;
    switch (snapshot.horizontalAlignment) {
        case HorizontalAlignment::Left:
            qtHAlign = Qt::AlignLeft;
            break;
        case HorizontalAlignment::Center:
            qtHAlign = Qt::AlignHCenter;
            break;
        case HorizontalAlignment::Right:
            qtHAlign = Qt::AlignRight;
            break;
    }

    QTextOption option;
    option.setWrapMode(snapshot.fitToTextEnabled ? QTextOption::NoWrap : QTextOption::WordWrap);
    option.setAlignment(qtHAlign);
    doc.setDefaultTextOption(option);

    const qreal logicalWidth = static_cast<qreal>(targetWidth) / effectiveScale;
    const qreal logicalHeight = static_cast<qreal>(targetHeight) / effectiveScale;
    const qreal margin = snapshot.contentPaddingPx;
    const qreal availableWidth = std::max<qreal>(1.0, logicalWidth - 2.0 * margin);

    if (snapshot.fitToTextEnabled) {
        doc.setTextWidth(-1.0);
    } else {
        doc.setTextWidth(availableWidth);
    }

    QAbstractTextDocumentLayout* layout = doc.documentLayout();
    if (!layout) {
        return;
    }
    const QSizeF docSize = layout->documentSize();
    const QRectF docBounds = computeDocumentTextBounds(doc, layout);
    const qreal availableHeight = std::max<qreal>(1.0, logicalHeight - 2.0 * margin);
    const qreal docVisualTop = docBounds.top();
    const qreal docVisualHeight = std::max<qreal>(1.0, docBounds.height());

    qreal offsetX = margin;
    if (!snapshot.fitToTextEnabled) {
        const qreal horizontalSpace = std::max<qreal>(0.0, availableWidth - docSize.width());
        switch (snapshot.horizontalAlignment) {
            case HorizontalAlignment::Left:
                offsetX = margin;
                break;
            case HorizontalAlignment::Center:
                offsetX = margin + horizontalSpace * 0.5;
                break;
            case HorizontalAlignment::Right:
                offsetX = margin + horizontalSpace;
                break;
        }
    }

    qreal offsetY = margin;
    switch (snapshot.verticalAlignment) {
        case VerticalAlignment::Top:
            offsetY = margin - docVisualTop;
            break;
        case VerticalAlignment::Center:
            offsetY = margin + (availableHeight - docVisualHeight) / 2.0 - docVisualTop;
            break;
        case VerticalAlignment::Bottom:
            offsetY = margin + (availableHeight - docVisualHeight) - docVisualTop;
            break;
    }

    const QColor fillColor = snapshot.fillColor;
    QColor outlineColor = snapshot.outlineColor.isValid() ? snapshot.outlineColor : fillColor;
    if (!outlineColor.isValid()) {
        outlineColor = fillColor;
    }

    bool highlightEnabled = snapshot.highlightEnabled && snapshot.highlightColor.alpha() > 0;
    QColor highlightColor = highlightEnabled ? snapshot.highlightColor : QColor();
    if (highlightEnabled && !highlightColor.isValid()) {
        highlightColor = TextMediaDefaults::TEXT_HIGHLIGHT_COLOR;
    }

    const qreal baseStrokeWidth = computeStrokeWidthFromFont(snapshot.font, snapshot.outlineWidthPercent);
    const qreal uniformScale = std::max(std::abs(snapshot.uniformScaleFactor), epsilon);
    const qreal strokeWidth = baseStrokeWidth * uniformScale;
    const QString preview = previewTextForLog(snapshot.text);
    auto logSnapshotPerf = [&](const char* context, qint64 outlineMs, int glyphCount, qint64 totalMs) {
        if (strokeWidth <= 0.0 && totalMs < 4) {
            return;
        }
        logStrokeDiagnostics(context,
                              snapshot.outlineWidthPercent,
                              strokeWidth,
                              glyphCount,
                              outlineMs,
                              totalMs,
                              docSize,
                              targetSize,
                              scaleFactor,
                              preview);
    };

    {
        QTextCursor cursor(&doc);
        QTextCharFormat format;
        format.setForeground(fillColor);
        format.clearProperty(QTextFormat::TextOutline);
        cursor.mergeCharFormat(format);
    }

    painter->save();
    painter->scale(effectiveScale, effectiveScale);

    if (!snapshot.fitToTextEnabled) {
        painter->translate(offsetX, offsetY);

        if (highlightEnabled && highlightColor.alpha() > 0) {
            painter->save();
            painter->setPen(Qt::NoPen);
            painter->setBrush(highlightColor);

            const qreal docWidth = std::max<qreal>(docSize.width(), 1.0);
            for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
                QTextLayout* textLayout = block.layout();
                if (!textLayout) continue;

                const QRectF blockRect = layout->blockBoundingRect(block);
                for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
                    QTextLine line = textLayout->lineAt(lineIndex);
                    if (!line.isValid()) continue;

                    const qreal lineWidth = line.naturalTextWidth();
                    const qreal width = std::max<qreal>(lineWidth, 1.0);
                    qreal alignedX = line.x();
                    if (std::abs(alignedX) < 1e-4 && width < docWidth - 1e-4) {
                        const qreal horizontalSpace = std::max<qreal>(0.0, docWidth - width);
                        if (snapshot.horizontalAlignment == HorizontalAlignment::Right) {
                            alignedX += horizontalSpace;
                        } else if (snapshot.horizontalAlignment == HorizontalAlignment::Center) {
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

        int glyphCount = 0;
        qint64 outlineMs = 0;
        if (strokeWidth > 0.0) {
            QElapsedTimer outlineTimer;
            outlineTimer.start();
            
            // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
            // TIER 2: Phase 8 - Stroke Quality LOD Selection
            // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
            
            // Determine stroke quality based on canvas zoom level
            StrokeQuality quality = StrokeQuality::Full;
            if (canvasZoom < 0.05) {
                quality = StrokeQuality::Low;
            } else if (canvasZoom < 0.20) {
                quality = StrokeQuality::Medium;
            }
            
            if (quality == StrokeQuality::Low) {
                // Phase 8: Simplified stroke - draw fills then unified outline
                painter->save();
                
                // Step 1: Draw all fills
                QAbstractTextDocumentLayout::PaintContext ctx;
                ctx.cursorPosition = -1;
                ctx.palette.setColor(QPalette::Text, fillColor);
                layout->draw(painter, ctx);
                
                // Step 2: Create unified outline around all glyphs
                QPainterPath unifiedOutline;
                for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
                    QTextLayout* textLayout = block.layout();
                    if (!textLayout) continue;
                    
                    const QRectF blockRect = layout->blockBoundingRect(block);
                    for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
                        QTextLine line = textLayout->lineAt(lineIndex);
                        if (!line.isValid()) continue;
                        
                        // Use line.x() from Qt's layout engine which already handles alignment
                        const qreal lineOffsetX = line.x();
                        
                        const QList<QGlyphRun> glyphRuns = line.glyphRuns();
                        for (const QGlyphRun& run : glyphRuns) {
                            const QVector<quint32> indexes = run.glyphIndexes();
                            const QVector<QPointF> positions = run.positions();
                            if (indexes.size() != positions.size()) continue;
                            const QRawFont rawFont = run.rawFont();
                            
                            for (int gi = 0; gi < indexes.size(); ++gi) {
                                const QPointF glyphPos = blockRect.topLeft() + QPointF(lineOffsetX, line.y()) + positions[gi];
                                QPainterPath glyphPath = cachedGlyphPath(rawFont, indexes[gi]);
                                glyphPath.translate(glyphPos);
                                unifiedOutline.addPath(glyphPath);
                            }
                            glyphCount += indexes.size();
                        }
                    }
                }
                
                // Step 3: Single stroke operation on unified outline (80%+ faster)
                painter->setPen(QPen(outlineColor, strokeWidth * 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter->setBrush(Qt::NoBrush);
                painter->drawPath(unifiedOutline);
                painter->restore();
            } else {
                // Phase 2 + Phase 7: Full quality with viewport culling
                // Strict two-pass rendering - ALL strokes first, then ALL fills
                
                // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
                // TIER 2: Phase 7 - Build Block Visibility Cache (if viewport provided)
                // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
                QVector<bool> skipBlock;
                if (!viewport.isEmpty() && viewport.isValid()) {
                    int blockIdx = 0;
                    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next(), ++blockIdx) {
                        const QRectF blockBounds = layout->blockBoundingRect(block);
                        const bool isInvisible = !viewport.intersects(blockBounds);
                        skipBlock.append(isInvisible);
                    }
                }
                
                // Pass 1: Render all strokes (background layer)
                int blockIdx = 0;
                for (QTextBlock block = doc.begin(); block.isValid(); block = block.next(), ++blockIdx) {
                    // Phase 7: Skip blocks outside viewport
                    if (blockIdx < skipBlock.size() && skipBlock[blockIdx]) {
                        continue;
                    }
                    
                    QTextLayout* textLayout = block.layout();
                    if (!textLayout) continue;

                    const QRectF blockRect = layout->blockBoundingRect(block);
                    for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
                        QTextLine line = textLayout->lineAt(lineIndex);
                        if (!line.isValid()) continue;

                        // Use line.x() from Qt's layout engine which already handles alignment
                        const qreal lineOffsetX = line.x();

                        const QList<QGlyphRun> glyphRuns = line.glyphRuns();
                        for (const QGlyphRun& run : glyphRuns) {
                            const QVector<quint32> indexes = run.glyphIndexes();
                            const QVector<QPointF> positions = run.positions();
                            if (indexes.size() != positions.size()) continue;
                            const QRawFont rawFont = run.rawFont();
                            
                            for (int gi = 0; gi < indexes.size(); ++gi) {
                                const QPointF glyphPos = blockRect.topLeft() + QPointF(lineOffsetX, line.y()) + positions[gi];
                                
                                // TIER 3: Use pre-rendered cached glyph (stroke+fill baked in)
                                const RenderedGlyphBitmap glyphBitmap = cachedRenderedGlyph(rawFont, indexes[gi], 
                                                                                           fillColor, outlineColor, 
                                                                                           strokeWidth, scaleFactor);
                                
                                if (!glyphBitmap.pixmap.isNull()) {
                                    // Draw pre-rendered glyph (contains stroke + fill) with offset compensation
                                    painter->drawPixmap(glyphPos + glyphBitmap.originOffset, glyphBitmap.pixmap);
                                } else {
                                    // Fallback to path rendering
                                    const QPainterPath glyphPath = cachedGlyphPath(rawFont, indexes[gi]);
                                    painter->save();
                                    painter->translate(glyphPos);
                                    painter->setPen(QPen(outlineColor, strokeWidth * 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                                    painter->setBrush(Qt::NoBrush);
                                    painter->drawPath(glyphPath);
                                    painter->restore();
                                }
                            }
                            glyphCount += indexes.size();
                        }
                    }
                }
                
                // Pass 2: Render all fills on top (foreground layer) - ONLY for fallback glyphs
                blockIdx = 0;
                for (QTextBlock block = doc.begin(); block.isValid(); block = block.next(), ++blockIdx) {
                    // Phase 7: Skip blocks outside viewport
                    if (blockIdx < skipBlock.size() && skipBlock[blockIdx]) {
                        continue;
                    }
                    
                    QTextLayout* textLayout = block.layout();
                    if (!textLayout) continue;

                    const QRectF blockRect = layout->blockBoundingRect(block);
                    for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
                        QTextLine line = textLayout->lineAt(lineIndex);
                        if (!line.isValid()) continue;

                        // Use line.x() from Qt's layout engine which already handles alignment
                        const qreal lineOffsetX = line.x();

                        const QList<QGlyphRun> glyphRuns = line.glyphRuns();
                        for (const QGlyphRun& run : glyphRuns) {
                            const QVector<quint32> indexes = run.glyphIndexes();
                            const QVector<QPointF> positions = run.positions();
                            if (indexes.size() != positions.size()) continue;
                            const QRawFont rawFont = run.rawFont();
                            
                            for (int gi = 0; gi < indexes.size(); ++gi) {
                                const QPointF glyphPos = blockRect.topLeft() + QPointF(lineOffsetX, line.y()) + positions[gi];
                                
                                // Only draw fill if we don't have cached pixmap
                                const RenderedGlyphBitmap glyphBitmap = cachedRenderedGlyph(rawFont, indexes[gi], 
                                                                                           fillColor, outlineColor, 
                                                                                           strokeWidth, scaleFactor);
                                
                                if (glyphBitmap.pixmap.isNull()) {
                                    const QPainterPath glyphPath = cachedGlyphPath(rawFont, indexes[gi]);
                                    painter->save();
                                    painter->translate(glyphPos);
                                    painter->setPen(Qt::NoPen);
                                    painter->setBrush(fillColor);
                                    painter->drawPath(glyphPath);
                                    painter->restore();
                                }
                            }
                        }
                    }
                }
            }
            
            outlineMs = outlineTimer.elapsed();
        } else {
            QAbstractTextDocumentLayout::PaintContext ctx;
            ctx.cursorPosition = -1;
            ctx.palette.setColor(QPalette::Text, fillColor);
            layout->draw(painter, ctx);
        }

        painter->restore();
        logSnapshotPerf("vector-full", outlineMs, glyphCount, snapshotTimer.elapsed());
        return;
    }

    painter->translate(margin, offsetY);
    const qreal contentWidth = availableWidth;
    int glyphCount = 0;
    qint64 outlineMs = 0;
    QElapsedTimer outlineTimer;
    bool outlineTimerRunning = false;

    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        QTextLayout* textLayout = block.layout();
        if (!textLayout) continue;

        const QRectF blockRect = layout->blockBoundingRect(block);
        for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
            QTextLine line = textLayout->lineAt(lineIndex);
            if (!line.isValid()) continue;

            qreal lineOffsetX = 0.0;
            const qreal lineWidth = line.naturalTextWidth();
            switch (snapshot.horizontalAlignment) {
                case HorizontalAlignment::Left:
                    lineOffsetX = 0.0;
                    break;
                case HorizontalAlignment::Center:
                    lineOffsetX = std::max<qreal>(0.0, (contentWidth - lineWidth) * 0.5);
                    break;
                case HorizontalAlignment::Right:
                    lineOffsetX = std::max<qreal>(0.0, contentWidth - lineWidth);
                    break;
            }

            const QPointF lineBasePos(lineOffsetX, blockRect.top() + line.y());

            if (highlightEnabled && highlightColor.alpha() > 0) {
                const qreal highlightWidth = std::max<qreal>(lineWidth > 0.0 ? lineWidth : contentWidth, 1.0);
                const qreal highlightHeight = std::max<qreal>(line.height(), 1.0);
                const QRectF highlightRect(QPointF(lineOffsetX, blockRect.top() + line.y()), QSizeF(highlightWidth, highlightHeight));
                painter->fillRect(highlightRect, highlightColor);
            }

            if (strokeWidth > 0.0) {
                if (!outlineTimerRunning) {
                    outlineTimer.start();
                    outlineTimerRunning = true;
                }
                
                // Phase 2: Strict two-pass rendering for fit-to-text mode
                const QList<QGlyphRun> glyphRuns = line.glyphRuns();
                
                // Pass 1: Render all strokes first (background)
                for (const QGlyphRun& run : glyphRuns) {
                    const QVector<quint32> indexes = run.glyphIndexes();
                    const QVector<QPointF> positions = run.positions();
                    if (indexes.size() != positions.size()) continue;
                    const QRawFont rawFont = run.rawFont();
                    
                    for (int gi = 0; gi < indexes.size(); ++gi) {
                        // Apply horizontal alignment offset to glyph position
                        const QPointF glyphPos = QPointF(lineOffsetX, blockRect.top() + line.y()) + positions[gi];
                        const QPainterPath glyphPath = cachedGlyphPath(rawFont, indexes[gi]);
                        
                        // Draw stroke for ALL glyphs
                        painter->save();
                        painter->translate(glyphPos);
                        painter->setPen(QPen(outlineColor, strokeWidth * 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                        painter->setBrush(Qt::NoBrush);
                        painter->drawPath(glyphPath);
                        painter->restore();
                    }
                    glyphCount += indexes.size();
                }
                
                // Pass 2: Render all fills on top (foreground)
                for (const QGlyphRun& run : glyphRuns) {
                    const QVector<quint32> indexes = run.glyphIndexes();
                    const QVector<QPointF> positions = run.positions();
                    if (indexes.size() != positions.size()) continue;
                    const QRawFont rawFont = run.rawFont();
                    
                    for (int gi = 0; gi < indexes.size(); ++gi) {
                        // Apply horizontal alignment offset to glyph position
                        const QPointF glyphPos = QPointF(lineOffsetX, blockRect.top() + line.y()) + positions[gi];
                        const QPainterPath glyphPath = cachedGlyphPath(rawFont, indexes[gi]);
                        
                        // Draw fill for ALL glyphs
                        painter->save();
                        painter->translate(glyphPos);
                        painter->setPen(Qt::NoPen);
                        painter->setBrush(fillColor);
                        painter->drawPath(glyphPath);
                        painter->restore();
                    }
                }
            } else {
                painter->save();
                painter->setPen(fillColor);
                line.draw(painter, lineBasePos);
                painter->restore();
            }
        }
    }

    painter->restore();
    if (outlineTimerRunning) {
        outlineMs = outlineTimer.elapsed();
    }
    logSnapshotPerf("vector-fit", outlineMs, glyphCount, snapshotTimer.elapsed());
}

QImage TextMediaItem::TextRasterJob::execute() const {
    QElapsedTimer rasterTimer;
    rasterTimer.start();
    const int targetWidth = std::max(1, targetSize.width());
    const int targetHeight = std::max(1, targetSize.height());

    // If targetRect is specified and valid, render only that region
    if (!targetRect.isEmpty() && targetRect.isValid()) {
        // Calculate pixel dimensions for the visible region
        const int regionWidth = std::max(1, static_cast<int>(std::ceil(targetRect.width() * scaleFactor)));
        const int regionHeight = std::max(1, static_cast<int>(std::ceil(targetRect.height() * scaleFactor)));
        
        // Create image for visible region only
        QImage result(regionWidth, regionHeight, QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::transparent);

        QPainter imagePainter(&result);
        imagePainter.setRenderHint(QPainter::Antialiasing, true);
        imagePainter.setRenderHint(QPainter::TextAntialiasing, true);
        imagePainter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        // Translate to render only the visible region
        // targetRect is in item coordinates, we need to offset by its top-left
        imagePainter.translate(-targetRect.left() * scaleFactor, -targetRect.top() * scaleFactor);

        // Tier 2: Pass canvasZoom for Phase 8 LOD, targetRect as viewport for Phase 7 culling
        TextMediaItem::paintVectorSnapshot(&imagePainter, snapshot, targetSize, scaleFactor, canvasZoom, targetRect);
        imagePainter.end();

        const qint64 renderMs = rasterTimer.elapsed();
        if ((snapshot.outlineWidthPercent > 0.0 || renderMs > 8) && textHotLogsEnabled()) {
            qDebug() << "[TextMediaItem][RasterJob]"
                     << "region" << QSize(regionWidth, regionHeight)
                     << "scale" << scaleFactor
                     << "strokePercent" << snapshot.outlineWidthPercent
                     << "durationMs" << renderMs;
        }

        return result;
    }

    // Fallback: render full image (original behavior)
    QImage result(targetWidth, targetHeight, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    QPainter imagePainter(&result);
    imagePainter.setRenderHint(QPainter::Antialiasing, true);
    imagePainter.setRenderHint(QPainter::TextAntialiasing, true);
    imagePainter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Tier 2: Pass canvasZoom for Phase 8 LOD selection
    TextMediaItem::paintVectorSnapshot(&imagePainter, snapshot, targetSize, scaleFactor, canvasZoom);
    imagePainter.end();

    const qint64 renderMs = rasterTimer.elapsed();
    if ((snapshot.outlineWidthPercent > 0.0 || renderMs > 8) && textHotLogsEnabled()) {
        qDebug() << "[TextMediaItem][RasterJob]"
                 << "region" << QSize(targetWidth, targetHeight)
                 << "scale" << scaleFactor
                 << "strokePercent" << snapshot.outlineWidthPercent
                 << "durationMs" << renderMs;
    }

    return result;
}

TextMediaItem::TextMediaItem(
    const QSize& initialSize,
    int visualSizePx,
    int selectionSizePx,
    const QString& initialText
)
    : ResizableMediaBase(initialSize, visualSizePx, selectionSizePx, QStringLiteral("Text"))
    , m_text(initialText)
    , m_textColor(TextMediaDefaults::TEXT_COLOR)
    , m_textBorderWidthPercent(TextMediaDefaults::TEXT_BORDER_WIDTH_PERCENT)
    , m_textBorderColor(TextMediaDefaults::TEXT_BORDER_COLOR)
    , m_highlightEnabled(TextMediaDefaults::TEXT_HIGHLIGHT_ENABLED)
    , m_highlightColor(TextMediaDefaults::TEXT_HIGHLIGHT_COLOR)
{
    // Set up default font from global configuration
    // Try system font first, with fallbacks to fonts known to have good weight support
    QStringList fontCandidates = {
        TextMediaDefaults::FONT_FAMILY,
        QStringLiteral(".SF NS Text"),          // macOS system font (alternate name)
        QStringLiteral("Helvetica Neue"),       // Good weight support on macOS
        QStringLiteral("Segoe UI"),             // Good weight support on Windows
        QStringLiteral("Roboto"),               // Good weight support cross-platform
        QStringLiteral("Arial")                 // Final fallback
    };

    QString selectedFamily = fontCandidates.value(0, TextMediaDefaults::FONT_FAMILY);
    for (const QString& candidate : fontCandidates) {
        if (QFontDatabase::hasFamily(candidate)) {
            selectedFamily = candidate;
            break;
        }
    }

    m_font = QFont(selectedFamily, TextMediaDefaults::FONT_SIZE);
    m_font.setItalic(TextMediaDefaults::FONT_ITALIC);
    m_font.setUnderline(TextMediaDefaults::FONT_UNDERLINE);
    m_font.setCapitalization(TextMediaDefaults::FONT_ALL_CAPS ? QFont::AllUppercase : QFont::MixedCase);
    m_italicEnabled = m_font.italic();
    m_underlineEnabled = m_font.underline();
    m_uppercaseEnabled = (m_font.capitalization() == QFont::AllUppercase);
    m_fontWeightValue = clampCssWeight(TextMediaDefaults::FONT_WEIGHT_VALUE);
    m_font = fontAdjustedForWeight(m_font, m_fontWeightValue);
    m_fontWeightValue = canonicalCssWeight(m_font);
    m_font = ensureRenderableFont(m_font, mediaId(), "ctor");
    
    // Text media should be selectable and movable
    setFlag(QGraphicsItem::ItemIsSelectable, true);
    setFlag(QGraphicsItem::ItemIsMovable, true);
    setAcceptHoverEvents(true);

    m_wasMovableBeforeEditing = flags().testFlag(QGraphicsItem::ItemIsMovable);

    m_cachedTextColor = m_textColor;
    m_cachedDocumentSize = QSizeF(1.0, 1.0);
    m_cachedIdealWidth = -1.0;
    m_cachedTextWidth = -1.0;
    m_cachedEditorOpacity = -1.0;
    m_cachedEditorPosValid = false;
    m_cachedEditorPos = QPointF();
    m_documentMetricsDirty = true;
    
    // Initialize rasterization state
    m_needsRasterization = true;
    m_lastRasterizedSize = QSize(0, 0);

    m_editorRenderingText = m_text;
    m_uniformScaleFactor = scale();
    m_lastObservedScale = scale();
    m_lastVisualScaleFactor = 1.0;
    m_appliedContentPaddingPx = contentPaddingPx();
    
    // Create alignment controls (will be shown when selected)
    ensureAlignmentControls();
    ensureTextRenderer();

    setFitToTextEnabled(true);
}

void TextMediaItem::setText(const QString& text) {
    if (m_text != text) {
        // Phase 4: Compute text diff before updating
        const int currentWidth = static_cast<int>(boundingRect().width());
        TextEditDiff diff = computeTextDiff(text, m_font, currentWidth);
        
        m_text = text;
        m_editorRenderingText = text;
        m_documentMetricsDirty = true;
        m_cachedEditorPosValid = false;
        invalidateRenderPipeline(InvalidationReason::Content, true);
        m_frozenFallbackValid = false;  // Phase 3: Invalidate fallback when text changes
        
        // Phase 4: Apply diff-based cache invalidation
        applyTextDiff(diff);
        
        if (m_inlineEditor && !m_isEditing) {
            QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);
            m_inlineEditor->setPlainText(m_text);
            m_documentMetricsDirty = true;
            m_cachedEditorPosValid = false;
            if (auto* inlineEditor = toInlineEditor(m_inlineEditor)) {
                inlineEditor->invalidateCache();
            }
        }
        updateInlineEditorGeometry();
        update(); // Trigger repaint
        updateOverlayLayout();
        if (m_fitToTextEnabled) {
            scheduleFitToTextUpdate();
        }
        m_waitingForHighResRaster = false;
        m_pendingHighResScale = 0.0;
        updateStrokePreviewState(m_textBorderWidthPercent);
    }
}

void TextMediaItem::setFont(const QFont& font) {
    QFont adjustedFont = font;
    m_fontWeightValue = canonicalCssWeight(adjustedFont);
    adjustedFont = fontAdjustedForWeight(adjustedFont, m_fontWeightValue);
    adjustedFont = ensureRenderableFont(adjustedFont, mediaId(), "setFont");
    m_frozenFallbackValid = false;  // Phase 3: Invalidate fallback when font changes
    
    // Phase 3: Font change invalidates atlas (glyphs need re-render)
    clearAtlases();
    // Phase 5: Font change invalidates base raster (geometric change)
    m_baseRasterValid = false;
    m_incrementalRenderValid = false;
    
    applyFontChange(adjustedFont);
}

void TextMediaItem::setTextColor(const QColor& color) {
    if (m_textColor == color) {
        return;
    }

    m_textColor = color;
    m_cachedTextColor = color;
    m_frozenFallbackValid = false;  // Phase 3: Invalidate fallback when text color changes
    
    // Phase 3: Color change invalidates atlas (glyphs use baked-in colors)
    clearAtlases();
    // Phase 5: Color-only change can reuse geometry but needs overlay re-render
    m_incrementalRenderValid = false;
    
    update();
    if (m_inlineEditor) {
        m_inlineEditor->setDefaultTextColor(m_textColor);
        const qreal strokeWidth = borderStrokeWidthPx();
        normalizeTextFormatting(m_inlineEditor, m_font, m_textColor, m_textBorderColor, strokeWidth);
        if (auto* inlineEditor = toInlineEditor(m_inlineEditor)) {
            inlineEditor->invalidateCache();
        }
    }
    invalidateRenderPipeline(InvalidationReason::VisualStyle, false);
}

void TextMediaItem::setTextColorOverrideEnabled(bool enabled) {
    if (m_textColorOverrideEnabled == enabled) {
        return;
    }
    m_textColorOverrideEnabled = enabled;
    if (!enabled) {
        setTextColor(TextMediaDefaults::TEXT_COLOR);
    }
}

void TextMediaItem::setTextBorderWidth(qreal percent) {
    const qreal clamped = std::clamp(percent, 0.0, 100.0);
    if (std::abs(m_textBorderWidthPercent - clamped) < 1e-4) {
        return;
    }

    const qreal oldPadding = m_appliedContentPaddingPx;
    if (textHotLogsEnabled()) {
        qDebug() << "[TextMediaItem]" << mediaId() << "setTextBorderWidth%" << clamped
                 << "approxPx" << computeStrokeWidthFromFont(m_font, clamped)
                 << "textLength" << m_text.size();
    }
    m_textBorderWidthPercent = clamped;
    m_waitingForHighResRaster = false;
    m_pendingHighResScale = 0.0;
    updateStrokePreviewState(clamped);
    const qreal newPadding = contentPaddingPx();

    invalidateRenderPipeline(InvalidationReason::VisualStyle, false);
    m_frozenFallbackValid = false;  // Phase 3: Invalidate fallback when border width changes
    update();

    handleContentPaddingChanged(oldPadding, newPadding);
    m_appliedContentPaddingPx = newPadding;

    if (m_inlineEditor) {
        normalizeTextFormatting(m_inlineEditor, m_font, m_textColor, m_textBorderColor, borderStrokeWidthPx());
        if (auto* inlineEditor = toInlineEditor(m_inlineEditor)) {
            inlineEditor->invalidateCache();
        }
    }
}

void TextMediaItem::setTextBorderWidthOverrideEnabled(bool enabled) {
    if (m_textBorderWidthOverrideEnabled == enabled) {
        return;
    }
    m_textBorderWidthOverrideEnabled = enabled;
    if (!enabled) {
        setTextBorderWidth(TextMediaDefaults::TEXT_BORDER_WIDTH_PERCENT);
    }
}

void TextMediaItem::setTextBorderColor(const QColor& color) {
    if (!color.isValid()) {
        return;
    }
    if (m_textBorderColor == color) {
        return;
    }

    m_textBorderColor = color;
    invalidateRenderPipeline(InvalidationReason::VisualStyle, false);
    m_frozenFallbackValid = false;  // Phase 3: Invalidate fallback when border color changes
    update();

    if (m_inlineEditor) {
        const qreal strokeWidth = borderStrokeWidthPx();
        normalizeTextFormatting(m_inlineEditor, m_font, m_textColor, m_textBorderColor, strokeWidth);
        if (auto* inlineEditor = toInlineEditor(m_inlineEditor)) {
            inlineEditor->invalidateCache();
        }
    }
}

void TextMediaItem::setTextBorderColorOverrideEnabled(bool enabled) {
    if (m_textBorderColorOverrideEnabled == enabled) {
        return;
    }
    m_textBorderColorOverrideEnabled = enabled;
    if (!enabled) {
        setTextBorderColor(TextMediaDefaults::TEXT_BORDER_COLOR);
    }
}

void TextMediaItem::setHighlightEnabled(bool enabled) {
    if (m_highlightEnabled == enabled) {
        return;
    }

    m_highlightEnabled = enabled;
    invalidateRenderPipeline(InvalidationReason::VisualStyle, false);
    m_frozenFallbackValid = false;  // Phase 3: Invalidate fallback when highlight toggled
    update();

    if (m_inlineEditor) {
        if (auto* inlineEditor = toInlineEditor(m_inlineEditor)) {
            inlineEditor->invalidateCache();
        }
    }
}

void TextMediaItem::setHighlightColor(const QColor& color) {
    QColor normalized = color;
    if (!normalized.isValid()) {
        normalized = TextMediaDefaults::TEXT_HIGHLIGHT_COLOR;
    }

    if (m_highlightColor == normalized) {
        return;
    }

    m_highlightColor = normalized;
    invalidateRenderPipeline(InvalidationReason::VisualStyle, false);
    m_frozenFallbackValid = false;  // Phase 3: Invalidate fallback when highlight color changes
    update();

    if (m_inlineEditor) {
        if (auto* inlineEditor = toInlineEditor(m_inlineEditor)) {
            inlineEditor->invalidateCache();
        }
    }
}

qreal TextMediaItem::borderStrokeWidthPx() const {
    return computeStrokeWidthFromFont(m_font, m_textBorderWidthPercent);
}

qreal TextMediaItem::contentPaddingPx() const {
    return kContentPadding;
}

void TextMediaItem::handleContentPaddingChanged(qreal oldPadding, qreal newPadding) {
    if (std::abs(newPadding - oldPadding) < 1e-3) {
        m_appliedContentPaddingPx = newPadding;
        return;
    }

    const qreal uniformScale = std::max(std::abs(m_uniformScaleFactor), 1e-4);
    const qreal oldPaddingScene = oldPadding * uniformScale;
    const qreal newPaddingScene = newPadding * uniformScale;

    const qreal contentWidth = std::max<qreal>(1.0, static_cast<qreal>(m_baseSize.width()) - oldPaddingScene * 2.0);
    const qreal contentHeight = std::max<qreal>(1.0, static_cast<qreal>(m_baseSize.height()) - oldPaddingScene * 2.0);

    const int newBaseWidth = std::max(1, static_cast<int>(std::ceil(contentWidth + newPaddingScene * 2.0)));
    const int newBaseHeight = std::max(1, static_cast<int>(std::ceil(contentHeight + newPaddingScene * 2.0)));
    const QSize newBase(newBaseWidth, newBaseHeight);

    if (newBase == m_baseSize) {
        m_documentMetricsDirty = true;
        m_cachedEditorPosValid = false;
        syncInlineEditorToBaseSize();
        updateInlineEditorGeometry();
        updateAlignmentControlsLayout();
        updateOverlayLayout();
        update();
        m_appliedContentPaddingPx = newPadding;
        return;
    }

    auto anchorPointForSize = [this](const QSize& size) {
        qreal x = 0.0;
        switch (m_horizontalAlignment) {
            case HorizontalAlignment::Left:
                x = 0.0;
                break;
            case HorizontalAlignment::Center:
                x = static_cast<qreal>(size.width()) * 0.5;
                break;
            case HorizontalAlignment::Right:
                x = static_cast<qreal>(size.width());
                break;
        }

        qreal y = 0.0;
        switch (m_verticalAlignment) {
            case VerticalAlignment::Top:
                y = 0.0;
                break;
            case VerticalAlignment::Center:
                y = static_cast<qreal>(size.height()) * 0.5;
                break;
            case VerticalAlignment::Bottom:
                y = static_cast<qreal>(size.height());
                break;
        }
        return QPointF(x, y);
    };

    const QSize oldBase = m_baseSize;
    const QPointF oldAnchorLocal = anchorPointForSize(oldBase);
    QPointF anchorBefore;
    if (parentItem()) {
        anchorBefore = mapToParent(oldAnchorLocal);
    } else if (scene()) {
        anchorBefore = mapToScene(oldAnchorLocal);
    } else {
        anchorBefore = oldAnchorLocal + pos();
    }

    prepareGeometryChange();
    m_baseSize = newBase;
    invalidateRenderPipeline(InvalidationReason::Geometry, true);
    m_lastRasterizedScale = 1.0;
    m_cachedEditorPosValid = false;
    m_documentMetricsDirty = true;
    m_cachedIdealWidth = -1.0;
    m_cachedDocumentSize = QSizeF();

    const QPointF newAnchorLocal = anchorPointForSize(newBase);
    QPointF anchorAfter;
    if (parentItem()) {
        anchorAfter = mapToParent(newAnchorLocal);
    } else if (scene()) {
        anchorAfter = mapToScene(newAnchorLocal);
    } else {
        anchorAfter = newAnchorLocal + pos();
    }

    const QPointF delta = anchorBefore - anchorAfter;
    if (!delta.isNull()) {
        setPos(pos() + delta);
    }

    syncInlineEditorToBaseSize();
    updateInlineEditorGeometry();
    updateAlignmentControlsLayout();
    updateOverlayLayout();
    update();

    if (m_fitToTextEnabled) {
        scheduleFitToTextUpdate();
    }
}

void TextMediaItem::setTextFontWeightValue(int weight, bool markOverride) {
    const int clamped = clampCssWeight(weight);
    const bool shouldApply = (m_fontWeightValue != clamped) || !markOverride;
    if (markOverride) {
        m_fontWeightOverrideEnabled = true;
    }
    if (!shouldApply) {
        return;
    }

    m_fontWeightValue = clamped;
    QFont updatedFont = fontAdjustedForWeight(m_font, m_fontWeightValue);
    applyFontChange(updatedFont);
}

void TextMediaItem::setTextFontWeightOverrideEnabled(bool enabled) {
    if (m_fontWeightOverrideEnabled == enabled) {
        return;
    }
    m_fontWeightOverrideEnabled = enabled;
    if (!enabled) {
        setTextFontWeightValue(TextMediaDefaults::FONT_WEIGHT_VALUE, /*markOverride*/false);
    }
}

void TextMediaItem::setItalicEnabled(bool enabled) {
    if (m_italicEnabled == enabled) {
        return;
    }
    m_italicEnabled = enabled;
    QFont updatedFont = m_font;
    updatedFont.setItalic(enabled);
    applyFontChange(updatedFont);
}

void TextMediaItem::setUnderlineEnabled(bool enabled) {
    if (m_underlineEnabled == enabled) {
        return;
    }
    m_underlineEnabled = enabled;
    QFont updatedFont = m_font;
    updatedFont.setUnderline(enabled);
    applyFontChange(updatedFont);
}

void TextMediaItem::setUppercaseEnabled(bool enabled) {
    if (m_uppercaseEnabled == enabled) {
        return;
    }
    m_uppercaseEnabled = enabled;
    QFont updatedFont = m_font;
    updatedFont.setCapitalization(enabled ? QFont::AllUppercase : QFont::MixedCase);
    applyFontChange(updatedFont);
}

void TextMediaItem::applyFontChange(const QFont& font) {
    QFont sanitizedFont = ensureRenderableFont(font, mediaId(), "applyFontChange");
    // Don't skip update for fit-to-text mode since weight changes might not be detected by QFont equality
    const bool fontChanged = (m_font != sanitizedFont);
    if (!fontChanged && !m_fitToTextEnabled) {
        return;
    }

    const qreal oldPadding = m_appliedContentPaddingPx;
    m_font = sanitizedFont;
    m_italicEnabled = m_font.italic();
    m_underlineEnabled = m_font.underline();
    m_uppercaseEnabled = (m_font.capitalization() == QFont::AllUppercase);
    m_fontWeightValue = canonicalCssWeight(m_font);
    const qreal newPadding = contentPaddingPx();

    m_documentMetricsDirty = true;
    m_cachedEditorPosValid = false;
    invalidateRenderPipeline(InvalidationReason::Geometry, true);
    
    // Don't clear the scaled raster immediately - let ensureScaledRaster handle the transition
    // to avoid showing empty frames during font changes
    
    update();

    handleContentPaddingChanged(oldPadding, newPadding);

    if (m_inlineEditor) {
        m_inlineEditor->setFont(m_font);
        // Always normalize text formatting when font changes, not just when editing
        // This ensures the document's character formats match the new font for fit-to-text measurements
        const qreal strokeWidth = borderStrokeWidthPx();
        normalizeTextFormatting(m_inlineEditor, m_font, m_textColor, m_textBorderColor, strokeWidth);
        if (auto* inlineEditor = toInlineEditor(m_inlineEditor)) {
            inlineEditor->invalidateCache();
        }
    }

    if (m_fitToTextEnabled) {
        scheduleFitToTextUpdate();
    }
}

void TextMediaItem::setHorizontalAlignment(HorizontalAlignment align) {
    if (m_horizontalAlignment == align) return;
    m_horizontalAlignment = align;
    invalidateRenderPipeline(InvalidationReason::Geometry, true);
    applyAlignmentToEditor(); // Update inline editor alignment
    update();
    updateAlignmentButtonStates();
    if (m_fitToTextEnabled) {
        scheduleFitToTextUpdate();
    }
}

void TextMediaItem::setVerticalAlignment(VerticalAlignment align) {
    if (m_verticalAlignment == align) return;
    m_verticalAlignment = align;
    invalidateRenderPipeline(InvalidationReason::Geometry, true);
    m_cachedEditorPosValid = false; // Force recalculation of editor position
    if (m_inlineEditor) {
        updateInlineEditorGeometry(); // Update editor position immediately
    }
    update();
    updateAlignmentButtonStates();
}

void TextMediaItem::normalizeEditorFormatting() {
    if (!m_inlineEditor || !m_isEditing) {
        return;
    }
    // Use base font (transform will scale it)
    normalizeTextFormatting(m_inlineEditor, m_font, m_textColor, m_textBorderColor, borderStrokeWidthPx());
    m_documentMetricsDirty = true;
    m_cachedEditorPosValid = false;
}

bool TextMediaItem::beginInlineEditing() {
    if (m_isEditing) {
        if (m_inlineEditor) {
            m_inlineEditor->setFocus(Qt::OtherFocusReason);
            QTextCursor cursor = m_inlineEditor->textCursor();
            cursor.select(QTextCursor::Document);
            m_inlineEditor->setTextCursor(cursor);
        }
        return true;
    }

    ensureInlineEditor();
    if (!m_inlineEditor) {
        return false;
    }

    m_wasMovableBeforeEditing = flags().testFlag(QGraphicsItem::ItemIsMovable);
    setFlag(QGraphicsItem::ItemIsMovable, false);

    m_isEditing = true;
    m_textBeforeEditing = m_text;
    m_editorRenderingText = m_text;

    {
        QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);
        m_inlineEditor->setPlainText(m_text);
    }
    m_documentMetricsDirty = true;
    m_cachedEditorPosValid = false;
    m_inlineEditor->setDefaultTextColor(m_textColor);
    // Use base font (transform will scale it)
    m_inlineEditor->setFont(m_font);
    m_inlineEditor->setEnabled(true);
    m_inlineEditor->setVisible(true);
    m_inlineEditor->setTextInteractionFlags(Qt::TextEditorInteraction);

    // Ensure editing is always visually available, even if the display layer
    // was previously faded out by transition logic.
    m_contentVisible = true;
    m_contentDisplayOpacity = 1.0;
    
    syncInlineEditorToBaseSize();
    applyAlignmentToEditor(); // Apply current alignment

    updateInlineEditorGeometry();

    // Normalize all text formatting to use current font (includes updated size from resize)
    normalizeEditorFormatting();

    m_inlineEditor->setFocus(Qt::OtherFocusReason);
    QTextCursor cursor = m_inlineEditor->textCursor();
    cursor.select(QTextCursor::Document);
    m_inlineEditor->setTextCursor(cursor);
    if (auto* inlineEditor = toInlineEditor(m_inlineEditor)) {
        inlineEditor->invalidateCache();
    }

    update();
    return true;
}

void TextMediaItem::commitInlineEditing() {
    if (!m_isEditing) {
        return;
    }
    finishInlineEditing(true);
}

void TextMediaItem::cancelInlineEditing() {
    if (!m_isEditing) {
        return;
    }
    finishInlineEditing(false);
}

void TextMediaItem::ensureInlineEditor() {
    if (m_inlineEditor) {
        return;
    }

    auto* editor = new InlineTextEditor(this);
    editor->setDefaultTextColor(m_textColor);
    editor->setFont(m_font);
    editor->setEnabled(false);
    editor->setVisible(false);

    if (QTextDocument* doc = editor->document()) {
        {
            QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);
            doc->setDocumentMargin(0.0);
            QTextOption opt = doc->defaultTextOption();
            opt.setWrapMode(QTextOption::WordWrap);
            // Apply current horizontal alignment
            Qt::Alignment qtAlign = Qt::AlignHCenter;
            switch (m_horizontalAlignment) {
                case HorizontalAlignment::Left:
                    qtAlign = Qt::AlignLeft;
                    break;
                case HorizontalAlignment::Center:
                    qtAlign = Qt::AlignHCenter;
                    break;
                case HorizontalAlignment::Right:
                    qtAlign = Qt::AlignRight;
                    break;
            }
            opt.setAlignment(qtAlign);
            doc->setDefaultTextOption(opt);
            // Set width at base size (transform will scale it visually)
            const qreal margin = contentPaddingPx();
            const qreal uniformScale = std::max(std::abs(m_uniformScaleFactor), 1e-4);
            const qreal logicalWidth = std::max<qreal>(1.0, static_cast<qreal>(m_baseSize.width()) / uniformScale - 2.0 * margin);
            doc->setTextWidth(logicalWidth);
        }

        QObject::connect(doc, &QTextDocument::contentsChanged, editor, [this, editor]() {
            if (m_ignoreDocumentChange) {
                return;
            }
            
            // Check if content actually changed (not just cursor movement)
            const QString newText = editor->toPlainText();
            const bool contentChanged = (m_editorRenderingText != newText);
            
            editor->invalidateCache();
            m_documentMetricsDirty = true;
            m_cachedEditorPosValid = false;
            
            if (contentChanged) {
                handleInlineEditorTextChanged(newText);
            }
            
            if (m_isUpdatingInlineGeometry) {
                return;
            }
            
            // Only update geometry if content changed (not just cursor movement)
            if (contentChanged) {
                updateInlineEditorGeometry();
            }

            if (contentChanged && m_fitToTextEnabled) {
                scheduleFitToTextUpdate();
            }
        });
    }

    {
        QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);
        // Apply current alignment (not hardcoded center)
        Qt::Alignment qtAlign = Qt::AlignHCenter;
        switch (m_horizontalAlignment) {
            case HorizontalAlignment::Left:
                qtAlign = Qt::AlignLeft;
                break;
            case HorizontalAlignment::Center:
                qtAlign = Qt::AlignHCenter;
                break;
            case HorizontalAlignment::Right:
                qtAlign = Qt::AlignRight;
                break;
        }
        applyTextAlignment(editor, qtAlign);
    }

    m_inlineEditor = editor;
    editor->invalidateCache();

    applyFitModeConstraintsToEditor();
}

void TextMediaItem::handleInlineEditorTextChanged(const QString& newText) {
    if (!m_isEditing) {
        return;
    }

    if (m_editorRenderingText == newText) {
        return;
    }

    m_editorRenderingText = newText;
    invalidateRenderPipeline(InvalidationReason::Content, false);
    m_waitingForHighResRaster = false;
    m_pendingHighResScale = 0.0;
    updateStrokePreviewState(m_textBorderWidthPercent);
    update();
}

const QString& TextMediaItem::textForRendering() const {
    return m_editorRenderingText;
}

void TextMediaItem::updateInlineEditorGeometry() {
    if (!m_inlineEditor) {
        return;
    }

    if (m_isUpdatingInlineGeometry) {
        return;
    }
    QScopedValueRollback<bool> guard(m_isUpdatingInlineGeometry, true);

    auto floatsDiffer = [](qreal a, qreal b, qreal epsilon = 0.1) {
        return std::abs(a - b) > epsilon;
    };

    const qreal uniformScale = std::max(std::abs(m_uniformScaleFactor), 1e-4);
    const qreal marginLogical = contentPaddingPx();
    const qreal marginScene = marginLogical * uniformScale;

    QTransform editorTransform;
    if (std::abs(uniformScale - 1.0) > 1e-4) {
        editorTransform.scale(uniformScale, uniformScale);
    }
    if (m_inlineEditor->transform() != editorTransform) {
        m_inlineEditor->setTransform(editorTransform);
    }

    QFont editorFont = m_font;
    if (m_inlineEditor->font() != editorFont) {
        m_inlineEditor->setFont(editorFont);
        m_documentMetricsDirty = true;
    }

    if (m_cachedTextColor != m_textColor) {
        m_inlineEditor->setDefaultTextColor(m_textColor);
        m_cachedTextColor = m_textColor;
    }

    const qreal targetOpacity = m_contentOpacity * m_contentDisplayOpacity;
    if (m_cachedEditorOpacity < 0.0 || floatsDiffer(targetOpacity, m_cachedEditorOpacity, 1e-4)) {
        m_inlineEditor->setOpacity(targetOpacity);
        m_cachedEditorOpacity = targetOpacity;
    }

    QRectF bounds = boundingRect();
    
    // Match the rendering logic: divide base size by scale first, then subtract padding
    // This ensures the inline editor wraps at exactly the same width as the visible text
    const qreal logicalWidth = std::max<qreal>(1.0, bounds.width() / uniformScale);
    const qreal logicalHeight = std::max<qreal>(1.0, bounds.height() / uniformScale);
    const qreal logicalContentWidth = std::max<qreal>(1.0, logicalWidth - 2.0 * marginLogical);
    const qreal logicalContentHeight = std::max<qreal>(1.0, logicalHeight - 2.0 * marginLogical);
    
    // Calculate visual dimensions for positioning
    QRectF contentRect = bounds.adjusted(marginScene, marginScene, -marginScene, -marginScene);
    if (contentRect.width() < 1.0) {
        contentRect.setWidth(1.0);
    }
    if (contentRect.height() < 1.0) {
        contentRect.setHeight(1.0);
    }
    const qreal visualContentWidth = contentRect.width();
    const qreal visualContentHeight = contentRect.height();

    bool widthChanged = false;
    if (!m_fitToTextEnabled) {
        if (m_cachedTextWidth < 0.0 || floatsDiffer(logicalContentWidth, m_cachedTextWidth, 1e-3)) {
            m_inlineEditor->setTextWidth(logicalContentWidth);
            m_cachedTextWidth = logicalContentWidth;
            widthChanged = true;
            m_documentMetricsDirty = true;
        }
    } else {
        widthChanged = true;
        m_cachedTextWidth = -1.0;
        m_documentMetricsDirty = true;
    }

    QTextDocument* doc = m_inlineEditor->document();
    qreal docIdealWidth = m_cachedIdealWidth;
    qreal logicalDocHeight = std::max<qreal>(1.0, m_cachedDocumentSize.height());
    qreal logicalDocTop = 0.0;

    const bool hasReusableLayoutSnapshot = canReuseLayoutSnapshot(textForRendering(), m_font, logicalContentWidth);
    const bool needMetrics = doc && (widthChanged || m_documentMetricsDirty || m_cachedIdealWidth < 0.0 || !hasReusableLayoutSnapshot);
    if (needMetrics) {
        docIdealWidth = doc ? doc->idealWidth() : logicalContentWidth;
        QAbstractTextDocumentLayout* layout = doc ? doc->documentLayout() : nullptr;
        int lineCount = 0;
        if (doc && layout) {
            QSizeF size = layout->documentSize();
            if (size.width() <= 0.0) {
                size.setWidth(logicalContentWidth);
            }
            if (size.height() <= 0.0) {
                size.setHeight(logicalContentHeight);
            }
            const QRectF docBounds = computeDocumentTextBounds(*doc, layout);
            m_cachedDocumentSize = size;
            logicalDocHeight = std::max<qreal>(1.0, docBounds.height());
            logicalDocTop = docBounds.top();
            for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
                if (QTextLayout* textLayout = block.layout()) {
                    lineCount += textLayout->lineCount();
                }
            }
        } else {
            m_cachedDocumentSize = QSizeF(logicalContentWidth, logicalContentHeight);
            logicalDocHeight = std::max<qreal>(1.0, logicalContentHeight);
            logicalDocTop = 0.0;
        }
        m_cachedIdealWidth = docIdealWidth;
        updateLayoutSnapshot(textForRendering(), m_font, logicalContentWidth, m_cachedDocumentSize, m_cachedIdealWidth, lineCount);
        m_documentMetricsDirty = false;
    } else {
        if (hasReusableLayoutSnapshot && m_layoutSnapshot.valid) {
            m_cachedDocumentSize = m_layoutSnapshot.docSize;
            m_cachedIdealWidth = m_layoutSnapshot.idealWidth;
        }
        if (m_cachedIdealWidth < 0.0) {
            docIdealWidth = logicalContentWidth;
        }
        logicalDocHeight = std::max<qreal>(1.0, m_cachedDocumentSize.height());
        if (doc) {
            if (QAbstractTextDocumentLayout* layout = doc->documentLayout()) {
                const QRectF docBounds = computeDocumentTextBounds(*doc, layout);
                logicalDocHeight = std::max<qreal>(1.0, docBounds.height());
                logicalDocTop = docBounds.top();
            }
        }
    }

    const qreal visualDocWidth = std::max<qreal>(1.0, m_cachedDocumentSize.width() * uniformScale);
    const qreal visualDocHeight = std::max<qreal>(1.0, logicalDocHeight * uniformScale);
    const qreal visualDocTop = logicalDocTop * uniformScale;

    const qreal availableWidth = std::max<qreal>(1.0, contentRect.width());
    const qreal availableHeight = std::max<qreal>(1.0, contentRect.height());
    const qreal offsetX = (availableWidth - visualDocWidth) * 0.5;
    
    // Apply vertical alignment
    qreal offsetY = 0.0;
    switch (m_verticalAlignment) {
        case VerticalAlignment::Top:
            offsetY = -visualDocTop;
            break;
        case VerticalAlignment::Center:
            offsetY = (availableHeight - visualDocHeight) * 0.5 - visualDocTop;
            break;
        case VerticalAlignment::Bottom:
            offsetY = availableHeight - visualDocHeight - visualDocTop;
            break;
    }
    
    const QPointF newEditorPos = contentRect.topLeft() + QPointF(offsetX, offsetY);

    if (!m_cachedEditorPosValid || floatsDiffer(newEditorPos.x(), m_cachedEditorPos.x(), 0.1) || floatsDiffer(newEditorPos.y(), m_cachedEditorPos.y(), 0.1)) {
        m_inlineEditor->setPos(newEditorPos);
        m_cachedEditorPos = newEditorPos;
        m_cachedEditorPosValid = true;
    }

    m_inlineEditor->setFlag(QGraphicsItem::ItemClipsToShape, true);
}

void TextMediaItem::onInteractiveGeometryChanged() {
    ResizableMediaBase::onInteractiveGeometryChanged();

    if (m_isEditing) {
        updateInlineEditorGeometry();
    }
    else {
        // Keep hidden editor in sync with base size so entering edit mode wraps correctly
        syncInlineEditorToBaseSize();
    }
    
    // Update alignment controls position (similar to video controls)
    updateAlignmentControlsLayout();
    
    m_scaledRasterDirty = true;
    update();

    updateStrokePreviewState(m_textBorderWidthPercent);
}

void TextMediaItem::onOverlayLayoutUpdated() {
    updateAlignmentControlsLayout();
}

bool TextMediaItem::onAltResizeModeEngaged() {
    // Entering Alt-resize should not rebake scale for text items.
    // Re-baking to scale(1.0) caused a visible text-size jump when switching
    // from a prior uniform resize to Alt-resize. Keep the current item scale
    // and let ResizableMediaBase adjust base size relative to that scale.

    // When user starts Alt-resize (axis-specific stretching), disable fit-to-text mode
    // so container dimensions can be adjusted freely.
    if (m_fitToTextEnabled) {
        setFitToTextEnabled(false);
    }

    m_scaledRasterDirty = true;
    update();
    return true;
}

void TextMediaItem::refreshAlignmentControlsLayout() {
    updateAlignmentControlsLayout();
}

void TextMediaItem::syncInlineEditorToBaseSize() {
    if (!m_inlineEditor) {
        return;
    }

    applyFitModeConstraintsToEditor();
    m_cachedEditorPosValid = false;
}

void TextMediaItem::finishInlineEditing(bool commitChanges) {
    if (!m_inlineEditor) {
        m_isEditing = false;
        return;
    }

    if (auto* inlineEditor = toInlineEditor(m_inlineEditor)) {
        inlineEditor->disableCaretBlink();
    }

    const QString editedText = m_inlineEditor->toPlainText();

    m_inlineEditor->setTextInteractionFlags(Qt::NoTextInteraction);
    m_inlineEditor->clearFocus();
    m_inlineEditor->setEnabled(false);
    m_inlineEditor->setVisible(false);
    {
        QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);
        m_inlineEditor->setPlainText(m_text);
    }
    m_documentMetricsDirty = true;
    m_cachedEditorPosValid = false;

    m_isEditing = false;
    setFlag(QGraphicsItem::ItemIsMovable, m_wasMovableBeforeEditing);

    if (commitChanges) {
        if (editedText != m_text) {
            setText(editedText);
        } else {
            m_editorRenderingText = m_text;
        }
    } else {
        if (editedText != m_textBeforeEditing) {
            m_editorRenderingText = m_textBeforeEditing;
            invalidateRenderPipeline(InvalidationReason::Content, false);
        } else {
            m_editorRenderingText = m_text;
        }
    }
    m_textBeforeEditing.clear();

    // Rasterize text after editing completes
    invalidateRenderPipeline(InvalidationReason::Content, false);
    
    // Phase 5: Flatten composite overlay to base on commit
    if (commitChanges && m_incrementalRenderValid) {
        flattenCompositeToBase();
    }

    updateInlineEditorGeometry();
    if (m_fitToTextEnabled) {
        scheduleFitToTextUpdate();
    }
    update();
}

void TextMediaItem::renderTextToImage(QImage& target,
                                      const QSize& imageSize,
                                      qreal scaleFactor,
                                      const QRectF& visibleRegion,
                                      StrokeRenderMode mode) {
    ensureTextRenderer();
    TextRasterJob job;
    job.snapshot = captureVectorSnapshot(mode);
    job.targetSize = QSize(std::max(1, imageSize.width()), std::max(1, imageSize.height()));
    job.scaleFactor = scaleFactor;
    job.canvasZoom = 1.0;  // Tier 2: Default to 1.0 for sync rendering
    job.targetRect = visibleRegion;  // ✅ Pass viewport for partial rendering even in sync mode

    target = renderRasterJobWithBackend(job);
}

void TextMediaItem::queueScaledRasterUpdate(qreal visualScaleFactor, qreal geometryScale, qreal canvasZoom) {
    if (textSchedulerV2Enabled()) {
        TextRenderTarget target{visualScaleFactor, geometryScale, canvasZoom};
        m_renderScheduler.requestHighRes(target);
    }

    m_queuedVisualScaleFactor = visualScaleFactor;
    m_queuedGeometryScale = geometryScale;
    m_queuedCanvasZoom = canvasZoom;

    if (m_scaledRasterUpdateQueued) {
        return;
    }

    m_scaledRasterUpdateQueued = true;
    std::weak_ptr<bool> guard = lifetimeGuard();
    QTimer::singleShot(0, [this, guard]() {
        if (guard.expired()) {
            return;
        }
        dispatchQueuedScaledRasterUpdate();
    });
}

void TextMediaItem::dispatchQueuedScaledRasterUpdate() {
    if (!m_scaledRasterUpdateQueued) {
        return;
    }

    m_scaledRasterUpdateQueued = false;
    if (textSchedulerV2Enabled()) {
        const std::optional<TextRenderTarget> present = m_renderScheduler.takePresentTarget();
        if (present.has_value()) {
            m_queuedVisualScaleFactor = present->visualScaleFactor;
            m_queuedGeometryScale = present->geometryScale;
            m_queuedCanvasZoom = present->canvasZoom;
        }
    }

    ensureScaledRaster(m_queuedVisualScaleFactor, m_queuedGeometryScale, m_queuedCanvasZoom);
    if (m_activeHandle == None) {
        ensureFrozenFallbackCache(m_queuedCanvasZoom);
    }

    if (textSchedulerV2Enabled()) {
        m_renderScheduler.markPresented();
    }
}

void TextMediaItem::ensureBasePreviewRaster(const QSize& targetSize) {
    if (targetSize.isEmpty()) {
        return;
    }

    const qreal previewScale = std::clamp(kInitialPreviewScaleMax, 0.1, 1.0);
    const qreal scaleRatio = std::max(previewScale, 1e-3);
    const QSize previewSize(
        std::max(1, static_cast<int>(std::ceil(static_cast<qreal>(targetSize.width()) * scaleRatio))),
        std::max(1, static_cast<int>(std::ceil(static_cast<qreal>(targetSize.height()) * scaleRatio))));

    StrokeRenderMode strokeMode = m_previewStrokeActive ? StrokeRenderMode::Preview : StrokeRenderMode::Normal;
    QImage preview;
    renderTextToImage(preview, previewSize, previewScale, QRectF(), strokeMode);

    if (preview.size() != targetSize) {
        preview = preview.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }

    m_rasterizedText = preview;
    m_baseRasterContentRevision = m_contentRevision;
}

void TextMediaItem::rasterizeText() {
    const QSize targetSize(std::max(1, m_baseSize.width()), std::max(1, m_baseSize.height()));
    const bool sizeChanged = (m_lastRasterizedSize != m_baseSize);
    const bool needsNewRaster = m_needsRasterization || sizeChanged;

    if (!needsNewRaster && !m_baseRasterInProgress) {
        return;
    }

    if (needsNewRaster) {
        ensureBasePreviewRaster(targetSize);
        m_lastRasterizedSize = m_baseSize;
        m_scaledRasterDirty = true;
        m_pendingBaseRasterRequest = targetSize;
        queueBaseRasterDispatch();
        m_needsRasterization = false;
    } else if (m_baseRasterInProgress && m_activeBaseRasterSize != targetSize) {
        m_pendingBaseRasterRequest = targetSize;
        queueBaseRasterDispatch();
    }
}

QRectF TextMediaItem::computeVisibleRegion() const {
    // If no scene or view, fallback to entire item bounds
    if (!scene() || scene()->views().isEmpty()) {
        return boundingRect();
    }
    
    QGraphicsView* view = scene()->views().first();
    if (!view) {
        return boundingRect();
    }
    
    // Get viewport rectangle in scene coordinates
    const QRect viewportRect = view->viewport()->rect();
    const QPolygonF viewportScenePolygon = view->mapToScene(viewportRect);
    const QRectF viewportSceneRect = viewportScenePolygon.boundingRect();
    
    // Get our item's bounding rect in scene coordinates
    const QRectF itemSceneRect = mapRectToScene(boundingRect());
    
    // Compute intersection
    const QRectF visibleSceneRect = viewportSceneRect.intersected(itemSceneRect);
    
    if (visibleSceneRect.isEmpty()) {
        return QRectF(); // Item completely off-screen
    }
    
    // Convert back to item local coordinates
    return mapRectFromScene(visibleSceneRect);
}

void TextMediaItem::ensureScaledRaster(qreal visualScaleFactor, qreal geometryScale, qreal canvasZoom) {
    const qreal epsilon = 1e-4;
    const qreal effectiveScale = std::max(std::abs(visualScaleFactor), epsilon);
    const qreal boundedGeometryScale = std::max(std::abs(geometryScale), epsilon);
    const qreal boundedCanvasZoom = std::max(std::abs(canvasZoom), epsilon);
    const qreal boundedUniformScale = std::max(std::abs(m_uniformScaleFactor), epsilon);
    const bool resizingUniformly = (m_activeHandle != None);
    const bool disableViewportOptimization = resizingUniformly;
    qreal boundedDpr = 1.0;
    if (scene() && !scene()->views().isEmpty()) {
        if (QGraphicsView* view = scene()->views().first()) {
            boundedDpr = std::max<qreal>(1.0, view->devicePixelRatioF());
        }
    }

    // Compute visible region in item coordinates
    const QRectF visibleRegion = disableViewportOptimization ? boundingRect() : computeVisibleRegion();
    const bool itemOffScreen = visibleRegion.isEmpty();
    
    if (itemOffScreen) {
        // No need to rasterize if completely off-screen
        return;
    }

    // Calculate target size first (needed for cache validation)
    int targetWidth = std::max(1, static_cast<int>(std::ceil(static_cast<qreal>(m_baseSize.width()) * boundedGeometryScale * boundedUniformScale * boundedCanvasZoom * boundedDpr)));
    int targetHeight = std::max(1, static_cast<int>(std::ceil(static_cast<qreal>(m_baseSize.height()) * boundedGeometryScale * boundedUniformScale * boundedCanvasZoom * boundedDpr)));
    qreal rasterScale = effectiveScale * boundedDpr;
    const QSize targetSize(targetWidth, targetHeight);
    const VectorDrawSnapshot cacheSnapshot = captureVectorSnapshot();
    const TextRenderCacheKey targetCacheKey = makeCacheKey(cacheSnapshot, targetSize, rasterScale, boundedDpr);

    // Étape 3: Cache management - check if viewport shift is significant
    // If we already have a raster and the viewport hasn't changed much, keep the cache
    bool viewportShiftedSignificantly = true;
    bool targetSizeChanged = (m_lastScaledTargetSize != targetSize);
    bool baseSizeChanged = (m_lastScaledBaseSize != m_baseSize);
    const bool forceRefresh = m_forceScaledRasterRefresh;
    
    if (!disableViewportOptimization && !forceRefresh && !m_lastViewportRect.isEmpty() && !visibleRegion.isEmpty() && m_scaledRasterPixmapValid && !targetSizeChanged && !baseSizeChanged) {
        // Check if scale changed significantly
        const qreal scaleEpsilon = 0.01; // 1% tolerance
        const bool scaleUnchanged = std::abs(boundedCanvasZoom - m_lastViewportScale) < scaleEpsilon;
        
        if (scaleUnchanged) {
            // Calculate overlap between old and new viewport
            const QRectF intersection = m_lastViewportRect.intersected(visibleRegion);
            const QRectF unionRect = m_lastViewportRect.united(visibleRegion);
            
            const qreal intersectionArea = intersection.width() * intersection.height();
            const qreal unionArea = unionRect.width() * unionRect.height();
            
            // If overlap > 70%, consider the viewport unchanged
            const qreal overlapRatio = (unionArea > 0) ? (intersectionArea / unionArea) : 0.0;
            
            if (overlapRatio > 0.7) {
                viewportShiftedSignificantly = false;
                
            }
        }
    }
    
    // If viewport didn't shift significantly AND target size AND base size unchanged, we can skip re-rasterization
    if (!forceRefresh && !viewportShiftedSignificantly && !targetSizeChanged && !baseSizeChanged && !m_scaledRasterDirty && m_scaledRasterPixmapValid) {
        if (!textCachePolicyV2Enabled() || (m_activeHighResCacheKeyValid && cacheKeyMatches(m_activeHighResCacheKey, targetCacheKey))) {
            return;
        }
    }

    if (!m_scaledRasterDirty && m_scaledRasterPixmapValid && textCachePolicyV2Enabled() &&
        m_activeHighResCacheKeyValid && cacheKeyMatches(m_activeHighResCacheKey, targetCacheKey) && !m_asyncRasterInProgress) {
        return;
    }

    // Note: No longer applying maxRasterDimension limit here because:
    // 1. With viewport optimization, we only render visible region (much smaller)
    // 2. This limit was causing pixelation during zoom by reducing rasterScale
    // 3. Memory is controlled by viewport size, not full text size
    
    // Log viewport optimization metrics
    const QRectF fullBounds = boundingRect();
    const qreal fullArea = fullBounds.width() * fullBounds.height();
    const qreal visibleArea = visibleRegion.width() * visibleRegion.height();
    const qreal visibleRatio = (fullArea > 0) ? (visibleArea / fullArea) : 1.0;
    
    // Calculate potential pixel savings
    const int fullPixels = targetWidth * targetHeight;
    const int visiblePixels = static_cast<int>(std::ceil(visibleRegion.width() * boundedGeometryScale * boundedUniformScale * boundedCanvasZoom)) *
                               static_cast<int>(std::ceil(visibleRegion.height() * boundedGeometryScale * boundedUniformScale * boundedCanvasZoom));
    const qreal pixelReduction = (fullPixels > 0) ? (static_cast<qreal>(visiblePixels) / static_cast<qreal>(fullPixels)) : 1.0;
    
    Q_UNUSED(fullArea);
    Q_UNUSED(visibleArea);
    Q_UNUSED(visibleRatio);
    Q_UNUSED(fullPixels);
    Q_UNUSED(visiblePixels);
    Q_UNUSED(pixelReduction);

    const bool zoomed = std::abs(boundedCanvasZoom - 1.0) > epsilon;

    const bool scaleChanged = std::abs(rasterScale - m_lastRasterizedScale) > epsilon;
    if (!m_scaledRasterDirty && !scaleChanged && m_scaledRasterizedText.size() == targetSize && !m_asyncRasterInProgress) {
        if (!resizingUniformly && !zoomed) {
            m_scaledRasterThrottleActive = false;
        }
        return;
    }

    const bool needsSyncRender = resizingUniformly;
    
    // Enable preview at BOTH high zoom (rasterScale > kInitialPreviewScaleMax) AND low zoom (rasterScale < kFallbackMinScale)
    // This ensures chunks outside viewport show cached preview at all zoom levels
    const bool highZoomPreview = (rasterScale - kInitialPreviewScaleMax > epsilon);
    const bool lowZoomPreview = (rasterScale < kFallbackMinScale);
    const bool previewEligible = !resizingUniformly && !needsSyncRender && !m_scaledRasterPixmapValid && !m_asyncRasterInProgress && (highZoomPreview || lowZoomPreview);
    if (previewEligible) {
        // For low zoom, use kFallbackMinScale as preview quality to maintain visibility
        // For high zoom, use kInitialPreviewScaleMax for faster initial render
        const qreal targetPreviewScale = lowZoomPreview ? kFallbackMinScale : kInitialPreviewScaleMax;
        const qreal previewScale = std::clamp(targetPreviewScale, epsilon, std::max(rasterScale, targetPreviewScale));
        const qreal previewRatio = std::max(previewScale / std::max(rasterScale, epsilon), epsilon);
        const QSize previewSize(
            std::max(1, static_cast<int>(std::ceil(static_cast<qreal>(targetWidth) * previewRatio))),
            std::max(1, static_cast<int>(std::ceil(static_cast<qreal>(targetHeight) * previewRatio))));

        const StrokeRenderMode strokeMode = m_previewStrokeActive ? StrokeRenderMode::Preview : StrokeRenderMode::Normal;
        renderTextToImage(m_scaledRasterizedText, previewSize, previewScale, visibleRegion, strokeMode);

        QImage previewImage = m_scaledRasterizedText;
        if (previewImage.size() != targetSize) {
            previewImage = previewImage.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        }

        m_scaledRasterizedText = previewImage;
        m_scaledRasterPixmap = QPixmap::fromImage(m_scaledRasterizedText);
        m_scaledRasterPixmap.setDevicePixelRatio(1.0);
        m_scaledRasterPixmapValid = !m_scaledRasterPixmap.isNull();
        m_scaledRasterVisibleRegion = visibleRegion;
        m_lastViewportRect = visibleRegion;
        m_lastViewportScale = boundedCanvasZoom;
        m_lastRasterizedScale = previewScale;
        m_lastCanvasZoomForRaster = boundedCanvasZoom;
        m_lastScaledTargetSize = targetSize;
        m_lastScaledBaseSize = m_baseSize;
        m_scaledRasterDirty = true;
        m_forceScaledRasterRefresh = false;
        m_renderState = RenderState::PreviewReady;
        if (textCachePolicyV2Enabled()) {
            m_previewCacheKey = targetCacheKey;
            m_previewCacheKeyValid = true;
        }

        m_waitingForHighResRaster = true;
        m_pendingHighResScale = rasterScale;

        ++m_rasterRequestId;
        startAsyncRasterRequest(targetSize, rasterScale, boundedCanvasZoom, m_rasterRequestId);
        m_renderState = RenderState::HighResPending;
        return;
    }

    if (needsSyncRender) {
        ++m_rasterRequestId;
        m_pendingRasterRequestId = m_rasterRequestId;
        m_asyncRasterInProgress = false;
        m_activeAsyncRasterRequest.reset();
        m_pendingAsyncRasterRequest.reset();

        // ✅ Option A: Apply viewport optimization even in sync rendering mode
        // This reduces pixels by 10× during editing at high zoom levels
        const StrokeRenderMode strokeMode = m_previewStrokeActive ? StrokeRenderMode::Preview : StrokeRenderMode::Normal;
        renderTextToImage(m_scaledRasterizedText, targetSize, rasterScale, visibleRegion, strokeMode);
        m_scaledRasterPixmap = QPixmap::fromImage(m_scaledRasterizedText);
        m_scaledRasterPixmap.setDevicePixelRatio(1.0);
        m_scaledRasterPixmapValid = !m_scaledRasterPixmap.isNull();
        
        // ✅ Update viewport tracking for cache management
        m_scaledRasterVisibleRegion = visibleRegion;
        m_lastViewportRect = visibleRegion;
        m_lastViewportScale = boundedCanvasZoom;
        
        m_lastRasterizedScale = rasterScale;
        m_lastCanvasZoomForRaster = boundedCanvasZoom;
        m_lastScaledTargetSize = targetSize;
        m_lastScaledBaseSize = m_baseSize;
        m_scaledRasterDirty = false;
        m_scaledRasterThrottleActive = false;
        m_lastScaledRasterUpdate = std::chrono::steady_clock::now();
        m_forceScaledRasterRefresh = false;
        m_renderState = RenderState::HighResReady;
        if (textCachePolicyV2Enabled()) {
            m_activeHighResCacheKey = targetCacheKey;
            m_activeHighResCacheKeyValid = true;
        }
        enforceCacheBudget();
        update();
        return;
    }

    const bool hasScaledRaster = !m_scaledRasterizedText.isNull() && m_scaledRasterizedText.size() == targetSize;

    // Throttle during active operations (resize or zoom) to prevent job spam
    const bool isActiveOperation = resizingUniformly || zoomed;
    if (isActiveOperation && hasScaledRaster && m_scaledRasterThrottleActive) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - m_lastScaledRasterUpdate;
        if (elapsed < std::chrono::milliseconds(kScaledRasterThrottleIntervalMs)) {
            m_scaledRasterDirty = true;
            return;
        }
    }

    // Kick async job instead of synchronous render
    ++m_rasterRequestId;
    startRasterJob(targetSize, rasterScale, boundedCanvasZoom, m_rasterRequestId);

    if (isActiveOperation) {
        m_scaledRasterThrottleActive = true;
        m_lastScaledRasterUpdate = std::chrono::steady_clock::now();
    } else {
        m_scaledRasterThrottleActive = false;
    }
}

void TextMediaItem::ensureFrozenFallbackCache(qreal currentCanvasZoom) {
    if (m_activeHandle != None) {
        return;
    }
    if (m_isEditing) {
        return;
    }

    // Phase 1: Create/update low-resolution fallback cache for freeze zones
    const qreal epsilon = 1e-4;
    const qreal targetFallbackScale = 1.5;
    const qreal logicalWidth = std::max<qreal>(1.0, static_cast<qreal>(m_baseSize.width()));
    const qreal logicalHeight = std::max<qreal>(1.0, static_cast<qreal>(m_baseSize.height()));

    auto computeClampedScale = [&](qreal desiredScale) {
        qreal finalScale = desiredScale;
        const qreal desiredWidth = logicalWidth * finalScale;
        const qreal desiredHeight = logicalHeight * finalScale;
        const bool exceedsWidth = desiredWidth > kFallbackMaxDimensionPx;
        const bool exceedsHeight = desiredHeight > kFallbackMaxDimensionPx;
        if (exceedsWidth || exceedsHeight) {
            const qreal widthFactor = exceedsWidth
                ? (kFallbackMaxDimensionPx / std::max<qreal>(desiredWidth, 1.0))
                : 1.0;
            const qreal heightFactor = exceedsHeight
                ? (kFallbackMaxDimensionPx / std::max<qreal>(desiredHeight, 1.0))
                : 1.0;
            finalScale *= std::min(widthFactor, heightFactor);
        }
        finalScale = std::clamp(finalScale, kFallbackMinScale, desiredScale);
        return finalScale;
    };

    const qreal fallbackScale = computeClampedScale(targetFallbackScale);
    const QSize fallbackSize(
        std::max(1, static_cast<int>(std::ceil(logicalWidth * fallbackScale))),
        std::max(1, static_cast<int>(std::ceil(logicalHeight * fallbackScale)))
    );

    const bool textSizeChanged = (m_baseSize != m_frozenFallbackSize);
    const bool scaleChanged = std::abs(m_frozenFallbackScale - fallbackScale) > epsilon;
    const bool needsUpdate = !m_frozenFallbackValid || textSizeChanged || scaleChanged;

    if (!needsUpdate) {
        return;
    }

    if (m_frozenFallbackJobInFlight) {
        const bool sameRequest = (m_pendingFallbackSize == fallbackSize) &&
                                 (std::abs(m_pendingFallbackScale - fallbackScale) < epsilon);
        if (sameRequest) {
            return;
        }
    }

    const quint64 generation = ++m_frozenFallbackJobGeneration;
    const quint64 contentRevision = m_contentRevision;
    m_frozenFallbackJobInFlight = true;
    m_pendingFallbackSize = fallbackSize;
    m_pendingFallbackScale = fallbackScale;

    TextRasterJob job;
    job.snapshot = captureVectorSnapshot();
    job.targetSize = fallbackSize;
    job.scaleFactor = fallbackScale;
    job.canvasZoom = currentCanvasZoom;  // Tier 2: Pass canvas zoom for Phase 8
    job.targetRect = QRectF();

    ensureTextRenderer();
    auto future = QtConcurrent::run([this, job]() mutable {
        return renderRasterJobWithBackend(job);
    });

    std::weak_ptr<bool> guard = lifetimeGuard();
    auto* watcher = new QFutureWatcher<QImage>();
    QObject::connect(watcher, &QFutureWatcher<QImage>::finished, watcher, [this, guard, watcher, generation, contentRevision, fallbackSize, fallbackScale]() mutable {
        if (guard.expired()) {
            watcher->deleteLater();
            return;
        }

        QImage result = watcher->result();
        watcher->deleteLater();

        handleFrozenFallbackJobFinished(generation, contentRevision, std::move(result), fallbackSize, fallbackScale);
    });

    watcher->setFuture(future);
}

void TextMediaItem::handleFrozenFallbackJobFinished(quint64 generation, quint64 contentRevision, QImage&& raster, const QSize& size, qreal scale) {
    if (generation != m_frozenFallbackJobGeneration || contentRevision != m_contentRevision) {
        return;
    }

    m_frozenFallbackJobInFlight = false;

    if (raster.isNull()) {
        m_pendingFallbackSize = QSize();
        m_pendingFallbackScale = 1.0;
        return;
    }

    m_frozenFallbackPixmap = QPixmap::fromImage(raster);
    m_frozenFallbackPixmap.setDevicePixelRatio(1.0);
    m_frozenFallbackValid = !m_frozenFallbackPixmap.isNull();
    if (!m_frozenFallbackValid) {
        m_frozenFallbackContentRevision = 0;
        m_pendingFallbackSize = QSize();
        m_pendingFallbackScale = 1.0;
        return;
    }

    m_frozenFallbackContentRevision = contentRevision;
    m_frozenFallbackScale = scale;
    m_frozenFallbackSize = m_baseSize;
    m_pendingFallbackSize = QSize();
    m_pendingFallbackScale = 1.0;
    update();
}

void TextMediaItem::startRasterJob(const QSize& targetSize, qreal effectiveScale, qreal canvasZoom, quint64 requestId) {
    const QSize sanitizedSize(std::max(1, targetSize.width()), std::max(1, targetSize.height()));
    AsyncRasterRequest request{sanitizedSize, effectiveScale, canvasZoom, m_contentRevision, requestId};

    if (m_asyncRasterInProgress) {
        if (m_activeAsyncRasterRequest && m_activeAsyncRasterRequest->isEquivalentTo(sanitizedSize, effectiveScale, canvasZoom, m_contentRevision)) {
            m_rasterRequestId = m_activeAsyncRasterRequest->requestId;
            m_pendingRasterRequestId = m_activeAsyncRasterRequest->requestId;
            return;
        }
        m_pendingAsyncRasterRequest = request;
        m_pendingRasterRequestId = requestId;
        queueRasterJobDispatch();
        return;
    }

    m_pendingAsyncRasterRequest = request;
    m_pendingRasterRequestId = requestId;
    queueRasterJobDispatch();
}

void TextMediaItem::startAsyncRasterRequest(const QSize& targetSize, qreal effectiveScale, qreal canvasZoom, quint64 requestId) {
    AsyncRasterRequest request{targetSize, effectiveScale, canvasZoom, m_contentRevision, requestId};
    request.generation = ++m_rasterJobGeneration;
    request.startedAt = std::chrono::steady_clock::now();

    m_asyncRasterInProgress = true;
    m_pendingRasterRequestId = requestId;
    m_activeAsyncRasterRequest = request;
    m_renderState = RenderState::HighResPending;

    const quint64 queueDepth = (m_asyncRasterInProgress ? 1 : 0) + (m_pendingAsyncRasterRequest.has_value() ? 1 : 0);
    recordTextRasterStart(queueDepth);

    // Calculate visible region for viewport optimization
    QRectF visibleRegion = computeVisibleRegion();
    
    TextRasterJob job;
    job.snapshot = captureVectorSnapshot();
    job.targetSize = targetSize;
    job.scaleFactor = effectiveScale;
    job.canvasZoom = request.canvasZoom;  // Tier 2: Pass canvas zoom for Phase 8
    job.targetRect = visibleRegion;  // Pass visible region for partial rendering

    ensureTextRenderer();
    auto future = QtConcurrent::run([this, job]() {
        return renderRasterJobWithBackend(job);
    });

    std::weak_ptr<bool> guard = lifetimeGuard();
    auto* watcher = new QFutureWatcher<QImage>();
    QObject::connect(watcher, &QFutureWatcher<QImage>::finished, watcher, [this, guard, watcher, request, visibleRegion]() mutable {
        if (guard.expired()) {
            watcher->deleteLater();
            return;
        }

        QImage result = watcher->result();
        watcher->deleteLater();

        handleRasterJobFinished(request.generation, std::move(result), request.targetSize, request.scale, request.canvasZoom, visibleRegion);
    });

    watcher->setFuture(future);
}

void TextMediaItem::queueRasterJobDispatch() {
    if (m_rasterDispatchQueued) {
        return;
    }

    m_rasterDispatchQueued = true;
    std::weak_ptr<bool> guard = lifetimeGuard();
    QTimer::singleShot(0, [this, guard]() {
        if (guard.expired()) {
            return;
        }

        m_rasterDispatchQueued = false;
        dispatchPendingRasterRequest();
    });
}

void TextMediaItem::dispatchPendingRasterRequest() {
    if (m_asyncRasterInProgress) {
        return;
    }

    if (!m_pendingAsyncRasterRequest.has_value()) {
        return;
    }

    AsyncRasterRequest request = *m_pendingAsyncRasterRequest;
    m_pendingAsyncRasterRequest.reset();

    if (request.requestId < m_rasterRequestId) {
        return;
    }

    startAsyncRasterRequest(request.targetSize, request.scale, request.canvasZoom, request.requestId);
}

void TextMediaItem::startBaseRasterRequest(const QSize& targetSize) {
    if (m_baseRasterInProgress) {
        return;
    }

    const QSize sanitized(std::max(1, targetSize.width()), std::max(1, targetSize.height()));
    m_baseRasterInProgress = true;
    m_activeBaseRasterSize = sanitized;
    const quint64 generation = ++m_baseRasterGeneration;
    const quint64 contentRevision = m_contentRevision;

    TextRasterJob job;
    job.snapshot = captureVectorSnapshot();
    job.targetSize = sanitized;
    job.scaleFactor = 1.0;
    job.canvasZoom = 1.0;  // Tier 2: Base raster always at 1.0 zoom

    ensureTextRenderer();
    auto future = QtConcurrent::run([this, job]() {
        return renderRasterJobWithBackend(job);
    });

    std::weak_ptr<bool> guard = lifetimeGuard();
    auto* watcher = new QFutureWatcher<QImage>();
    QObject::connect(watcher, &QFutureWatcher<QImage>::finished, watcher, [this, guard, watcher, generation, contentRevision, sanitized]() mutable {
        if (guard.expired()) {
            watcher->deleteLater();
            return;
        }

        QImage result = watcher->result();
        watcher->deleteLater();

        handleBaseRasterJobFinished(generation, contentRevision, std::move(result), sanitized);
    });

    watcher->setFuture(future);
}

void TextMediaItem::queueBaseRasterDispatch() {
    if (m_baseRasterDispatchQueued) {
        return;
    }

    m_baseRasterDispatchQueued = true;
    std::weak_ptr<bool> guard = lifetimeGuard();
    QTimer::singleShot(0, [this, guard]() {
        if (guard.expired()) {
            return;
        }

        m_baseRasterDispatchQueued = false;
        dispatchPendingBaseRasterRequest();
    });
}

void TextMediaItem::dispatchPendingBaseRasterRequest() {
    if (m_baseRasterInProgress) {
        return;
    }

    if (!m_pendingBaseRasterRequest.has_value()) {
        return;
    }

    const QSize target = *m_pendingBaseRasterRequest;
    m_pendingBaseRasterRequest.reset();

    startBaseRasterRequest(target);
}

void TextMediaItem::startNextPendingBaseRasterRequest() {
    if (!m_pendingBaseRasterRequest.has_value()) {
        return;
    }

    if (m_baseRasterInProgress) {
        return;
    }

    queueBaseRasterDispatch();
}

void TextMediaItem::handleBaseRasterJobFinished(quint64 generation, quint64 contentRevision, QImage&& raster, const QSize& size) {
    if (generation != m_baseRasterGeneration || contentRevision != m_contentRevision) {
        m_baseRasterInProgress = false;
        startNextPendingBaseRasterRequest();
        return;
    }

    m_baseRasterInProgress = false;
    m_activeBaseRasterSize = QSize();

    const QSize expectedSize(std::max(1, m_baseSize.width()), std::max(1, m_baseSize.height()));
    if (size != expectedSize) {
        m_needsRasterization = true;
        m_pendingBaseRasterRequest = expectedSize;
        queueBaseRasterDispatch();
        return;
    }

    m_rasterizedText = std::move(raster);
    m_baseRasterContentRevision = contentRevision;
    m_lastRasterizedSize = m_baseSize;
    m_needsRasterization = false;
    m_scaledRasterDirty = true;
    m_lastRasterizedScale = 1.0;

    update();
    startNextPendingBaseRasterRequest();
}

void TextMediaItem::startNextPendingAsyncRasterRequest() {
    if (!m_pendingAsyncRasterRequest.has_value()) {
        return;
    }

    if (m_asyncRasterInProgress) {
        return;
    }

    queueRasterJobDispatch();
}

void TextMediaItem::handleRasterJobFinished(quint64 generation, QImage&& raster, const QSize& size, qreal scale, qreal canvasZoom, const QRectF& visibleRegion) {
    Q_UNUSED(size);

    const auto now = std::chrono::steady_clock::now();
    qint64 durationMs = 0;
    quint64 completedContentRevision = 0;
    if (m_activeAsyncRasterRequest.has_value()) {
        durationMs = static_cast<qint64>(std::chrono::duration_cast<std::chrono::milliseconds>(now - m_activeAsyncRasterRequest->startedAt).count());
        completedContentRevision = m_activeAsyncRasterRequest->contentRevision;
    }

    if (m_activeAsyncRasterRequest && m_activeAsyncRasterRequest->generation == generation) {
        m_asyncRasterInProgress = false;
        m_activeAsyncRasterRequest.reset();
    }

    if (generation != m_rasterJobGeneration || completedContentRevision != m_contentRevision) {
        recordTextRasterResult(durationMs, true, false);
        if (m_waitingForHighResRaster && std::abs(scale - m_pendingHighResScale) < 1e-3) {
            m_waitingForHighResRaster = false;
            m_pendingHighResScale = 0.0;
        }
        startNextPendingAsyncRasterRequest();
        return;
    }

    // Keep interactive resize rendering stable: avoid swapping in async rasters
    // produced against stale intermediate geometry while handles are moving.
    if (m_activeHandle != None) {
        m_scaledRasterDirty = true;
        recordTextRasterResult(durationMs, false, true);
        startNextPendingAsyncRasterRequest();
        return;
    }

    m_scaledRasterizedText = std::move(raster);
    m_scaledRasterPixmap = QPixmap::fromImage(m_scaledRasterizedText);
    m_scaledRasterPixmap.setDevicePixelRatio(1.0);
    m_scaledRasterPixmapValid = !m_scaledRasterPixmap.isNull();
    m_scaledRasterContentRevision = completedContentRevision;
    m_lastScaledTargetSize = size;
    m_lastScaledBaseSize = m_baseSize;
    m_scaledRasterVisibleRegion = visibleRegion;  // Store visible region for correct positioning
    m_forceScaledRasterRefresh = false;
    
    // Étape 3: Update viewport tracking when new raster completes
    m_lastViewportRect = visibleRegion;
    m_lastViewportScale = canvasZoom;
    
    m_lastRasterizedScale = scale;
    m_lastCanvasZoomForRaster = canvasZoom;
    m_scaledRasterDirty = false;
    m_asyncRasterInProgress = false;
    m_pendingRasterRequestId = 0;
    m_lastScaledRasterUpdate = std::chrono::steady_clock::now();
    m_renderState = RenderState::HighResReady;
    if (textCachePolicyV2Enabled()) {
        qreal dpr = 1.0;
        if (scene() && !scene()->views().isEmpty()) {
            if (QGraphicsView* view = scene()->views().first()) {
                dpr = std::max<qreal>(1.0, view->devicePixelRatioF());
            }
        }
        const VectorDrawSnapshot snapshot = captureVectorSnapshot();
        m_activeHighResCacheKey = makeCacheKey(snapshot, size, scale, dpr);
        m_activeHighResCacheKeyValid = true;
    }
    enforceCacheBudget();
    recordTextRasterResult(durationMs, false, false);

    if (m_waitingForHighResRaster && std::abs(scale - m_pendingHighResScale) < 1e-3) {
        m_waitingForHighResRaster = false;
        m_pendingHighResScale = 0.0;
        if (m_previewStrokeActive) {
            m_previewStrokeActive = false;
        }
    }

    update();
    startNextPendingAsyncRasterRequest();
}

void TextMediaItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
    Q_UNUSED(option);
    Q_UNUSED(widget);
    
    if (!painter) return;
    
    // Only update geometry when not editing to avoid triggering expensive layout operations during panning
    if (!m_isEditing) {
        updateInlineEditorGeometry();
    }

    if (m_isEditing && m_inlineEditor) {
        m_inlineEditor->setVisible(true);
        m_inlineEditor->setOpacity(1.0);
    }

    painter->save();

    // Get the item bounds
    QRectF bounds = boundingRect();

    // Apply content visibility and opacity
    if (!m_contentVisible || m_contentOpacity <= 0.0 || m_contentDisplayOpacity <= 0.0) {
        painter->restore();
        // Still paint selection chrome and overlays
        paintSelectionAndLabel(painter);
        return;
    }
    
    // Calculate effective opacity
    qreal effectiveOpacity = m_contentOpacity * m_contentDisplayOpacity;
    painter->setOpacity(effectiveOpacity);

    const qreal currentScale = scale();
    const qreal uniformScale = std::max(std::abs(m_uniformScaleFactor), 1e-4);

    qreal canvasZoom = 1.0;
    if (scene() && !scene()->views().isEmpty()) {
        if (QGraphicsView* view = scene()->views().first()) {
            const QTransform viewTransform = view->transform();
            const qreal scaleX = std::hypot(viewTransform.m11(), viewTransform.m21());
            const qreal scaleY = std::hypot(viewTransform.m22(), viewTransform.m12());
            if (scaleX > 1e-6 && scaleY > 1e-6) {
                canvasZoom = (scaleX + scaleY) * 0.5;
            } else if (scaleX > 1e-6) {
                canvasZoom = scaleX;
            } else if (scaleY > 1e-6) {
                canvasZoom = scaleY;
            }
        }
    }
    if (canvasZoom <= 1e-6) {
        canvasZoom = 1.0;
    }

    const qreal effectiveScale = std::max(std::abs(currentScale) * uniformScale * canvasZoom, 1e-4);
    const bool resizingUniformly = (m_activeHandle != None);
    const bool zoomed = std::abs(canvasZoom - 1.0) > 1e-4;
    const bool needsScaledRaster = zoomed ||
        std::abs(currentScale - 1.0) > 1e-4 ||
        std::abs(m_uniformScaleFactor - 1.0) > 1e-4 ||
        resizingUniformly;

    if (needsScaledRaster) {
        const bool interactiveResize = resizingUniformly;
        if (interactiveResize) {
            ensureScaledRaster(effectiveScale, currentScale, canvasZoom);
        } else {
            queueScaledRasterUpdate(effectiveScale, currentScale, canvasZoom);
        }

        if (m_scaledRasterPixmapValid && m_lastScaledBaseSize != m_baseSize) {
            m_scaledRasterPixmapValid = false;
            m_scaledRasterDirty = true;
            if (interactiveResize) {
                ensureScaledRaster(effectiveScale, currentScale, canvasZoom);
            } else {
                queueScaledRasterUpdate(effectiveScale, currentScale, canvasZoom);
            }
        }

        painter->save();
        painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
        // Neutralize the item's local scale, uniform scale, and canvas/view zoom so the text
        // keeps the same layout size inside the container while the raster image
        // provides more pixels when zooming.
        const qreal epsilon = 1e-4;
        const qreal totalScale = std::max(std::abs(currentScale * uniformScale * canvasZoom), epsilon);
        painter->scale(1.0 / totalScale, 1.0 / totalScale);

        const QTransform scaleTransform = (std::abs(totalScale) > epsilon)
            ? QTransform::fromScale(totalScale, totalScale)
            : QTransform::fromScale(1.0, 1.0);
        QRectF scaledBounds = scaleTransform.mapRect(bounds);

        // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
        // Phase 2: DUAL-CACHE RENDERING - Two-pass system for freeze zones
        // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
        
        // Calculate viewport region FIRST (needed for both passes)
        QRectF viewportDestRect;
        bool hasViewportCache = false;
        const bool hasCurrentHighRes =
            m_scaledRasterPixmapValid &&
            !m_scaledRasterPixmap.isNull() &&
            m_scaledRasterContentRevision == m_contentRevision;
        const bool hasCurrentFallback =
            m_frozenFallbackValid &&
            !m_frozenFallbackPixmap.isNull() &&
            m_frozenFallbackContentRevision == m_contentRevision;
        const bool canUseDualLayerCompositing = hasCurrentHighRes && hasCurrentFallback;

        if (hasCurrentHighRes) {
            hasViewportCache = true;
            viewportDestRect = scaledBounds;
            if (!m_scaledRasterVisibleRegion.isEmpty() && m_scaledRasterVisibleRegion.isValid()) {
                const QRectF scaledVisibleRegion = scaleTransform.mapRect(m_scaledRasterVisibleRegion);
                viewportDestRect = scaledVisibleRegion;
            }
        }
        
        // PASS 1: Draw frozen fallback cache (background layer - full text at low res)
        // Disabled during interactive resize to avoid dual-layer flicker.
        if (!interactiveResize && canUseDualLayerCompositing) {
            // Calculate display scale for fallback
            // fallbackDisplayScale converts from fallback resolution to current display resolution
            const qreal fallbackDisplayScale = totalScale / m_frozenFallbackScale;
            
            painter->save();
            painter->scale(fallbackDisplayScale, fallbackDisplayScale);
            
            // OPTIMIZATION: Set clipping region to exclude viewport area
            // This prevents drawing fallback under the high-res viewport cache
            if (hasViewportCache) {
                // Create clipping path: full text MINUS viewport region
                QPainterPath fullTextPath;
                const QSizeF fallbackSize(
                    static_cast<qreal>(m_frozenFallbackPixmap.width()),
                    static_cast<qreal>(m_frozenFallbackPixmap.height())
                );
                fullTextPath.addRect(QRectF(QPointF(0.0, 0.0), fallbackSize));
                const QRectF viewportInFallbackSpace(
                    viewportDestRect.x() / fallbackDisplayScale,
                    viewportDestRect.y() / fallbackDisplayScale,
                    viewportDestRect.width() / fallbackDisplayScale,
                    viewportDestRect.height() / fallbackDisplayScale
                );
                
                QPainterPath viewportPath;
                viewportPath.addRect(viewportInFallbackSpace);
                // Subtract viewport from full text to get "freeze zones only"
                QPainterPath freezeZonesPath = fullTextPath.subtracted(viewportPath);
                painter->setClipPath(freezeZonesPath);
            }
            
            const QSizeF fallbackSize(
                static_cast<qreal>(m_frozenFallbackPixmap.width()),
                static_cast<qreal>(m_frozenFallbackPixmap.height())
            );
            // Position fallback in the fallback-scaled coordinate space
            const QPointF fallbackOrigin(
                scaledBounds.x() / fallbackDisplayScale,
                scaledBounds.y() / fallbackDisplayScale
            );
            const QRectF fallbackDestRect(fallbackOrigin, fallbackSize);
            const QRectF fallbackSourceRect(QPointF(0.0, 0.0), fallbackSize);
            
            // Draw fallback ONLY in non-viewport areas (thanks to clipping)
            painter->drawPixmap(fallbackDestRect, m_frozenFallbackPixmap, fallbackSourceRect);
            
            painter->restore();
        }
        
        // ✅ PASS 2: Draw high-res viewport cache (foreground layer - viewport area only)
        // This provides sharp rendering for the currently visible portion
        if (hasCurrentHighRes) {
            // DPR is always 1.0, so source size equals physical pixel dimensions
            const QSizeF sourceSize(
                static_cast<qreal>(m_scaledRasterPixmap.width()),
                static_cast<qreal>(m_scaledRasterPixmap.height())
            );
            const QRectF sourceRect(QPointF(0.0, 0.0), sourceSize);
            
            // If viewport optimization is active, draw at the correct offset
            QRectF destRect = scaledBounds;
            if (!m_scaledRasterVisibleRegion.isEmpty() && m_scaledRasterVisibleRegion.isValid()) {
                // The rasterized image represents only m_scaledRasterVisibleRegion of the item
                // Position it correctly in item coordinates
                const QRectF scaledVisibleRegion = scaleTransform.mapRect(m_scaledRasterVisibleRegion);
                destRect = scaledVisibleRegion;
            }

            if (interactiveResize) {
                // During active handle drag, pin draw target to full bounds to prevent
                // viewport-region wobble while geometry is changing every frame.
                destRect = scaledBounds;
            }
            
            // Draw viewport at high resolution (8×+) - this overwrites the fallback in the visible area
            painter->drawPixmap(destRect, m_scaledRasterPixmap, sourceRect);
            
        } else if (!m_scaledRasterizedText.isNull() && m_scaledRasterContentRevision == m_contentRevision) {
            // drawImage has no sourceRect overload for QImage here; scale via painter
            painter->drawImage(scaledBounds, m_scaledRasterizedText);
        } else {
            if (m_needsRasterization || m_lastRasterizedSize != m_baseSize || m_baseRasterContentRevision != m_contentRevision) {
                rasterizeText();
            }
            if (!m_rasterizedText.isNull() && m_baseRasterContentRevision == m_contentRevision) {
                painter->drawImage(scaledBounds, m_rasterizedText);
            }
        }
        painter->restore();
    } else {
        // Rasterize text if needed (once after editing/resizing)
        rasterizeText();
        m_renderState = RenderState::Idle;

        // Draw the cached bitmap instead of re-rendering vector text
        if (!m_rasterizedText.isNull()) {
            painter->drawImage(bounds, m_rasterizedText);
        }
    }

    painter->restore();
    
    // Paint selection chrome and overlays (handles, buttons, etc.)
    paintSelectionAndLabel(painter);
}

void TextMediaItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (!isSelected()) {
            setSelected(true);
        }

        beginInlineEditing();

        event->accept();
        return;
    }

    ResizableMediaBase::mouseDoubleClickEvent(event);
}

QVariant TextMediaItem::itemChange(GraphicsItemChange change, const QVariant& value) {
    if (change == ItemSelectedHasChanged && value.canConvert<bool>()) {
        const bool selectedNow = value.toBool();
        if (!selectedNow && m_isEditing) {
            commitInlineEditing();
        }
    }

    // Detect scale bake BEFORE calling parent (which will change the scale)
    if (change == ItemScaleHasChanged && value.canConvert<double>()) {
        const qreal epsilon = 1e-4;
        qreal newScale = value.toDouble();
        qreal oldScale = m_lastObservedScale;
        
        if (textHotLogsEnabled()) {
            qDebug() << "[TextMedia][itemChange][DEBUG] ItemScaleHasChanged for item" << mediaId()
                     << "\n  oldScale:" << oldScale << "-> newScale:" << newScale
                     << "\n  m_customScaleBaking:" << m_customScaleBaking
                     << "\n  m_uniformScaleFactor (before):" << m_uniformScaleFactor;
        }
        
        // Skip automatic baking if we're doing custom scale baking (e.g., during Alt-resize)
        // to preserve m_uniformScaleFactor
        if (!m_customScaleBaking) {
            // Detect when scale is being reset to 1.0 after a uniform resize (scale bake into base size)
            const bool scaleBeingBaked = (std::abs(newScale - 1.0) < epsilon) && (std::abs(oldScale - 1.0) > epsilon);
            
            if (textHotLogsEnabled()) {
                qDebug() << "[TextMedia][itemChange][DEBUG] scaleBeingBaked:" << scaleBeingBaked
                         << "m_pendingUniformScaleBake:" << m_pendingUniformScaleBake;
            }
            
            if (scaleBeingBaked) {
                qreal bakeScale = m_pendingUniformScaleBake ? m_pendingUniformScaleAmount : std::abs(oldScale);
                if (std::abs(bakeScale) <= epsilon) {
                    bakeScale = 1.0;
                }

                if (textHotLogsEnabled()) {
                    qDebug() << "[TextMedia][itemChange][DEBUG] Applying bakeScale" << bakeScale
                             << "to m_uniformScaleFactor" << m_uniformScaleFactor;
                }
                
                if (std::abs(bakeScale - 1.0) > epsilon) {
                    m_uniformScaleFactor *= bakeScale;
                    if (textHotLogsEnabled()) {
                        qDebug() << "[TextMedia][itemChange][DEBUG] m_uniformScaleFactor updated to" << m_uniformScaleFactor;
                    }
                }
                if (std::abs(m_uniformScaleFactor) < epsilon) {
                    m_uniformScaleFactor = 1.0;
                    if (textHotLogsEnabled()) {
                        qDebug() << "[TextMedia][itemChange][DEBUG] m_uniformScaleFactor was too small, reset to 1.0";
                    }
                }

                m_pendingUniformScaleBake = false;
                m_pendingUniformScaleAmount = 1.0;
                m_scaledRasterDirty = true;
            }
        } else {
            if (textHotLogsEnabled()) {
                qDebug() << "[TextMedia][itemChange][DEBUG] Skipping automatic baking (m_customScaleBaking=true)";
            }
        }
        
        m_lastObservedScale = newScale;
    }

    QVariant result = ResizableMediaBase::itemChange(change, value);

    if (change == ItemScaleHasChanged) {
        m_scaledRasterDirty = true;
        update();
    }

    if (change == ItemTransformHasChanged ||
        change == ItemPositionHasChanged ||
        change == ItemSelectedChange ||
        change == ItemSelectedHasChanged) {
        updateInlineEditorGeometry();
    }

    return result;
}

void TextMediaItem::ensureAlignmentControls() {
    bool createdPanel = false;
    if (!m_alignmentPanel) {
        m_alignmentPanel = std::make_unique<OverlayPanel>(OverlayPanel::Bottom);
        createdPanel = true;
    }

    OverlayStyle style = m_overlayStyle;
    style.itemSpacing = 10;
    style.paddingX = std::max(style.paddingX, 8);
    style.paddingY = std::max(style.paddingY, 6);
    const QString styleSignature = alignmentPanelStyleSignature(style);
    if (createdPanel || m_alignmentPanelStyleSignature != styleSignature) {
        m_alignmentPanel->setStyle(style);
        m_alignmentPanelStyleSignature = styleSignature;
    }
    if (createdPanel) {
        m_alignmentPanel->setBackgroundVisible(false);
    }

    if (scene() && m_alignmentPanel->scene() != scene()) {
        m_alignmentPanel->setScene(scene());
    }

    if (m_fitToTextBtn) {
        updateAlignmentButtonStates();
        return;
    }

    const qreal groupGap = 10.0;

    m_fitToTextBtn = m_alignmentPanel->addButton(QString(), QStringLiteral("fit_to_text"));
    if (m_fitToTextBtn) {
        m_fitToTextBtn->setSvgIcon(":/icons/icons/text/fit-to-text.svg");
        m_fitToTextBtn->setToggleOnly(true);
        m_fitToTextBtn->setSegmentRole(OverlayButtonElement::SegmentRole::Solo);
        m_fitToTextBtn->setSpacingAfter(groupGap);
        m_fitToTextBtn->setOnClicked([this]() {
            setFitToTextEnabled(!m_fitToTextEnabled);
        });
    }

    auto makeAlignButton = [&](const QString& id,
                               const char* iconPath,
                               OverlayButtonElement::SegmentRole role,
                               std::function<void()> onActivate) -> std::shared_ptr<OverlayButtonElement> {
        auto button = m_alignmentPanel->addButton(QString(), id);
        if (button) {
            button->setSvgIcon(iconPath);
            button->setToggleOnly(true);
            button->setSegmentRole(role);
            button->setOnClicked(std::move(onActivate));
        }
        return button;
    };

    m_alignLeftBtn = makeAlignButton(
        QStringLiteral("align_left"),
        ":/icons/icons/text/horizontal-align-left.svg",
        OverlayButtonElement::SegmentRole::Leading,
        [this]() { setHorizontalAlignment(HorizontalAlignment::Left); });

    m_alignCenterHBtn = makeAlignButton(
        QStringLiteral("align_center_h"),
        ":/icons/icons/text/horizontal-align-center.svg",
        OverlayButtonElement::SegmentRole::Middle,
        [this]() { setHorizontalAlignment(HorizontalAlignment::Center); });

    m_alignRightBtn = makeAlignButton(
        QStringLiteral("align_right"),
        ":/icons/icons/text/horizontal-align-right.svg",
        OverlayButtonElement::SegmentRole::Trailing,
        [this]() { setHorizontalAlignment(HorizontalAlignment::Right); });

    if (m_alignRightBtn) {
        m_alignRightBtn->setSpacingAfter(groupGap);
    }

    m_alignTopBtn = makeAlignButton(
        QStringLiteral("align_top"),
        ":/icons/icons/text/vertical-align-top.svg",
        OverlayButtonElement::SegmentRole::Leading,
        [this]() { setVerticalAlignment(VerticalAlignment::Top); });

    m_alignCenterVBtn = makeAlignButton(
        QStringLiteral("align_center_v"),
        ":/icons/icons/text/vertical-align-center.svg",
        OverlayButtonElement::SegmentRole::Middle,
        [this]() { setVerticalAlignment(VerticalAlignment::Center); });

    m_alignBottomBtn = makeAlignButton(
        QStringLiteral("align_bottom"),
        ":/icons/icons/text/vertical-align-bottom.svg",
        OverlayButtonElement::SegmentRole::Trailing,
        [this]() { setVerticalAlignment(VerticalAlignment::Bottom); });

    updateAlignmentButtonStates();
    if (m_alignmentPanel) {
        m_alignmentPanel->setVisible(false);
    }
}

void TextMediaItem::updateAlignmentControlsLayout() {
    ensureAlignmentControls();
    if (!m_alignmentPanel) return;

    if (scene() && m_alignmentPanel->scene() != scene()) {
        m_alignmentPanel->setScene(scene());
    }

    const bool shouldShow = isSelected();
    m_alignmentPanel->setVisible(shouldShow);

    if (!shouldShow) return;
    if (!scene() || scene()->views().isEmpty()) return;
    QGraphicsView* view = scene()->views().first();
    if (!view) return;

    const QSize textSize = baseSizePx();
    const QPointF anchorItem(textSize.width() / 2.0, textSize.height());
    const QPointF anchorScene = mapToScene(anchorItem);

    m_alignmentPanel->updateLayoutWithAnchor(anchorScene, view);
}

bool TextMediaItem::isStrokeWorkExpensiveCandidate() const {
    const QString& content = textForRendering();
    if (content.size() > kStrokeHeavyGlyphThreshold) {
        return true;
    }

    const qint64 width = std::max(1, m_baseSize.width());
    const qint64 height = std::max(1, m_baseSize.height());
    const qint64 pixelEstimate = width * height;
    return pixelEstimate > kStrokeHeavyPixelThreshold;
}

void TextMediaItem::updateStrokePreviewState(qreal requestedPercent) {
    const bool expensiveStroke = (requestedPercent >= kPreviewStrokePercentCap) && isStrokeWorkExpensiveCandidate();
    const bool shouldEnablePreview = expensiveStroke && requestedPercent > 0.0;

    if (shouldEnablePreview) {
        const bool wasActive = m_previewStrokeActive;
        m_previewStrokeActive = true;
        const qreal previewPercent = std::min(kPreviewStrokePercentCap, std::max(kMinPreviewStrokePercent, requestedPercent * 0.35));
        m_previewStrokePercent = previewPercent;
        if (!wasActive) {
            if (textHotLogsEnabled()) {
                qDebug() << "[TextMediaItem][StrokePreview]" << mediaId()
                         << "requested%" << requestedPercent
                         << "preview%" << previewPercent
                         << "glyphEstimate" << textForRendering().size();
            }
        }
    } else if (m_previewStrokeActive) {
        m_previewStrokeActive = false;
        m_previewStrokePercent = 0.0;
    }

}


void TextMediaItem::updateAlignmentButtonStates() {
    if (m_fitToTextBtn) {
        m_fitToTextBtn->setState(m_fitToTextEnabled ?
            OverlayElement::Toggled : OverlayElement::Normal);
    }

    auto updateHorizontal = [&](HorizontalAlignment alignment, const std::shared_ptr<OverlayButtonElement>& button) {
        if (!button) return;
        button->setState(m_horizontalAlignment == alignment ?
            OverlayElement::Toggled : OverlayElement::Normal);
    };

    updateHorizontal(HorizontalAlignment::Left, m_alignLeftBtn);
    updateHorizontal(HorizontalAlignment::Center, m_alignCenterHBtn);
    updateHorizontal(HorizontalAlignment::Right, m_alignRightBtn);

    auto updateVertical = [&](VerticalAlignment alignment, const std::shared_ptr<OverlayButtonElement>& button) {
        if (!button) return;
        button->setState(m_verticalAlignment == alignment ?
            OverlayElement::Toggled : OverlayElement::Normal);
    };

    updateVertical(VerticalAlignment::Top, m_alignTopBtn);
    updateVertical(VerticalAlignment::Center, m_alignCenterVBtn);
    updateVertical(VerticalAlignment::Bottom, m_alignBottomBtn);
}

void TextMediaItem::setFitToTextEnabled(bool enabled) {
    if (m_fitToTextEnabled == enabled) {
        return;
    }

    m_fitToTextEnabled = enabled;
    
    // Invalidate all caches when fit mode changes because text layout is completely different
    invalidateRenderPipeline(InvalidationReason::Geometry, true);
    m_scaledRasterPixmapValid = false;
    m_frozenFallbackValid = false;
    if (textHotLogsEnabled()) {
        qDebug() << "[TextMedia][FitMode] toggled" << (m_fitToTextEnabled ? "ON" : "OFF")
                 << "- invalidating caches for item" << mediaId();
    }
    
    if (!m_fitToTextEnabled) {
        m_fitToTextUpdatePending = false;
    }
    applyFitModeConstraintsToEditor();
    updateInlineEditorGeometry();
    updateAlignmentButtonStates();
    updateAlignmentControlsLayout();

    if (m_fitToTextEnabled) {
        scheduleFitToTextUpdate();
    }
}

void TextMediaItem::scheduleFitToTextUpdate() {
    if (!m_fitToTextEnabled) {
        return;
    }
    if (m_fitToTextUpdatePending) {
        return;
    }
    m_fitToTextUpdatePending = true;
    std::weak_ptr<bool> guard = lifetimeGuard();
    QTimer::singleShot(0, [this, guard]() {
        if (guard.expired()) {
            return;
        }
        applyFitToTextNow();
    });
}

void TextMediaItem::applyFitToTextNow() {
    m_fitToTextUpdatePending = false;
    if (!m_fitToTextEnabled || m_applyingFitToText) {
        return;
    }

    ensureInlineEditor();
    if (!m_inlineEditor) {
        return;
    }

    QScopedValueRollback<bool> guard(m_applyingFitToText, true);

    if (!m_isEditing && m_inlineEditor->toPlainText() != m_text) {
        QScopedValueRollback<bool> docGuard(m_ignoreDocumentChange, true);
        m_inlineEditor->setPlainText(m_text);
    }

    QTextDocument* doc = m_inlineEditor->document();
    if (!doc) {
        return;
    }

    doc->adjustSize();

    const auto measureLogicalWidth = [doc]() -> qreal {
        qreal minLeft = 0.0;
        qreal maxRight = 0.0;
        bool haveLine = false;

        for (QTextBlock block = doc->begin(); block != doc->end(); block = block.next()) {
            QTextLayout* layout = block.layout();
            if (!layout) {
                continue;
            }

            const int lineCount = layout->lineCount();
            for (int lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
                QTextLine line = layout->lineAt(lineIndex);
                if (!line.isValid()) {
                    continue;
                }

                const qreal left = line.x();
                const qreal right = line.x() + line.naturalTextWidth();
                if (!haveLine) {
                    minLeft = left;
                    maxRight = right;
                    haveLine = true;
                } else {
                    if (left < minLeft) {
                        minLeft = left;
                    }
                    if (right > maxRight) {
                        maxRight = right;
                    }
                }
            }
        }

        if (!haveLine) {
            const qreal width = doc->idealWidth();
            return std::max<qreal>(1.0, width);
        }

        const qreal logicalWidth = std::max<qreal>(1.0, maxRight - minLeft);
        return logicalWidth;
    };

    const qreal logicalContentWidth = measureLogicalWidth();

    QAbstractTextDocumentLayout* layout = doc->documentLayout();
    qreal logicalContentHeight = 0.0;
    if (layout) {
        const QSizeF size = layout->documentSize();
        logicalContentHeight = std::max<qreal>(1.0, size.height());
    } else {
        logicalContentHeight = std::max<qreal>(1.0, doc->size().height());
    }

    const qreal uniformScale = std::max(std::abs(m_uniformScaleFactor), 1e-4);
    const qreal marginLogical = contentPaddingPx();
    const qreal marginScene = marginLogical * uniformScale;

    // Calculate dimensions with minimum width constraint in fit-to-text mode
    const int calculatedWidth = static_cast<int>(std::ceil(logicalContentWidth * uniformScale + marginScene * 2.0));
    const int minWidth = static_cast<int>(std::ceil(kFitToTextMinWidth * uniformScale + marginScene * 2.0));
    
    QSize newBase(
        std::max(minWidth, std::max(1, calculatedWidth)),
        std::max(1, static_cast<int>(std::ceil(logicalContentHeight * uniformScale + marginScene * 2.0))));

    const QSize oldBase = m_baseSize;

    if (std::abs(newBase.width() - oldBase.width()) <= kFitToTextSizeStabilizationPx &&
        std::abs(newBase.height() - oldBase.height()) <= kFitToTextSizeStabilizationPx) {
        newBase = oldBase;
    }

    if (newBase != oldBase) {
        auto anchorPointForSize = [this](const QSize& size) {
            qreal x = 0.0;
            switch (m_horizontalAlignment) {
                case HorizontalAlignment::Left:
                    x = 0.0;
                    break;
                case HorizontalAlignment::Center:
                    x = static_cast<qreal>(size.width()) * 0.5;
                    break;
                case HorizontalAlignment::Right:
                    x = static_cast<qreal>(size.width());
                    break;
            }

            qreal y = 0.0;
            switch (m_verticalAlignment) {
                case VerticalAlignment::Top:
                    y = 0.0;
                    break;
                case VerticalAlignment::Center:
                    y = static_cast<qreal>(size.height()) * 0.5;
                    break;
                case VerticalAlignment::Bottom:
                    y = static_cast<qreal>(size.height());
                    break;
            }

            return QPointF(x, y);
        };

        const QPointF oldAnchorLocal = anchorPointForSize(oldBase);
        QPointF anchorBefore;
        if (parentItem()) {
            anchorBefore = mapToParent(oldAnchorLocal);
        } else if (scene()) {
            anchorBefore = mapToScene(oldAnchorLocal);
        } else {
            anchorBefore = oldAnchorLocal + pos();
        }

        prepareGeometryChange();
        m_baseSize = newBase;
        invalidateRenderPipeline(InvalidationReason::Geometry, true);
        m_lastRasterizedScale = 1.0;
        m_cachedEditorPosValid = false;

        const QPointF newAnchorLocal = anchorPointForSize(newBase);
        QPointF anchorAfter;
        if (parentItem()) {
            anchorAfter = mapToParent(newAnchorLocal);
        } else if (scene()) {
            anchorAfter = mapToScene(newAnchorLocal);
        } else {
            anchorAfter = newAnchorLocal + pos();
        }

        const QPointF delta = anchorBefore - anchorAfter;
        if (!delta.isNull()) {
            setPos(pos() + delta);
        }
    }

    m_cachedDocumentSize = QSizeF(logicalContentWidth, logicalContentHeight);
    m_cachedIdealWidth = logicalContentWidth;
    m_documentMetricsDirty = true;
    m_cachedEditorPosValid = false;

    syncInlineEditorToBaseSize();
    updateInlineEditorGeometry();
    updateAlignmentControlsLayout();
    updateOverlayLayout();
    update();
}

void TextMediaItem::applyAlignmentToEditor() {
    if (!m_inlineEditor) {
        return;
    }
    
    // Convert horizontal alignment to Qt alignment
    Qt::Alignment qtAlign = Qt::AlignVCenter; // We only control horizontal in editor
    switch (m_horizontalAlignment) {
        case HorizontalAlignment::Left:
            qtAlign = Qt::AlignLeft;
            break;
        case HorizontalAlignment::Center:
            qtAlign = Qt::AlignHCenter;
            break;
        case HorizontalAlignment::Right:
            qtAlign = Qt::AlignRight;
            break;
    }
    
    // Apply to document default
    if (QTextDocument* doc = m_inlineEditor->document()) {
        QTextOption opt = doc->defaultTextOption();
        opt.setAlignment(qtAlign);
        doc->setDefaultTextOption(opt);
    }
    
    // Apply to all existing text blocks
    applyTextAlignment(m_inlineEditor, qtAlign);
    
    m_documentMetricsDirty = true;
    m_cachedEditorPosValid = false;
}

void TextMediaItem::applyFitModeConstraintsToEditor() {
    if (!m_inlineEditor) {
        return;
    }

    const qreal margin = contentPaddingPx();
    const qreal uniformScale = std::max(std::abs(m_uniformScaleFactor), 1e-4);
    const qreal desiredTextWidth = m_fitToTextEnabled
        ? -1.0
        : std::max<qreal>(1.0, static_cast<qreal>(m_baseSize.width()) / uniformScale - 2.0 * margin);

    bool widthModified = false;

    {
        QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);

        if (QTextDocument* doc = m_inlineEditor->document()) {
            QTextOption opt = doc->defaultTextOption();
            const QTextOption::WrapMode desiredWrap = m_fitToTextEnabled ? QTextOption::NoWrap : QTextOption::WordWrap;
            if (opt.wrapMode() != desiredWrap) {
                opt.setWrapMode(desiredWrap);
                doc->setDefaultTextOption(opt);
                widthModified = true;
            }
        }

        const qreal currentWidth = m_inlineEditor->textWidth();
        const bool widthSignDiffers = (currentWidth < 0.0) != (desiredTextWidth < 0.0);
        const bool widthValueDiffers = (currentWidth >= 0.0 && desiredTextWidth >= 0.0 && std::abs(currentWidth - desiredTextWidth) > 1e-3);
        if (widthSignDiffers || widthValueDiffers) {
            m_inlineEditor->setTextWidth(desiredTextWidth);
            widthModified = true;
        }
    }

    if (widthModified) {
        if (QTextDocument* doc = m_inlineEditor->document()) {
            doc->adjustSize();
        }
        m_cachedEditorPosValid = false;
    }

    m_cachedTextWidth = -1.0;
    m_cachedIdealWidth = -1.0;
    m_documentMetricsDirty = true;

    applyAlignmentToEditor();
}

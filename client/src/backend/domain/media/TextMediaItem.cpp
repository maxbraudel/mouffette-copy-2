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
#include <array>
#include <limits>
#include <functional>
#include "frontend/rendering/canvas/OverlayPanels.h"
#include <QObject>
#include <QScopedValueRollback>
#include <QTimer>
#include <QPalette>
#include <chrono>
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

namespace {

constexpr qreal kContentPadding = 4.0;
constexpr qreal kStrokeOverflowScale = 0.75;
constexpr qreal kStrokeOverflowMinPx = 1.5;
constexpr int kScaledRasterThrottleIntervalMs = 20;
constexpr qreal kFitToTextMinWidth = 24.0;
constexpr int kFitToTextSizeStabilizationPx = 1;
constexpr qreal kMaxOutlineThicknessFactor = 0.35;
constexpr qreal kOutlineCurveExponent = 1.35;
constexpr qreal kMaxOutlineStrokePx = 14.0;
constexpr qreal kMinOutlineStrokePx = 0.25;
constexpr qreal kBorderWidthQuantizationStepPercent = 1.0;
constexpr int kFallbackFontPixelSize = 12;

bool textHotLogsEnabled() {
    static const bool enabled = false;
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
    const qreal eased = std::pow(normalized, kOutlineCurveExponent);
    const qreal scaledStroke = eased * kMaxOutlineThicknessFactor * reference;
    return std::clamp(scaledStroke, 0.0, kMaxOutlineStrokePx);
}

QRectF computeDocumentTextBounds(const QTextDocument& doc, QAbstractTextDocumentLayout* layout) {
    if (!layout) {
        return QRectF();
    }

    const auto lineVisualRectInDocument = [](const QRectF& blockRect, const QTextLine& line) {
        const QRectF naturalRect = line.naturalTextRect();
        if (naturalRect.isValid() && naturalRect.width() > 0.0 && naturalRect.height() > 0.0) {
            return naturalRect.translated(blockRect.topLeft());
        }

        return QRectF(blockRect.left() + line.x(),
                      blockRect.top() + line.y(),
                      std::max<qreal>(line.naturalTextWidth(), 1.0),
                      std::max<qreal>(line.height(), 1.0));
    };

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

            const QRectF lineRect = lineVisualRectInDocument(blockRect, line);
            bounds = hasBounds ? bounds.united(lineRect) : lineRect;
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
        setAcceptedMouseButtons(Qt::NoButton);

        m_caretBlinkTimer.setSingleShot(false);
        m_caretBlinkTimer.setTimerType(Qt::CoarseTimer);
        const int interval = caretBlinkInterval();
        if (interval > 0) {
            m_caretBlinkTimer.setInterval(interval);
        }
        QObject::connect(&m_caretBlinkTimer, &QTimer::timeout, this, [this]() {
            if (textCursor().hasSelection()) {
                return;
            }
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

        // Keep blink timer idle while selection is active to avoid unnecessary repaints.
        // Formatting normalization is handled at edit start and explicit style changes.
        updateCaretBlinkState();
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override {
        if (!painter) {
            return;
        }

        painter->save();
        if (m_owner) {
            const QSize ownerBaseSize = m_owner->baseSizePx();
            const QRectF ownerBounds(0.0,
                                     0.0,
                                     std::max(1, ownerBaseSize.width()),
                                     std::max(1, ownerBaseSize.height()));
            const QRectF ownerBoundsInEditor = mapRectFromParent(ownerBounds);
            painter->setClipRect(ownerBoundsInEditor);
        }
        QGraphicsTextItem::paint(painter, option, widget);
        painter->restore();

        m_cacheDirty = false;
        m_cachedRenderScale = 1.0;
        if (!m_cachedImage.isNull()) {
            m_cachedImage = QImage();
        }
    }

    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        QGraphicsTextItem::mousePressEvent(event);
        invalidateCache(true);
        updateCaretBlinkState();
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
        updateCaretBlinkState();
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        QGraphicsTextItem::mouseReleaseEvent(event);
        invalidateCache(false);
        updateCaretBlinkState();
    }

private:
    void startCaretBlink(bool resetVisible) {
        const int interval = caretBlinkInterval();
        if (textCursor().hasSelection()) {
            if (m_caretBlinkTimer.isActive()) {
                m_caretBlinkTimer.stop();
            }
            return;
        }

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

    void updateCaretBlinkState() {
        if (textCursor().hasSelection()) {
            if (m_caretBlinkTimer.isActive()) {
                m_caretBlinkTimer.stop();
            }
            return;
        }
        startCaretBlink(false);
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

TextMediaItem::VectorDrawSnapshot TextMediaItem::captureVectorSnapshot(StrokeRenderMode mode) const {
    Q_UNUSED(mode);
    VectorDrawSnapshot snapshot;
    snapshot.text = textForRendering();
    snapshot.font = m_font;
    snapshot.font = ensureRenderableFont(snapshot.font, mediaId(), "snapshot");
    snapshot.fillColor = m_textColor;
    snapshot.outlineColor = m_textBorderColor;
    snapshot.outlineWidthPercent = m_textBorderWidthPercent;
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
    Q_UNUSED(reason);
    m_pendingGeometryCommitSize.reset();
    if (invalidateLayout) {
        m_layoutSnapshot.valid = false;
        m_documentMetricsDirty = true;
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

void TextMediaItem::paintVectorSnapshot(QPainter* painter, const VectorDrawSnapshot& snapshot, const QSize& targetSize, qreal scaleFactor, qreal canvasZoom, const QRectF& viewport, const std::atomic<bool>* cancelled) {
    if (!painter) {
        return;
    }

    QElapsedTimer snapshotTimer;
    snapshotTimer.start();
    const bool hasViewport = viewport.isValid() && !viewport.isEmpty();

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
        qreal maxLineWidth = 1.0;
        if (QAbstractTextDocumentLayout* docLayout = doc.documentLayout()) {
            for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
                if (QTextLayout* textLayout = block.layout()) {
                    for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
                        const QTextLine line = textLayout->lineAt(lineIndex);
                        if (!line.isValid()) {
                            continue;
                        }
                        maxLineWidth = std::max(maxLineWidth, line.naturalTextWidth());
                    }
                }
            }
            maxLineWidth = std::max(maxLineWidth, docLayout->documentSize().width());
        }
        doc.setTextWidth(std::max<qreal>(1.0, maxLineWidth));
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
    // Clamp: never push text above the top margin.
    // When the box is too short to hold the wrapped text (e.g. during a fast alt-resize
    // that narrows the width while height is fixed, producing more line-breaks than the
    // height can accommodate), a Center/Bottom offsetY can go negative, causing the first
    // lines to be painted above y=0. This makes partially-visible lines from the top
    // bleed into the next visible line, creating apparent visual overlap. By clamping to
    // the top-aligned position we fall back to top-clipping which is visually correct.
    const qreal topAlignedOffsetY = margin - docVisualTop;
    if (offsetY < topAlignedOffsetY) {
        offsetY = topAlignedOffsetY;
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
    const qreal strokeWidthRaw = baseStrokeWidth * uniformScale;
    const qreal strokeWidth = (strokeWidthRaw >= kMinOutlineStrokePx)
        ? std::min(strokeWidthRaw, kMaxOutlineStrokePx)
        : 0.0;
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

    auto drawDocumentWithStrokeBehindFill = [&](qint64* outlineMsOut,
                                                int* glyphCountOut,
                                                qreal translateX,
                                                qreal translateY) {
        if (outlineMsOut) {
            *outlineMsOut = 0;
        }
        if (glyphCountOut) {
            *glyphCountOut = 0;
        }

        if (strokeWidth <= 0.0) {
            QAbstractTextDocumentLayout::PaintContext ctx;
            ctx.cursorPosition = -1;
            ctx.palette.setColor(QPalette::Text, fillColor);
            if (hasViewport) {
                ctx.clip = viewport.translated(-translateX, -translateY);
            }
            layout->draw(painter, ctx);
            return;
        }

        QElapsedTimer outlineTimer;
        outlineTimer.start();

        // Pass 1: fill only
        {
            QTextCursor cursor(&doc);
            cursor.select(QTextCursor::Document);
            QTextCharFormat fillFormat;
            fillFormat.setForeground(fillColor);
            fillFormat.clearProperty(QTextFormat::TextOutline);
            cursor.mergeCharFormat(fillFormat);
        }

        {
            QAbstractTextDocumentLayout::PaintContext ctx;
            ctx.cursorPosition = -1;
            ctx.palette.setColor(QPalette::Text, fillColor);
            if (hasViewport) {
                ctx.clip = viewport.translated(-translateX, -translateY);
            }
            layout->draw(painter, ctx);
        }

        // Pass 2: outline only, drawn behind fill so it appears outside
        {
            QTextCursor cursor(&doc);
            cursor.select(QTextCursor::Document);
            QTextCharFormat strokeFormat;
            QColor transparentFill = fillColor;
            transparentFill.setAlpha(0);
            strokeFormat.setForeground(transparentFill);
            strokeFormat.setTextOutline(QPen(outlineColor, strokeWidth * 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            cursor.mergeCharFormat(strokeFormat);
        }

        {
            painter->save();
            painter->setCompositionMode(QPainter::CompositionMode_DestinationOver);
            QAbstractTextDocumentLayout::PaintContext ctx;
            ctx.cursorPosition = -1;
            ctx.palette.setColor(QPalette::Text, Qt::transparent);
            if (hasViewport) {
                ctx.clip = viewport.translated(-translateX, -translateY);
            }
            layout->draw(painter, ctx);
            painter->restore();
        }

        if (outlineMsOut) {
            *outlineMsOut = outlineTimer.elapsed();
        }
        if (glyphCountOut) {
            *glyphCountOut = snapshot.text.size();
        }
    };

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
                if (hasViewport) {
                    const QRectF blockInItemCoords = blockRect.translated(offsetX, offsetY);
                    if (!viewport.intersects(blockInItemCoords)) {
                        continue;
                    }
                }
                for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
                    QTextLine line = textLayout->lineAt(lineIndex);
                    if (!line.isValid()) continue;

                    QRectF lineRect = line.naturalTextRect();
                    if (lineRect.isValid() && lineRect.width() > 0.0 && lineRect.height() > 0.0) {
                        lineRect.translate(blockRect.topLeft());
                    } else {
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
                        lineRect = QRectF(blockRect.topLeft() + QPointF(alignedX, line.y()), QSizeF(width, height));
                    }
                    painter->drawRect(lineRect);
                }
            }

            painter->restore();
        }

        int glyphCount = 0;
        qint64 outlineMs = 0;
        drawDocumentWithStrokeBehindFill(&outlineMs, &glyphCount, offsetX, offsetY);

        painter->restore();
        logSnapshotPerf("vector-full", outlineMs, glyphCount, snapshotTimer.elapsed());
        return;
    }

    painter->translate(offsetX, offsetY);
    int glyphCount = 0;
    qint64 outlineMs = 0;

    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        QTextLayout* textLayout = block.layout();
        if (!textLayout) continue;

        const QRectF blockRect = layout->blockBoundingRect(block);
        if (hasViewport) {
            const QRectF blockInItemCoords = blockRect.translated(offsetX, offsetY);
            if (!viewport.intersects(blockInItemCoords)) {
                continue;
            }
        }
        for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
            QTextLine line = textLayout->lineAt(lineIndex);
            if (!line.isValid()) continue;

            const qreal lineWidth = line.naturalTextWidth();
            const qreal lineOffsetX = line.x();

            if (highlightEnabled && highlightColor.alpha() > 0) {
                const qreal highlightWidth = std::max<qreal>(lineWidth > 0.0 ? lineWidth : availableWidth, 1.0);
                const qreal highlightHeight = std::max<qreal>(line.height(), 1.0);
                const QRectF highlightRect(QPointF(lineOffsetX, blockRect.top() + line.y()), QSizeF(highlightWidth, highlightHeight));
                painter->fillRect(highlightRect, highlightColor);
            }
        }
    }

    drawDocumentWithStrokeBehindFill(&outlineMs, &glyphCount, offsetX, offsetY);

    painter->restore();
    logSnapshotPerf("vector-fit", outlineMs, glyphCount, snapshotTimer.elapsed());
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
    setFlag(QGraphicsItem::ItemClipsChildrenToShape, true);
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
    
    m_editorRenderingText = m_text;
    m_uniformScaleFactor = scale();
    m_lastObservedScale = scale();
    m_appliedContentPaddingPx = contentPaddingPx();
    
    // Create alignment controls (will be shown when selected)
    ensureAlignmentControls();
    ensureInlineEditor();

    setFitToTextEnabled(true);
    // Ensure initial geometry is already fit-to-text before first paint.
    // Without this, the first frame can show default 400x200 then snap.
    applyFitToTextNow();
}

void TextMediaItem::setText(const QString& text) {
    if (m_text != text) {
        m_text = text;
        m_editorRenderingText = text;
        m_documentMetricsDirty = true;
        m_cachedEditorPosValid = false;
        invalidateRenderPipeline(InvalidationReason::Content, true);

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
    }
}

void TextMediaItem::setFont(const QFont& font) {
    QFont adjustedFont = font;
    m_fontWeightValue = canonicalCssWeight(adjustedFont);
    adjustedFont = fontAdjustedForWeight(adjustedFont, m_fontWeightValue);
    adjustedFont = ensureRenderableFont(adjustedFont, mediaId(), "setFont");
    
    applyFontChange(adjustedFont);
}

void TextMediaItem::setTextColor(const QColor& color) {
    if (m_textColor == color) {
        return;
    }

    m_textColor = color;
    m_cachedTextColor = color;
    
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
    const qreal quantized = std::clamp(
        std::round(clamped / kBorderWidthQuantizationStepPercent) * kBorderWidthQuantizationStepPercent,
        0.0,
        100.0);
    if (std::abs(m_textBorderWidthPercent - quantized) < 1e-4) {
        return;
    }

    const qreal oldPadding = m_appliedContentPaddingPx;
    if (textHotLogsEnabled()) {
        qDebug() << "[TextMediaItem]" << mediaId() << "setTextBorderWidth%" << quantized
                 << "approxPx" << computeStrokeWidthFromFont(m_font, quantized)
                 << "textLength" << m_text.size();
    }
    m_textBorderWidthPercent = quantized;
    const qreal newPadding = contentPaddingPx();

    invalidateRenderPipeline(InvalidationReason::VisualStyle, false);
    update();

    handleContentPaddingChanged(oldPadding, newPadding);
    m_appliedContentPaddingPx = newPadding;

    if (m_inlineEditor) {
        normalizeTextFormatting(m_inlineEditor, m_font, m_textColor, m_textBorderColor, borderStrokeWidthPx());
        if (auto* inlineEditor = toInlineEditor(m_inlineEditor)) {
            inlineEditor->invalidateCache();
        }
    }

    if (m_fitToTextEnabled) {
        scheduleFitToTextUpdate();
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
    m_inlineEditor->setAcceptedMouseButtons(Qt::AllButtons);

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
    editor->setAcceptedMouseButtons(Qt::NoButton);

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
            // Match applyFitModeConstraintsToEditor: visual wrap = (baseWidth - 2*margin),
            // so logical width (before editor's scale(uniformScale) transform) must be
            // (baseWidth - 2*margin) / uniformScale.
            const qreal logicalWidth = std::max<qreal>(1.0, (static_cast<qreal>(m_baseSize.width()) - 2.0 * margin) / uniformScale);
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
    
    // Compute logical content dimensions that match paintVectorSnapshot exactly.
    // paintVectorSnapshot wraps at (m_baseSize.width() - 2*margin) in its own logical
    // space (scale-independent). The editor renders with transform scale(uniformScale),
    // so its *logical* text width must be (m_baseSize.width() - 2*margin) / uniformScale
    // so that the *visual* wrap = logicalContentWidth * uniformScale = m_baseSize.width() - 2*margin.
    // The previous formula  (bounds.width()/uniformScale - 2*marginLogical) was equivalent to
    // (m_baseSize.width() - 2*margin*uniformScale) / uniformScale, giving visual wrap
    // m_baseSize.width() - 2*margin*uniformScale — incorrect when uniformScale != 1.
    const qreal logicalContentWidth = std::max<qreal>(1.0, (bounds.width() - 2.0 * marginLogical) / uniformScale);
    const qreal logicalContentHeight = std::max<qreal>(1.0, (bounds.height() - 2.0 * marginLogical) / uniformScale);
    
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
    // Clamp: mirror the same guard as paintVectorSnapshot — never push the editor above
    // the content area top. When the box is narrowed via alt-resize (height fixed, more
    // line-breaks needed), Center/Bottom can produce a negative offsetY that shifts the
    // editor outside the bounding rect. Without ItemClipsChildrenToShape on the parent,
    // this causes lines to visually overlap or bleed onto adjacent scene items.
    const qreal topAlignedOffsetY = -visualDocTop;
    if (offsetY < topAlignedOffsetY) {
        offsetY = topAlignedOffsetY;
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
        // Keep hidden editor in sync with base size so entering edit mode wraps correctly.
        // During active handle resize (especially Alt stretch), avoid synchronous document
        // relayout on every mouse-move to keep the UI thread responsive.
        if (m_activeHandle == None) {
            syncInlineEditorToBaseSize();
        }
    }
    
    // Update alignment controls position (similar to video controls)
    updateAlignmentControlsLayout();
    
    update();

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
    m_inlineEditor->setAcceptedMouseButtons(Qt::NoButton);
    m_inlineEditor->clearFocus();
    m_inlineEditor->setEnabled(false);
    m_inlineEditor->setVisible(true);
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

    invalidateRenderPipeline(InvalidationReason::Content, false);

    updateInlineEditorGeometry();
    if (m_fitToTextEnabled) {
        scheduleFitToTextUpdate();
    }
    update();
}

void TextMediaItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
    Q_UNUSED(option);
    Q_UNUSED(widget);

    if (!painter) {
        return;
    }

    if (!m_inlineEditor) {
        ensureInlineEditor();
    }

    if (m_inlineEditor) {
        if (!m_isEditing) {
            QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);
            if (m_inlineEditor->toPlainText() != m_text) {
                m_inlineEditor->setPlainText(m_text);
                m_documentMetricsDirty = true;
                m_cachedEditorPosValid = false;
            }
            m_inlineEditor->setTextInteractionFlags(Qt::NoTextInteraction);
            m_inlineEditor->setAcceptedMouseButtons(Qt::NoButton);
            m_inlineEditor->setEnabled(false);
        } else {
            m_inlineEditor->setAcceptedMouseButtons(Qt::AllButtons);
        }

        updateInlineEditorGeometry();

        const bool showContent = m_contentVisible && (m_contentOpacity > 0.0) && (m_contentDisplayOpacity > 0.0);
        m_inlineEditor->setVisible(showContent);
        m_inlineEditor->setOpacity(showContent ? (m_contentOpacity * m_contentDisplayOpacity) : 0.0);
    }

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
        update();
    }

    if (change == ItemTransformHasChanged ||
        change == ItemPositionHasChanged ||
        change == ItemSelectedChange ||
        change == ItemSelectedHasChanged) {
        if (m_activeHandle == None || m_isEditing) {
            updateInlineEditorGeometry();
        }
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

QPointF TextMediaItem::anchorPointForAlignment(const QSize& size) const {
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
}

void TextMediaItem::commitBaseSizeKeepingAnchor(const QSize& newBase) {
    const QSize oldBase = m_baseSize;
    if (newBase == oldBase) {
        return;
    }

    const QPointF oldAnchorLocal = anchorPointForAlignment(oldBase);
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

    const QPointF newAnchorLocal = anchorPointForAlignment(newBase);
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

void TextMediaItem::setFitToTextEnabled(bool enabled) {
    if (m_fitToTextEnabled == enabled) {
        return;
    }

    m_fitToTextEnabled = enabled;
    // Invalidate all caches when fit mode changes because text layout is completely different
    invalidateRenderPipeline(InvalidationReason::Geometry, true);
    // Keep the last completed scaled raster alive as visual fallback while the
    // new async raster is computed. Clearing here causes a blank frame on the
    // first Alt-resize transition (fit-to-text -> free resize).
    if (textHotLogsEnabled()) {
        qDebug() << "[TextMedia][FitMode] toggled" << (m_fitToTextEnabled ? "ON" : "OFF")
                 << "- invalidating caches for item" << mediaId();
    }
    
    if (!m_fitToTextEnabled) {
        m_fitToTextUpdatePending = false;
        m_pendingGeometryCommitSize.reset();
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
        const QRectF docBounds = computeDocumentTextBounds(*doc, layout);
        logicalContentHeight = std::max<qreal>(1.0, docBounds.height());
    } else {
        logicalContentHeight = std::max<qreal>(1.0, doc->size().height());
    }

    const qreal uniformScale = std::max(std::abs(m_uniformScaleFactor), 1e-4);
    const qreal marginLogical = contentPaddingPx();
    const qreal marginScene = marginLogical * uniformScale;

    const qreal strokeWidthLogical = borderStrokeWidthPx();
    const qreal borderPaddingLogical = (strokeWidthLogical > 0.0)
        ? (strokeWidthLogical + std::max<qreal>(kStrokeOverflowMinPx, strokeWidthLogical * kStrokeOverflowScale + 1.0))
        : 0.0;
    const qreal borderPaddingScene = borderPaddingLogical * uniformScale;

    // Calculate dimensions with minimum width constraint in fit-to-text mode
    const int calculatedWidth = static_cast<int>(std::ceil(logicalContentWidth * uniformScale + marginScene * 2.0 + borderPaddingScene * 2.0));
    const int minWidth = static_cast<int>(std::ceil(kFitToTextMinWidth * uniformScale + marginScene * 2.0 + borderPaddingScene * 2.0));
    
    QSize newBase(
        std::max(minWidth, std::max(1, calculatedWidth)),
        std::max(1, static_cast<int>(std::ceil(logicalContentHeight * uniformScale + marginScene * 2.0 + borderPaddingScene * 2.0))));

    const QSize oldBase = m_baseSize;

    if (std::abs(newBase.width() - oldBase.width()) <= kFitToTextSizeStabilizationPx &&
        std::abs(newBase.height() - oldBase.height()) <= kFitToTextSizeStabilizationPx) {
        newBase = oldBase;
    }

    if (newBase != oldBase) {
        m_pendingGeometryCommitSize = newBase;
        invalidateRenderPipeline(InvalidationReason::Geometry, true);
        commitBaseSizeKeepingAnchor(newBase);
        m_pendingGeometryCommitSize.reset();
        m_cachedEditorPosValid = false;
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
    // The inline editor is rendered with a transform of scale(uniformScale), so its
    // document coordinates are in "logical" units. The raster (paintVectorSnapshot) wraps
    // at `m_baseSize.width() - 2*margin` (logical pixels, scale-independent). To produce
    // the same visual wrap width in the editor we need:
    //   desiredTextWidth * uniformScale == m_baseSize.width() - 2*margin
    //   → desiredTextWidth = (m_baseSize.width() - 2*margin) / uniformScale
    // The previous formula  (m_baseSize.width() / uniformScale - 2*margin) was wrong:
    // it gave visual wrap = m_baseSize.width() - 2*margin*uniformScale, which diverges
    // from the raster when uniformScale != 1 and caused editors for scaled items to
    // break lines at different points than the displayed raster.
    qreal desiredTextWidth = std::max<qreal>(1.0, (static_cast<qreal>(m_baseSize.width()) - 2.0 * margin) / uniformScale);

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

            if (m_fitToTextEnabled) {
                doc->adjustSize();
                qreal maxLineWidth = 1.0;
                for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
                    if (QTextLayout* textLayout = block.layout()) {
                        for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
                            const QTextLine line = textLayout->lineAt(lineIndex);
                            if (!line.isValid()) {
                                continue;
                            }
                            maxLineWidth = std::max(maxLineWidth, line.naturalTextWidth());
                        }
                    }
                }
                maxLineWidth = std::max(maxLineWidth, doc->idealWidth());
                desiredTextWidth = std::max<qreal>(1.0, maxLineWidth);
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

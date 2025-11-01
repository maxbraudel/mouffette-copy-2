// TextMediaItem.cpp - Implementation of text media item
#include "TextMediaItem.h"
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QGraphicsSceneMouseEvent>
#include <algorithm>
#include <cmath>
#include <QGraphicsTextItem>
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
#include <QBrush>
#include <QClipboard>
#include <QApplication>
#include <QImage>
#include <QGraphicsRectItem>
#include <QGraphicsSvgItem>
#include "frontend/rendering/canvas/OverlayPanels.h"
#include "frontend/rendering/canvas/SegmentedButtonItem.h"
#include "frontend/ui/theme/AppColors.h"
#include <QObject>
#include <QScopedValueRollback>
#include <QTimer>
#include <QPalette>

// Global text styling configuration - tweak these to change all text media appearance
namespace TextMediaDefaults {
    const QString FONT_FAMILY = QStringLiteral("Arial");
    const int FONT_SIZE = 24;
    const QFont::Weight FONT_WEIGHT = QFont::Bold;
    const bool FONT_ITALIC = false;
    const QColor TEXT_COLOR = Qt::white;
    
    // Default text content when creating new text media
    const QString DEFAULT_TEXT = QStringLiteral("No Text");
    
    // Default size when creating new text media (width x height in pixels)
    const int DEFAULT_WIDTH = 150;
    const int DEFAULT_HEIGHT = 75;
    
    // Default text scale
    const qreal DEFAULT_SCALE = 2.0;
}

namespace {

constexpr qreal kContentPadding = 0.0;

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
        const int width = std::max(1, static_cast<int>(std::ceil(bounds.width())));
        const int height = std::max(1, static_cast<int>(std::ceil(bounds.height())));

        const bool skipDocRendering = (m_owner && m_owner->isEditing());
        if (!skipDocRendering) {
            if (m_cacheDirty || m_cachedImage.size() != QSize(width, height)) {
                m_cachedImage = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
                m_cachedImage.fill(Qt::transparent);

                QPainter bufferPainter(&m_cachedImage);
                bufferPainter.setRenderHint(QPainter::Antialiasing, true);
                bufferPainter.setRenderHint(QPainter::TextAntialiasing, true);
                bufferPainter.translate(-bounds.topLeft());
                // Render only the base text content; caret and selection are drawn as overlays
                if (QTextDocument* doc = document()) {
                    QAbstractTextDocumentLayout::PaintContext ctx;
                    ctx.cursorPosition = -1; // hide caret in cached image
                    ctx.palette.setColor(QPalette::Text, defaultTextColor());
                    doc->documentLayout()->draw(&bufferPainter, ctx);
                } else {
                    QGraphicsTextItem::paint(&bufferPainter, &opt, widget);
                }
                bufferPainter.end();

                m_cacheDirty = false;
            }

            painter->drawImage(bounds.topLeft(), m_cachedImage);
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
    mutable bool m_cacheDirty = true;
    QTimer m_caretBlinkTimer;
    bool m_caretVisible = true;
};

static InlineTextEditor* toInlineEditor(QGraphicsTextItem* item) {
    return static_cast<InlineTextEditor*>(item);
}

static void normalizeTextFormatting(QGraphicsTextItem* editor, const QFont& currentFont, const QColor& currentColor) {
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
    QTextCharFormat standardFormat;
    standardFormat.setFont(currentFont);
    standardFormat.setForeground(QBrush(currentColor));

    // Apply to all text
    cursor.mergeCharFormat(standardFormat);
    cursor.endEditBlock();

    editor->setTextCursor(restoreCursor);
}

static void applyCenterAlignment(QGraphicsTextItem* editor) {
    if (!editor) {
        return;
    }

    if (QTextDocument* doc = editor->document()) {
        QTextCursor activeCursor = editor->textCursor();
        QTextCursor cursor(doc);
        cursor.select(QTextCursor::Document);
        QTextBlockFormat blockFormat = cursor.blockFormat();
        blockFormat.setAlignment(Qt::AlignHCenter);
        cursor.mergeBlockFormat(blockFormat);
        cursor.clearSelection();
        editor->setTextCursor(activeCursor);
    }
}

} // anonymous namespace

TextMediaItem::TextMediaItem(
    const QSize& initialSize,
    int visualSizePx,
    int selectionSizePx,
    const QString& initialText
)
    : ResizableMediaBase(initialSize, visualSizePx, selectionSizePx, QStringLiteral("Text"))
    , m_text(initialText)
    , m_textColor(TextMediaDefaults::TEXT_COLOR)
{
    // Set up default font from global configuration
    m_font = QFont(TextMediaDefaults::FONT_FAMILY, TextMediaDefaults::FONT_SIZE);
    m_font.setWeight(TextMediaDefaults::FONT_WEIGHT);
    m_font.setItalic(TextMediaDefaults::FONT_ITALIC);
    
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
    
    // Create alignment controls (will be shown when selected)
    ensureAlignmentControls();
}

void TextMediaItem::setText(const QString& text) {
    if (m_text != text) {
        m_text = text;
        m_editorRenderingText = text;
        m_pendingAutoSize = true;
        m_documentMetricsDirty = true;
        m_cachedEditorPosValid = false;
        m_needsRasterization = true;
        m_scaledRasterDirty = true;
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
    }
}

void TextMediaItem::setFont(const QFont& font) {
    if (m_font == font) {
        return;
    }

    m_font = font;
    m_documentMetricsDirty = true;
    m_cachedEditorPosValid = false;
    m_needsRasterization = true;
    m_scaledRasterDirty = true;
    update();
    updateInlineEditorGeometry();
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
        if (auto* inlineEditor = toInlineEditor(m_inlineEditor)) {
            inlineEditor->invalidateCache();
        }
    }
    m_needsRasterization = true;
    m_scaledRasterDirty = true;
}

void TextMediaItem::normalizeEditorFormatting() {
    if (!m_inlineEditor || !m_isEditing) {
        return;
    }
    // Use base font (transform will scale it)
    normalizeTextFormatting(m_inlineEditor, m_font, m_textColor);
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
    
    // Set editor document width at base size (transform will scale it visually)
    if (QTextDocument* doc = m_inlineEditor->document()) {
        QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);
        doc->setTextWidth(m_baseSize.width() - 2.0 * kContentPadding);
        applyCenterAlignment(m_inlineEditor);
    }

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
            opt.setAlignment(Qt::AlignHCenter);
            doc->setDefaultTextOption(opt);
            // Set width at base size (transform will scale it visually)
            doc->setTextWidth(m_baseSize.width() - 2.0 * kContentPadding);
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
            
            // Only trigger auto-resize if content actually changed
            if (contentChanged) {
                m_pendingAutoSize = true;
                handleInlineEditorTextChanged(newText);
            }
            
            if (m_isUpdatingInlineGeometry) {
                return;
            }
            
            // Only update geometry if content changed (not just cursor movement)
            if (contentChanged) {
                updateInlineEditorGeometry();
            }
        });
    }

    {
        QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);
        applyCenterAlignment(editor);
    }

    m_inlineEditor = editor;
    editor->invalidateCache();
}

void TextMediaItem::handleInlineEditorTextChanged(const QString& newText) {
    if (!m_isEditing) {
        return;
    }

    if (m_editorRenderingText == newText) {
        return;
    }

    m_editorRenderingText = newText;
    m_needsRasterization = true;
    m_scaledRasterDirty = true;
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

    const bool allowAutoSize = m_pendingAutoSize;
    if (allowAutoSize) {
        m_pendingAutoSize = false;
    }

    const qreal margin = kContentPadding;
    const qreal uniformScale = std::max(std::abs(m_uniformScaleFactor), 1e-4);

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
    QRectF contentRect = bounds.adjusted(margin, margin, -margin, -margin);
    if (contentRect.width() < 1.0) {
        contentRect.setWidth(1.0);
    }
    if (contentRect.height() < 1.0) {
        contentRect.setHeight(1.0);
    }

    const qreal visualContentWidth = contentRect.width();
    const qreal visualContentHeight = contentRect.height();
    const qreal logicalContentWidth = std::max<qreal>(1.0, visualContentWidth / uniformScale);
    const qreal logicalContentHeight = std::max<qreal>(1.0, visualContentHeight / uniformScale);

    bool widthChanged = false;
    if (m_cachedTextWidth < 0.0 || floatsDiffer(logicalContentWidth, m_cachedTextWidth, 1e-3)) {
        m_inlineEditor->setTextWidth(logicalContentWidth);
        m_cachedTextWidth = logicalContentWidth;
        widthChanged = true;
        m_documentMetricsDirty = true;
    }

    QTextDocument* doc = m_inlineEditor->document();
    qreal docIdealWidth = m_cachedIdealWidth;
    qreal logicalDocHeight = std::max<qreal>(1.0, m_cachedDocumentSize.height());

    const bool needMetrics = doc && (allowAutoSize || widthChanged || m_documentMetricsDirty || m_cachedIdealWidth < 0.0);
    if (needMetrics) {
        docIdealWidth = doc ? doc->idealWidth() : logicalContentWidth;
        QAbstractTextDocumentLayout* layout = doc ? doc->documentLayout() : nullptr;
        if (doc && layout) {
            QSizeF size = layout->documentSize();
            if (size.width() <= 0.0) {
                size.setWidth(logicalContentWidth);
            }
            if (size.height() <= 0.0) {
                size.setHeight(logicalContentHeight);
            }
            m_cachedDocumentSize = size;
            logicalDocHeight = std::max<qreal>(1.0, size.height());
        } else {
            m_cachedDocumentSize = QSizeF(logicalContentWidth, logicalContentHeight);
            logicalDocHeight = std::max<qreal>(1.0, logicalContentHeight);
        }
        m_cachedIdealWidth = docIdealWidth;
        m_documentMetricsDirty = false;
    } else {
        if (m_cachedIdealWidth < 0.0) {
            docIdealWidth = logicalContentWidth;
        }
        logicalDocHeight = std::max<qreal>(1.0, m_cachedDocumentSize.height());
    }

    QSize newBaseSize = m_baseSize;
    if (doc && allowAutoSize) {
        const qreal requiredVisualWidth = docIdealWidth * uniformScale + margin * 2.0;
        if (requiredVisualWidth > static_cast<qreal>(newBaseSize.width()) + 0.5) {
            newBaseSize.setWidth(static_cast<int>(std::ceil(requiredVisualWidth)));
        }

        const qreal requiredVisualHeight = logicalDocHeight * uniformScale + margin * 2.0;
        if (requiredVisualHeight > static_cast<qreal>(newBaseSize.height()) + 0.5) {
            newBaseSize.setHeight(static_cast<int>(std::ceil(requiredVisualHeight)));
        }
    }

    const bool geometryChanged = (newBaseSize != m_baseSize);
    if (geometryChanged) {
        prepareGeometryChange();
        m_baseSize = newBaseSize;
        m_needsRasterization = true;
        m_scaledRasterDirty = true;
        m_lastRasterizedScale = 1.0;
        bounds = boundingRect();
        contentRect = bounds.adjusted(margin, margin, -margin, -margin);
        if (contentRect.width() < 1.0) {
            contentRect.setWidth(1.0);
        }
        if (contentRect.height() < 1.0) {
            contentRect.setHeight(1.0);
        }
    }

    const qreal visualDocWidth = std::max<qreal>(1.0, m_cachedDocumentSize.width() * uniformScale);
    const qreal visualDocHeight = std::max<qreal>(1.0, logicalDocHeight * uniformScale);

    const qreal availableWidth = std::max<qreal>(1.0, contentRect.width());
    const qreal availableHeight = std::max<qreal>(1.0, contentRect.height());
    const qreal offsetX = (availableWidth - visualDocWidth) * 0.5;
    const qreal offsetY = (availableHeight - visualDocHeight) * 0.5;
    const QPointF newEditorPos = contentRect.topLeft() + QPointF(offsetX, offsetY);

    if (!m_cachedEditorPosValid || floatsDiffer(newEditorPos.x(), m_cachedEditorPos.x(), 0.1) || floatsDiffer(newEditorPos.y(), m_cachedEditorPos.y(), 0.1)) {
        m_inlineEditor->setPos(newEditorPos);
        m_cachedEditorPos = newEditorPos;
        m_cachedEditorPosValid = true;
    }

    m_inlineEditor->setFlag(QGraphicsItem::ItemClipsToShape, true);

    if (geometryChanged) {
        if (m_inlineEditor) {
            QTimer::singleShot(0, m_inlineEditor, [this]() {
                if (!m_beingDeleted) {
                    updateOverlayLayout();
                    update();
                }
            });
        }
    }

    const bool hasPendingResize = m_pendingAutoSize;
    if (hasPendingResize && m_inlineEditor) {
        QTimer::singleShot(0, m_inlineEditor, [this]() {
            updateInlineEditorGeometry();
        });
    }
}

void TextMediaItem::onInteractiveGeometryChanged() {
    ResizableMediaBase::onInteractiveGeometryChanged();

    if (m_isEditing) {
        // Update editor geometry to follow Alt-resize base size changes
        updateInlineEditorGeometry();
    }
    
    m_scaledRasterDirty = true;
    update();
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
            m_needsRasterization = true;
            m_scaledRasterDirty = true;
            m_pendingAutoSize = true;
        } else {
            m_editorRenderingText = m_text;
        }
    }
    m_textBeforeEditing.clear();

    // Rasterize text after editing completes
    m_needsRasterization = true;
    m_scaledRasterDirty = true;

    updateInlineEditorGeometry();
    update();
}

void TextMediaItem::renderTextToImage(QImage& target, const QSize& imageSize, qreal scaleFactor) {
    const int targetWidth = std::max(1, imageSize.width());
    const int targetHeight = std::max(1, imageSize.height());

    const qreal epsilon = 1e-4;
    const qreal effectiveScale = std::max(std::abs(scaleFactor), epsilon);

    target = QImage(targetWidth, targetHeight, QImage::Format_ARGB32_Premultiplied);
    target.fill(Qt::transparent);

    QPainter imagePainter(&target);
    imagePainter.setRenderHint(QPainter::Antialiasing, true);
    imagePainter.setRenderHint(QPainter::TextAntialiasing, true);
    imagePainter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QString& text = textForRendering();

    QTextOption option;
    option.setWrapMode(QTextOption::WordWrap);
    option.setAlignment(Qt::AlignCenter);

    QTextDocument doc;
    doc.setDocumentMargin(0.0);
    doc.setDefaultFont(m_font);
    doc.setDefaultTextOption(option);
    doc.setPlainText(text);

    const qreal logicalWidth = static_cast<qreal>(targetWidth) / effectiveScale;
    const qreal logicalHeight = static_cast<qreal>(targetHeight) / effectiveScale;
    const qreal availableWidth = std::max<qreal>(1.0, logicalWidth - 2.0 * kContentPadding);
    doc.setTextWidth(availableWidth);

    const QSizeF docSize = doc.documentLayout()->documentSize();
    const qreal availableHeight = logicalHeight - 2.0 * kContentPadding;
    const qreal offsetX = kContentPadding + (availableWidth - docSize.width()) / 2.0;
    const qreal offsetY = kContentPadding + (availableHeight - docSize.height()) / 2.0;

    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette.setColor(QPalette::Text, m_textColor);

    imagePainter.scale(effectiveScale, effectiveScale);
    imagePainter.translate(offsetX, offsetY);
    doc.documentLayout()->draw(&imagePainter, ctx);
    imagePainter.end();
}

void TextMediaItem::rasterizeText() {
    if (!m_needsRasterization && m_lastRasterizedSize == m_baseSize) {
        return;
    }

    const QSize targetSize(std::max(1, m_baseSize.width()), std::max(1, m_baseSize.height()));
    renderTextToImage(m_rasterizedText, targetSize, 1.0);

    m_lastRasterizedSize = m_baseSize;
    m_needsRasterization = false;
    m_scaledRasterDirty = true;
}

void TextMediaItem::ensureScaledRaster(qreal visualScaleFactor, qreal geometryScale) {
    const qreal epsilon = 1e-4;
    const qreal effectiveScale = std::max(std::abs(visualScaleFactor), epsilon);
    const qreal boundedGeometryScale = std::max(std::abs(geometryScale), epsilon);

    const int targetWidth = std::max(1, static_cast<int>(std::ceil(static_cast<qreal>(m_baseSize.width()) * boundedGeometryScale)));
    const int targetHeight = std::max(1, static_cast<int>(std::ceil(static_cast<qreal>(m_baseSize.height()) * boundedGeometryScale)));
    const QSize targetSize(targetWidth, targetHeight);

    const bool scaleChanged = std::abs(effectiveScale - m_lastRasterizedScale) > epsilon;
    if (!m_scaledRasterDirty && !scaleChanged && m_scaledRasterizedText.size() == targetSize) {
        return;
    }

    renderTextToImage(m_scaledRasterizedText, targetSize, effectiveScale);

    m_lastRasterizedScale = effectiveScale;
    m_scaledRasterDirty = false;
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
        m_inlineEditor->setOpacity(m_contentOpacity * m_contentDisplayOpacity);
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
    const qreal effectiveScale = std::max(std::abs(currentScale) * uniformScale, 1e-4);
    const bool resizingUniformly = (m_activeHandle != None && !m_lastAxisAltStretch);
    const bool needsScaledRaster = std::abs(currentScale - 1.0) > 1e-4 ||
        std::abs(m_uniformScaleFactor - 1.0) > 1e-4 ||
        resizingUniformly;

    if (needsScaledRaster) {
        ensureScaledRaster(effectiveScale, currentScale);

        painter->save();
        if (std::abs(currentScale) > 1e-4) {
            painter->scale(1.0 / currentScale, 1.0 / currentScale);
        }
        const QTransform scaleTransform = (std::abs(currentScale) > 1e-4)
            ? QTransform::fromScale(currentScale, currentScale)
            : QTransform::fromScale(1.0, 1.0);
        QRectF scaledBounds = scaleTransform.mapRect(bounds);
        if (!m_scaledRasterizedText.isNull()) {
            painter->drawImage(scaledBounds, m_scaledRasterizedText);
        }
        painter->restore();
    } else {
        // Rasterize text if needed (once after editing/resizing)
        rasterizeText();

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
        
        // Detect when scale is being reset to 1.0 after a uniform resize (scale bake into base size)
        // This happens when user starts Alt-resize after doing uniform resize
        const bool scaleBeingBaked = (std::abs(newScale - 1.0) < epsilon) && (std::abs(oldScale - 1.0) > epsilon);
        
        if (scaleBeingBaked) {
            // When scale is baked back to 1.0 we need to carry the previous uniform zoom into
            // m_uniformScaleFactor so text rendering and inline editing keep their size.
            // Alt-resize mutates the base size, so accumulate the baked scale here as well.
            if (m_lastAxisAltStretch) {
                if (std::abs(oldScale) > epsilon) {
                    m_uniformScaleFactor *= std::abs(oldScale);
                }
                if (std::abs(m_uniformScaleFactor) < epsilon) {
                    m_uniformScaleFactor = 1.0;
                }
                // Force editor geometry update to sync with new base size
                m_cachedEditorPosValid = false;
                m_documentMetricsDirty = true;
            } else {
                // Normal bake: accumulate scale to preserve it
                if (std::abs(oldScale) > epsilon) {
                    m_uniformScaleFactor *= std::abs(oldScale);
                }
                if (std::abs(m_uniformScaleFactor) < epsilon) {
                    m_uniformScaleFactor = 1.0;
                }
            }
            m_scaledRasterDirty = true;
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
        updateAlignmentControlsLayout();
    }

    return result;
}

void TextMediaItem::ensureAlignmentControls() {
    if (m_alignmentControlsBg) return;
    
    // Create background container for all alignment controls
    m_alignmentControlsBg = new QGraphicsRectItem();
    m_alignmentControlsBg->setPen(Qt::NoPen);
    m_alignmentControlsBg->setBrush(Qt::NoBrush);
    m_alignmentControlsBg->setZValue(12000.0); // Same z-value as video controls
    m_alignmentControlsBg->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_alignmentControlsBg->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_alignmentControlsBg->setAcceptedMouseButtons(Qt::NoButton);
    m_alignmentControlsBg->setOpacity(1.0);
    m_alignmentControlsBg->setData(0, "overlay"); // Mark as overlay so canvas doesn't deselect on click
    if (scene()) scene()->addItem(m_alignmentControlsBg);
    
    // Helper lambda to apply segmented border styling
    auto applySegmentBorder = [](SegmentedButtonItem* item) {
        if (item) {
            QPen borderPen(AppColors::gOverlayBorderColor, 1);
            // Left segment has border on all sides except right
            // Middle segment has border on top and bottom only
            // Right segment has border on all sides except left
            if (item->segment() == SegmentedButtonItem::Segment::Left) {
                borderPen.setStyle(Qt::SolidLine);
            } else if (item->segment() == SegmentedButtonItem::Segment::Middle) {
                borderPen.setStyle(Qt::SolidLine);
            } else { // Right
                borderPen.setStyle(Qt::SolidLine);
            }
            item->setPen(borderPen);
        }
    };
    
    // Helper lambda to create divider
    auto makeDivider = [this](QGraphicsItem* parent) {
        auto* divider = new QGraphicsRectItem(parent);
        divider->setPen(Qt::NoPen);
        divider->setBrush(AppColors::gOverlayBorderColor);
        divider->setZValue(12001.5); // Between buttons and icons
        divider->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        divider->setAcceptedMouseButtons(Qt::NoButton);
        return divider;
    };
    
    // Helper lambda to create SVG icon
    auto makeSvg = [](const char* path, QGraphicsItem* parent) {
        auto* svg = new QGraphicsSvgItem(path, parent);
        svg->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
        svg->setZValue(12002.0);
        svg->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        svg->setAcceptedMouseButtons(Qt::NoButton);
        return svg;
    };
    
    // Create horizontal alignment buttons (fused group: left | center | right)
    m_alignLeftBtn = new SegmentedButtonItem(SegmentedButtonItem::Segment::Left, m_alignmentControlsBg);
    m_alignLeftBtn->setBrush(AppColors::gOverlayActiveBackgroundColor); // Default active
    m_alignLeftBtn->setZValue(12001.0);
    m_alignLeftBtn->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_alignLeftBtn->setAcceptedMouseButtons(Qt::NoButton);
    m_alignLeftBtn->setData(0, "overlay"); // Mark as overlay
    applySegmentBorder(m_alignLeftBtn);
    m_alignLeftIcon = makeSvg(":/icons/icons/text/horizontal-align-left.svg", m_alignLeftBtn);
    
    m_hDivider1 = makeDivider(m_alignmentControlsBg);
    
    m_alignCenterHBtn = new SegmentedButtonItem(SegmentedButtonItem::Segment::Middle, m_alignmentControlsBg);
    m_alignCenterHBtn->setBrush(AppColors::gOverlayBackgroundColor);
    m_alignCenterHBtn->setZValue(12001.0);
    m_alignCenterHBtn->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_alignCenterHBtn->setAcceptedMouseButtons(Qt::NoButton);
    m_alignCenterHBtn->setData(0, "overlay"); // Mark as overlay
    applySegmentBorder(m_alignCenterHBtn);
    m_alignCenterHIcon = makeSvg(":/icons/icons/text/horizontal-align-center.svg", m_alignCenterHBtn);
    
    m_hDivider2 = makeDivider(m_alignmentControlsBg);
    
    m_alignRightBtn = new SegmentedButtonItem(SegmentedButtonItem::Segment::Right, m_alignmentControlsBg);
    m_alignRightBtn->setBrush(AppColors::gOverlayBackgroundColor);
    m_alignRightBtn->setZValue(12001.0);
    m_alignRightBtn->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_alignRightBtn->setAcceptedMouseButtons(Qt::NoButton);
    m_alignRightBtn->setData(0, "overlay"); // Mark as overlay
    applySegmentBorder(m_alignRightBtn);
    m_alignRightIcon = makeSvg(":/icons/icons/text/horizontal-align-right.svg", m_alignRightBtn);
    
    // Create vertical alignment buttons (fused group: top | center | bottom)
    m_alignTopBtn = new SegmentedButtonItem(SegmentedButtonItem::Segment::Left, m_alignmentControlsBg);
    m_alignTopBtn->setBrush(AppColors::gOverlayActiveBackgroundColor); // Default active
    m_alignTopBtn->setZValue(12001.0);
    m_alignTopBtn->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_alignTopBtn->setAcceptedMouseButtons(Qt::NoButton);
    m_alignTopBtn->setData(0, "overlay"); // Mark as overlay
    applySegmentBorder(m_alignTopBtn);
    m_alignTopIcon = makeSvg(":/icons/icons/text/vertical-align-top.svg", m_alignTopBtn);
    
    m_vDivider1 = makeDivider(m_alignmentControlsBg);
    
    m_alignCenterVBtn = new SegmentedButtonItem(SegmentedButtonItem::Segment::Middle, m_alignmentControlsBg);
    m_alignCenterVBtn->setBrush(AppColors::gOverlayBackgroundColor);
    m_alignCenterVBtn->setZValue(12001.0);
    m_alignCenterVBtn->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_alignCenterVBtn->setAcceptedMouseButtons(Qt::NoButton);
    m_alignCenterVBtn->setData(0, "overlay"); // Mark as overlay
    applySegmentBorder(m_alignCenterVBtn);
    m_alignCenterVIcon = makeSvg(":/icons/icons/text/vertical-align-center.svg", m_alignCenterVBtn);
    
    m_vDivider2 = makeDivider(m_alignmentControlsBg);
    
    m_alignBottomBtn = new SegmentedButtonItem(SegmentedButtonItem::Segment::Right, m_alignmentControlsBg);
    m_alignBottomBtn->setBrush(AppColors::gOverlayBackgroundColor);
    m_alignBottomBtn->setZValue(12001.0);
    m_alignBottomBtn->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_alignBottomBtn->setAcceptedMouseButtons(Qt::NoButton);
    m_alignBottomBtn->setData(0, "overlay"); // Mark as overlay
    applySegmentBorder(m_alignBottomBtn);
    m_alignBottomIcon = makeSvg(":/icons/icons/text/vertical-align-bottom.svg", m_alignBottomBtn);
    
    // Initially hide controls (they'll show when item is selected)
    m_alignmentControlsBg->setVisible(false);
}

void TextMediaItem::updateAlignmentControlsLayout() {
    ensureAlignmentControls();
    if (!m_alignmentControlsBg || !scene()) return;
    
    // Ensure controls are added to the scene if they haven't been yet
    if (m_alignmentControlsBg->scene() != scene()) {
        scene()->addItem(m_alignmentControlsBg);
    }
    
    // Only show controls when this text item is selected
    const bool shouldShow = isSelected();
    m_alignmentControlsBg->setVisible(shouldShow);
    
    if (!shouldShow) return;
    
    // Get current view to access viewport transform
    QGraphicsView* view = nullptr;
    if (scene() && !scene()->views().isEmpty()) {
        view = scene()->views().first();
    }
    if (!view) return;
    
    // Button sizing (same as video controls)
    const int cornerRadiusPx = ResizableMediaBase::getCornerRadiusOfMediaOverlaysPx();
    int buttonSize = ResizableMediaBase::getHeightOfMediaOverlaysPx();
    if (buttonSize <= 0) buttonSize = 36;
    buttonSize = std::max(buttonSize, 24);
    
    const int gapPx = 8; // Gap between text item and controls
    const int buttonGap = 10; // Gap between button groups (matches top overlay spacing)
    
    // Icon size (60% of button size)
    int iconSize = static_cast<int>(std::round(buttonSize * 0.6));
    const int maxIcon = std::max(16, buttonSize - 4);
    iconSize = std::clamp(iconSize, 16, maxIcon);
    
    // Calculate total width: 3 buttons + gap + 3 buttons
    const int totalWidth = (buttonSize * 3) + buttonGap + (buttonSize * 3);
    const int totalHeight = buttonSize;
    
    // Get bottom center of text item in item coordinates
    const QSize textSize = baseSizePx();
    const qreal textWidth = static_cast<qreal>(textSize.width());
    const qreal textHeight = static_cast<qreal>(textSize.height());
    QPointF bottomCenterItem(textWidth / 2.0, textHeight);
    
    // Convert to scene coordinates
    QPointF bottomCenterScene = mapToScene(bottomCenterItem);
    
    // Convert to viewport coordinates
    QPointF bottomCenterView = view->viewportTransform().map(bottomCenterScene);
    
    // Position controls centered horizontally, with gap below the text item
    QPointF controlsTopLeftView = bottomCenterView + QPointF(-totalWidth / 2.0, gapPx);
    
    // Convert back to scene coordinates
    QPointF controlsTopLeftScene = view->viewportTransform().inverted().map(controlsTopLeftView);
    
    // Set background rect
    m_alignmentControlsBg->setRect(0, 0, totalWidth, totalHeight);
    m_alignmentControlsBg->setPos(controlsTopLeftScene);
    
    // Position horizontal alignment buttons (left group)
    int xOffset = 0;
    
    m_alignLeftBtn->setRect(0, 0, buttonSize, buttonSize);
    m_alignLeftBtn->setPos(xOffset, 0);
    m_alignLeftBtn->setRadius(cornerRadiusPx);
    qreal iconOffsetX = (buttonSize - iconSize) / 2.0;
    qreal iconOffsetY = (buttonSize - iconSize) / 2.0;
    m_alignLeftIcon->setPos(iconOffsetX, iconOffsetY);
    QRectF iconBounds = m_alignLeftIcon->boundingRect();
    if (iconBounds.width() > 0 && iconBounds.height() > 0) {
        qreal scaleX = iconSize / iconBounds.width();
        qreal scaleY = iconSize / iconBounds.height();
        qreal iconScale = std::min(scaleX, scaleY);
        m_alignLeftIcon->setScale(iconScale);
    }
    xOffset += buttonSize;
    
    m_alignCenterHBtn->setRect(0, 0, buttonSize, buttonSize);
    m_alignCenterHBtn->setPos(xOffset, 0);
    m_alignCenterHBtn->setRadius(cornerRadiusPx);
    m_alignCenterHIcon->setPos(iconOffsetX, iconOffsetY);
    iconBounds = m_alignCenterHIcon->boundingRect();
    if (iconBounds.width() > 0 && iconBounds.height() > 0) {
        qreal scaleX = iconSize / iconBounds.width();
        qreal scaleY = iconSize / iconBounds.height();
        qreal iconScale = std::min(scaleX, scaleY);
        m_alignCenterHIcon->setScale(iconScale);
    }
    xOffset += buttonSize;
    
    m_alignRightBtn->setRect(0, 0, buttonSize, buttonSize);
    m_alignRightBtn->setPos(xOffset, 0);
    m_alignRightBtn->setRadius(cornerRadiusPx);
    m_alignRightIcon->setPos(iconOffsetX, iconOffsetY);
    iconBounds = m_alignRightIcon->boundingRect();
    if (iconBounds.width() > 0 && iconBounds.height() > 0) {
        qreal scaleX = iconSize / iconBounds.width();
        qreal scaleY = iconSize / iconBounds.height();
        qreal iconScale = std::min(scaleX, scaleY);
        m_alignRightIcon->setScale(iconScale);
    }
    xOffset += buttonSize + buttonGap;
    
    // Position vertical alignment buttons (right group)
    m_alignTopBtn->setRect(0, 0, buttonSize, buttonSize);
    m_alignTopBtn->setPos(xOffset, 0);
    m_alignTopBtn->setRadius(cornerRadiusPx);
    m_alignTopIcon->setPos(iconOffsetX, iconOffsetY);
    iconBounds = m_alignTopIcon->boundingRect();
    if (iconBounds.width() > 0 && iconBounds.height() > 0) {
        qreal scaleX = iconSize / iconBounds.width();
        qreal scaleY = iconSize / iconBounds.height();
        qreal iconScale = std::min(scaleX, scaleY);
        m_alignTopIcon->setScale(iconScale);
    }
    xOffset += buttonSize;
    
    m_alignCenterVBtn->setRect(0, 0, buttonSize, buttonSize);
    m_alignCenterVBtn->setPos(xOffset, 0);
    m_alignCenterVBtn->setRadius(cornerRadiusPx);
    m_alignCenterVIcon->setPos(iconOffsetX, iconOffsetY);
    iconBounds = m_alignCenterVIcon->boundingRect();
    if (iconBounds.width() > 0 && iconBounds.height() > 0) {
        qreal scaleX = iconSize / iconBounds.width();
        qreal scaleY = iconSize / iconBounds.height();
        qreal iconScale = std::min(scaleX, scaleY);
        m_alignCenterVIcon->setScale(iconScale);
    }
    xOffset += buttonSize;
    
    m_alignBottomBtn->setRect(0, 0, buttonSize, buttonSize);
    m_alignBottomBtn->setPos(xOffset, 0);
    m_alignBottomBtn->setRadius(cornerRadiusPx);
    m_alignBottomIcon->setPos(iconOffsetX, iconOffsetY);
    iconBounds = m_alignBottomIcon->boundingRect();
    if (iconBounds.width() > 0 && iconBounds.height() > 0) {
        qreal scaleX = iconSize / iconBounds.width();
        qreal scaleY = iconSize / iconBounds.height();
        qreal iconScale = std::min(scaleX, scaleY);
        m_alignBottomIcon->setScale(iconScale);
    }
    
    // Calculate button rectangles in item coordinates using viewport transform approach
    // (same as video controls - convert pixel positions to item coordinates)
    QPointF ctrlTopLeftItem = mapFromScene(controlsTopLeftScene);
    
    const qreal buttonSizeItem = toItemLengthFromPixels(buttonSize);
    const qreal x0Item = toItemLengthFromPixels(0); // Left button
    const qreal x1Item = toItemLengthFromPixels(buttonSize); // Center H button
    const qreal x2Item = toItemLengthFromPixels(buttonSize * 2); // Right button
    const qreal x3Item = toItemLengthFromPixels((buttonSize * 3) + buttonGap); // Top button
    const qreal x4Item = toItemLengthFromPixels((buttonSize * 4) + buttonGap); // Center V button
    const qreal x5Item = toItemLengthFromPixels((buttonSize * 5) + buttonGap); // Bottom button
    
    m_alignLeftBtnRect = QRectF(ctrlTopLeftItem.x() + x0Item, ctrlTopLeftItem.y(), buttonSizeItem, buttonSizeItem);
    m_alignCenterHBtnRect = QRectF(ctrlTopLeftItem.x() + x1Item, ctrlTopLeftItem.y(), buttonSizeItem, buttonSizeItem);
    m_alignRightBtnRect = QRectF(ctrlTopLeftItem.x() + x2Item, ctrlTopLeftItem.y(), buttonSizeItem, buttonSizeItem);
    m_alignTopBtnRect = QRectF(ctrlTopLeftItem.x() + x3Item, ctrlTopLeftItem.y(), buttonSizeItem, buttonSizeItem);
    m_alignCenterVBtnRect = QRectF(ctrlTopLeftItem.x() + x4Item, ctrlTopLeftItem.y(), buttonSizeItem, buttonSizeItem);
    m_alignBottomBtnRect = QRectF(ctrlTopLeftItem.x() + x5Item, ctrlTopLeftItem.y(), buttonSizeItem, buttonSizeItem);
}

bool TextMediaItem::handleAlignmentControlsPressAtItemPos(const QPointF& itemPos) {
    if (!m_alignmentControlsBg || !m_alignmentControlsBg->isVisible()) return false;
    
    // Check horizontal alignment buttons
    if (m_alignLeftBtnRect.contains(itemPos)) {
        // Set left button active, others inactive
        m_alignLeftBtn->setBrush(AppColors::gOverlayActiveBackgroundColor);
        m_alignCenterHBtn->setBrush(AppColors::gOverlayBackgroundColor);
        m_alignRightBtn->setBrush(AppColors::gOverlayBackgroundColor);
        // TODO: Apply left alignment to text rendering
        return true;
    }
    if (m_alignCenterHBtnRect.contains(itemPos)) {
        // Set center button active, others inactive
        m_alignLeftBtn->setBrush(AppColors::gOverlayBackgroundColor);
        m_alignCenterHBtn->setBrush(AppColors::gOverlayActiveBackgroundColor);
        m_alignRightBtn->setBrush(AppColors::gOverlayBackgroundColor);
        // TODO: Apply center alignment to text rendering
        return true;
    }
    if (m_alignRightBtnRect.contains(itemPos)) {
        // Set right button active, others inactive
        m_alignLeftBtn->setBrush(AppColors::gOverlayBackgroundColor);
        m_alignCenterHBtn->setBrush(AppColors::gOverlayBackgroundColor);
        m_alignRightBtn->setBrush(AppColors::gOverlayActiveBackgroundColor);
        // TODO: Apply right alignment to text rendering
        return true;
    }
    
    // Check vertical alignment buttons
    if (m_alignTopBtnRect.contains(itemPos)) {
        // Set top button active, others inactive
        m_alignTopBtn->setBrush(AppColors::gOverlayActiveBackgroundColor);
        m_alignCenterVBtn->setBrush(AppColors::gOverlayBackgroundColor);
        m_alignBottomBtn->setBrush(AppColors::gOverlayBackgroundColor);
        // TODO: Apply top alignment to text rendering
        return true;
    }
    if (m_alignCenterVBtnRect.contains(itemPos)) {
        // Set center V button active, others inactive
        m_alignTopBtn->setBrush(AppColors::gOverlayBackgroundColor);
        m_alignCenterVBtn->setBrush(AppColors::gOverlayActiveBackgroundColor);
        m_alignBottomBtn->setBrush(AppColors::gOverlayBackgroundColor);
        // TODO: Apply center vertical alignment to text rendering
        return true;
    }
    if (m_alignBottomBtnRect.contains(itemPos)) {
        // Set bottom button active, others inactive
        m_alignTopBtn->setBrush(AppColors::gOverlayBackgroundColor);
        m_alignCenterVBtn->setBrush(AppColors::gOverlayBackgroundColor);
        m_alignBottomBtn->setBrush(AppColors::gOverlayActiveBackgroundColor);
        // TODO: Apply bottom alignment to text rendering
        return true;
    }
    
    return false;
}

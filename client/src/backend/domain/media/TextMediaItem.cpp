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
#include <QBrush>
#include <QClipboard>
#include <QApplication>
#include <QObject>
#include <QScopedValueRollback>
#include <QTimer>

// Global text styling configuration - tweak these to change all text media appearance
namespace TextMediaDefaults {
    static const QString FONT_FAMILY = QStringLiteral("Arial");
    static const int FONT_SIZE = 24;
    static const QFont::Weight FONT_WEIGHT = QFont::Bold;
    static const bool FONT_ITALIC = false;
    static const QColor TEXT_COLOR = Qt::white;
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
    }

protected:
    void focusOutEvent(QFocusEvent* event) override {
        QGraphicsTextItem::focusOutEvent(event);
        if (!m_owner) {
            return;
        }

        const Qt::FocusReason reason = event ? event->reason() : Qt::OtherFocusReason;
        if (reason == Qt::MouseFocusReason && QApplication::mouseButtons() != Qt::NoButton) {
            m_owner->commitInlineEditing();
            return;
        }

        if (!m_owner->isEditing()) {
            return;
        }

        QTimer::singleShot(0, this, [this]() {
            if (m_owner && m_owner->isEditing()) {
                this->setFocus(Qt::OtherFocusReason);
            }
        });
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (!m_owner) {
            QGraphicsTextItem::keyPressEvent(event);
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
            return;
        }

        // Intercept paste to strip formatting
        if (event->matches(QKeySequence::Paste)) {
            QClipboard* clipboard = QApplication::clipboard();
            if (clipboard) {
                QString plainText = clipboard->text();
                if (!plainText.isEmpty()) {
                    textCursor().insertText(plainText);
                    event->accept();
                    return;
                }
            }
        }

        QGraphicsTextItem::keyPressEvent(event);
        
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
        QStyleOptionGraphicsItem opt(*option);
        opt.state &= ~QStyle::State_HasFocus;
        QGraphicsTextItem::paint(painter, &opt, widget);
    }

private:
    TextMediaItem* m_owner = nullptr;
};

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

    // Create standard character format using the current font size (which may have been scaled)
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

    m_lastKnownScale = scale();
}

void TextMediaItem::setText(const QString& text) {
    if (m_text != text) {
        m_text = text;
        m_pendingAutoSize = true;
        m_documentMetricsDirty = true;
        m_cachedEditorPosValid = false;
        if (m_inlineEditor && !m_isEditing) {
            QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);
            m_inlineEditor->setPlainText(m_text);
            m_documentMetricsDirty = true;
            m_cachedEditorPosValid = false;
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
    }
}

void TextMediaItem::normalizeEditorFormatting() {
    if (!m_inlineEditor || !m_isEditing) {
        return;
    }
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

    {
        QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);
        m_inlineEditor->setPlainText(m_text);
    }
    m_documentMetricsDirty = true;
    m_cachedEditorPosValid = false;
    m_inlineEditor->setDefaultTextColor(m_textColor);
    m_inlineEditor->setEnabled(true);
    m_inlineEditor->setVisible(true);
    m_inlineEditor->setTextInteractionFlags(Qt::TextEditorInteraction);
    {
        QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);
        applyCenterAlignment(m_inlineEditor);
    }

    updateInlineEditorGeometry();

    m_inlineEditor->setFocus(Qt::OtherFocusReason);
    QTextCursor cursor = m_inlineEditor->textCursor();
    cursor.select(QTextCursor::Document);
    m_inlineEditor->setTextCursor(cursor);

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
        }

        QObject::connect(doc, &QTextDocument::contentsChanged, editor, [this]() {
            if (m_ignoreDocumentChange) {
                return;
            }
            m_documentMetricsDirty = true;
            m_cachedEditorPosValid = false;
            m_pendingAutoSize = true;
            if (m_isUpdatingInlineGeometry) {
                return;
            }
            updateInlineEditorGeometry();
        });
    }

    {
        QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);
        applyCenterAlignment(editor);
    }

    m_inlineEditor = editor;
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

    if (!m_inlineEditor->transform().isIdentity()) {
        m_inlineEditor->setTransform(QTransform());
    }

    if (m_inlineEditor->font() != m_font) {
        m_inlineEditor->setFont(m_font);
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

    const qreal contentWidth = std::max<qreal>(1.0, contentRect.width());
    bool widthChanged = false;
    if (m_cachedTextWidth < 0.0 || floatsDiffer(contentWidth, m_cachedTextWidth)) {
        m_inlineEditor->setTextWidth(contentWidth);
        m_cachedTextWidth = contentWidth;
        widthChanged = true;
        m_documentMetricsDirty = true;
    }

    QTextDocument* doc = m_inlineEditor->document();
    qreal docIdealWidth = m_cachedIdealWidth;
    qreal docHeight = std::max<qreal>(1.0, m_cachedDocumentSize.height());

    const bool needMetrics = doc && (allowAutoSize || widthChanged || m_documentMetricsDirty || m_cachedIdealWidth < 0.0);
    if (needMetrics) {
        docIdealWidth = doc ? doc->idealWidth() : contentWidth;
        QAbstractTextDocumentLayout* layout = doc ? doc->documentLayout() : nullptr;
        if (doc && layout) {
            QSizeF size = layout->documentSize();
            if (size.width() <= 0.0) {
                size.setWidth(contentWidth);
            }
            if (size.height() <= 0.0) {
                size.setHeight(contentRect.height());
            }
            m_cachedDocumentSize = size;
            docHeight = std::max<qreal>(1.0, size.height());
        } else {
            m_cachedDocumentSize = QSizeF(contentWidth, docHeight);
            docHeight = std::max<qreal>(1.0, docHeight);
        }
        m_cachedIdealWidth = docIdealWidth;
        m_documentMetricsDirty = false;
    } else {
        if (m_cachedIdealWidth < 0.0) {
            docIdealWidth = contentWidth;
        }
        docHeight = std::max<qreal>(1.0, m_cachedDocumentSize.height());
    }

    if (docIdealWidth <= 0.0) {
        docIdealWidth = contentWidth;
    }

    QSize newBaseSize = m_baseSize;
    if (doc && allowAutoSize) {
        const qreal requiredContentWidth = std::max(docIdealWidth, contentWidth);
        if (requiredContentWidth > contentWidth + 0.5) {
            const qreal requiredWidth = requiredContentWidth + margin * 2.0;
            newBaseSize.setWidth(static_cast<int>(std::ceil(requiredWidth)));
        }

        if (docHeight > contentRect.height() + 0.5) {
            const qreal requiredHeight = docHeight + margin * 2.0;
            newBaseSize.setHeight(static_cast<int>(std::ceil(requiredHeight)));
        }
    }

    const bool geometryChanged = (newBaseSize != m_baseSize);
    if (geometryChanged) {
        prepareGeometryChange();
        m_baseSize = newBaseSize;
        bounds = boundingRect();
        contentRect = bounds.adjusted(margin, margin, -margin, -margin);
        if (contentRect.width() < 1.0) {
            contentRect.setWidth(1.0);
        }
        if (contentRect.height() < 1.0) {
            contentRect.setHeight(1.0);
        }
    }

    const qreal finalTextWidth = std::max<qreal>(1.0, contentRect.width());
    bool finalWidthChanged = false;
    if (floatsDiffer(finalTextWidth, m_cachedTextWidth)) {
        m_inlineEditor->setTextWidth(finalTextWidth);
        m_cachedTextWidth = finalTextWidth;
        finalWidthChanged = true;
        m_documentMetricsDirty = true;
    }

    qreal finalDocWidth = m_cachedDocumentSize.width();
    qreal finalDocHeight = std::max<qreal>(1.0, m_cachedDocumentSize.height());

    if (doc && (finalWidthChanged || m_documentMetricsDirty)) {
        QAbstractTextDocumentLayout* layout = doc ? doc->documentLayout() : nullptr;
        if (layout) {
            QSizeF size = layout->documentSize();
            if (size.width() <= 0.0) {
                size.setWidth(finalTextWidth);
            }
            if (size.height() <= 0.0) {
                size.setHeight(contentRect.height());
            }
            m_cachedDocumentSize = size;
            finalDocWidth = size.width();
            finalDocHeight = std::max<qreal>(1.0, size.height());
        } else {
            m_cachedDocumentSize = QSizeF(finalTextWidth, finalDocHeight);
            finalDocWidth = finalTextWidth;
        }
        if (doc) {
            m_cachedIdealWidth = doc->idealWidth();
        }
        m_documentMetricsDirty = false;
    } else {
        if (finalDocWidth <= 0.0) {
            finalDocWidth = finalTextWidth;
        }
        if (finalDocHeight <= 0.0) {
            finalDocHeight = contentRect.height();
        }
    }

    const qreal offsetX = std::max<qreal>(0.0, (contentRect.width() - finalDocWidth) / 2.0);
    const qreal offsetY = std::max<qreal>(0.0, (contentRect.height() - finalDocHeight) / 2.0);
    const QPointF newEditorPos = contentRect.topLeft() + QPointF(offsetX, offsetY);
    if (!m_cachedEditorPosValid || floatsDiffer(newEditorPos.x(), m_cachedEditorPos.x(), 0.1) || floatsDiffer(newEditorPos.y(), m_cachedEditorPos.y(), 0.1)) {
        m_inlineEditor->setPos(newEditorPos);
        m_cachedEditorPos = newEditorPos;
        m_cachedEditorPosValid = true;
    }

    if (geometryChanged) {
        // Defer overlay and repaint updates to avoid cascading repaints during active painting
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

void TextMediaItem::finishInlineEditing(bool commitChanges) {
    if (!m_inlineEditor) {
        m_isEditing = false;
        return;
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

    if (commitChanges && editedText != m_text) {
        setText(editedText);
    }

    updateInlineEditorGeometry();
    update();
}

void TextMediaItem::applyFontScale(qreal factor) {
    if (factor <= 0.0 || std::abs(factor - 1.0) < 1e-4) {
        return;
    }

    QFont updatedFont = m_font;
    qreal pointSize = updatedFont.pointSizeF();
    if (pointSize <= 0.0) {
        pointSize = static_cast<qreal>(updatedFont.pointSize());
    }
    if (pointSize <= 0.0) {
        return;
    }

    updatedFont.setPointSizeF(pointSize * factor);
    m_font = updatedFont;
    m_documentMetricsDirty = true;
    m_cachedEditorPosValid = false;

    if (m_inlineEditor) {
        m_inlineEditor->setFont(m_font);
    }

    m_pendingAutoSize = true;
    updateInlineEditorGeometry();
    update();
}

void TextMediaItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
    Q_UNUSED(option);
    Q_UNUSED(widget);
    
    if (!painter) return;
    
    // Only update geometry when not editing to avoid triggering expensive layout operations during panning
    if (!m_isEditing) {
        updateInlineEditorGeometry();
    }

    if (m_isEditing) {
        if (m_inlineEditor) {
            m_inlineEditor->setOpacity(m_contentOpacity * m_contentDisplayOpacity);
        }
        paintSelectionAndLabel(painter);
        return;
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
    
    painter->setPen(m_textColor);
    painter->setFont(m_font);
    QRectF textRect = bounds.adjusted(kContentPadding, kContentPadding, -kContentPadding, -kContentPadding);
    painter->drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap, m_text);
    
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
    if (change == ItemScaleChange && value.canConvert<double>()) {
        const qreal newScale = value.toDouble();
        const qreal previousScale = m_lastKnownScale;
        const qreal scaleDiff = std::abs(newScale - previousScale);
        const qreal prevDiffFromOne = std::abs(previousScale - 1.0);
        const qreal newDiffFromOne = std::abs(newScale - 1.0);
        const qreal epsilon = 1e-4;
        if (scaleDiff > epsilon && newDiffFromOne < epsilon && prevDiffFromOne > epsilon && newScale > 0.0) {
            const qreal factor = previousScale / newScale;
            applyFontScale(factor);
        }
    }

    if (change == ItemSelectedHasChanged && value.canConvert<bool>()) {
        const bool selectedNow = value.toBool();
        if (!selectedNow && m_isEditing) {
            commitInlineEditing();
        }
    }

    QVariant result = ResizableMediaBase::itemChange(change, value);

    if (change == ItemScaleHasChanged && value.canConvert<double>()) {
        m_lastKnownScale = value.toDouble();
    } else if (change == ItemScaleHasChanged) {
        m_lastKnownScale = scale();
    }

    if (change == ItemTransformHasChanged ||
        change == ItemPositionHasChanged ||
        change == ItemSelectedChange ||
        change == ItemSelectedHasChanged) {
        updateInlineEditorGeometry();
    }

    return result;
}

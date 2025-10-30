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
        if (m_owner) {
            m_owner->commitInlineEditing();
        }
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
    QTextCursor cursor(doc);
    
    // Select all text
    cursor.select(QTextCursor::Document);
    
    // Create standard character format using the current font size (which may have been scaled)
    QTextCharFormat standardFormat;
    standardFormat.setFont(currentFont);
    standardFormat.setForeground(QBrush(currentColor));
    
    // Apply to all text
    cursor.mergeCharFormat(standardFormat);
    
    // Reset cursor to end
    cursor.clearSelection();
    cursor.movePosition(QTextCursor::End);
    editor->setTextCursor(cursor);
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

    m_lastKnownScale = scale();
}

void TextMediaItem::setText(const QString& text) {
    if (m_text != text) {
        m_text = text;
        m_pendingAutoSize = true;
        if (m_inlineEditor && !m_isEditing) {
            QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);
            m_inlineEditor->setPlainText(m_text);
        }
        updateInlineEditorGeometry();
        update(); // Trigger repaint
        updateOverlayLayout();
    }
}

void TextMediaItem::setFont(const QFont& font) {
    m_font = font;
    update();
    updateInlineEditorGeometry();
}

void TextMediaItem::setTextColor(const QColor& color) {
    m_textColor = color;
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

    m_isEditing = true;

    {
        QScopedValueRollback<bool> guard(m_ignoreDocumentChange, true);
        m_inlineEditor->setPlainText(m_text);
    }
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

    const bool allowAutoSize = m_pendingAutoSize;
    if (allowAutoSize) {
        m_pendingAutoSize = false;
    }
    const qreal margin = 10.0;

    QTextDocument* doc = m_inlineEditor->document();
    if (doc) {
        QScopedValueRollback<bool> ignoreGuard(m_ignoreDocumentChange, true);
        QTextOption opt = doc->defaultTextOption();
        opt.setWrapMode(QTextOption::WordWrap);
        opt.setAlignment(Qt::AlignHCenter);
        doc->setDefaultTextOption(opt);
        doc->setDocumentMargin(0.0);
    }

    const qreal currentContentWidth = std::max<qreal>(1.0, static_cast<qreal>(m_baseSize.width()) - (margin * 2.0));
    const qreal currentContentHeight = std::max<qreal>(1.0, static_cast<qreal>(m_baseSize.height()) - (margin * 2.0));
    m_inlineEditor->setTextWidth(currentContentWidth);

    qreal docIdealWidth = 0.0;
    qreal docHeight = 0.0;
    if (doc) {
        docIdealWidth = doc->idealWidth();
        if (auto* layout = doc->documentLayout()) {
            QSizeF size = layout->documentSize();
            docHeight = std::max<qreal>(1.0, size.height());
        }
    }

    QSize newBaseSize = m_baseSize;
    if (doc && allowAutoSize) {
        const qreal requiredContentWidth = docIdealWidth;
        if (requiredContentWidth > currentContentWidth + 0.5) {
            const qreal requiredWidth = requiredContentWidth + margin * 2.0;
            newBaseSize.setWidth(static_cast<int>(std::ceil(requiredWidth)));
        }

        if (docHeight > currentContentHeight + 0.5) {
            const qreal requiredHeight = docHeight + margin * 2.0;
            newBaseSize.setHeight(static_cast<int>(std::ceil(requiredHeight)));
        }
    }

    const bool geometryChanged = (newBaseSize != m_baseSize);
    if (geometryChanged) {
        prepareGeometryChange();
        m_baseSize = newBaseSize;
    }

    const QRectF bounds = boundingRect();
    QRectF contentRect = bounds.adjusted(margin, margin, -margin, -margin);
    if (contentRect.width() < 1.0) {
        contentRect.setWidth(1.0);
    }
    if (contentRect.height() < 1.0) {
        contentRect.setHeight(1.0);
    }

    m_inlineEditor->setDefaultTextColor(m_textColor);
    m_inlineEditor->setOpacity(m_contentOpacity * m_contentDisplayOpacity);

    QFont editorFont = m_font;
    m_inlineEditor->setFont(editorFont);
    m_inlineEditor->setTransform(QTransform());

    const qreal finalTextWidth = std::max<qreal>(1.0, contentRect.width());
    m_inlineEditor->setTextWidth(finalTextWidth);

    qreal finalDocWidth = finalTextWidth;
    qreal finalDocHeight = contentRect.height();
    if (doc) {
        if (auto* layout = doc->documentLayout()) {
            QSizeF size = layout->documentSize();
            finalDocWidth = std::max<qreal>(1.0, size.width());
            finalDocHeight = std::max<qreal>(1.0, size.height());
        }
    }

    const qreal offsetX = std::max<qreal>(0.0, (contentRect.width() - finalDocWidth) / 2.0);
    const qreal offsetY = std::max<qreal>(0.0, (contentRect.height() - finalDocHeight) / 2.0);

    m_inlineEditor->setPos(contentRect.topLeft() + QPointF(offsetX, offsetY));

    if (geometryChanged) {
        updateOverlayLayout();
        update();
    }

    {
        QScopedValueRollback<bool> ignoreGuard(m_ignoreDocumentChange, true);
        applyCenterAlignment(m_inlineEditor);
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

    m_isEditing = false;

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
    
    updateInlineEditorGeometry();

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
    QRectF textRect = bounds.adjusted(10.0, 10.0, -10.0, -10.0);
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

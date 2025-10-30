// TextMediaItem.cpp - Implementation of text media item
#include "TextMediaItem.h"
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QGraphicsSceneMouseEvent>
#include <QtGlobal>
#include <algorithm>
#include <QGraphicsTextItem>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QTransform>
#include <QTextDocument>
#include <QTextOption>
#include <QAbstractTextDocumentLayout>
#include <QTextCursor>
#include <QTextBlockFormat>
#include <QTextCursor>

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

        QGraphicsTextItem::keyPressEvent(event);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override {
        QStyleOptionGraphicsItem opt(*option);
        opt.state &= ~QStyle::State_HasFocus;
        QGraphicsTextItem::paint(painter, &opt, widget);
    }

private:
    TextMediaItem* m_owner = nullptr;
};

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
    , m_textColor(Qt::white) // Default white text
    , m_initialContentSize(initialSize)
{
    // Set up default font
    m_font = QFont(QStringLiteral("Arial"), 24);
    m_font.setBold(true);
    
    // Text media should be selectable and movable
    setFlag(QGraphicsItem::ItemIsSelectable, true);
    setFlag(QGraphicsItem::ItemIsMovable, true);
    setAcceptHoverEvents(true);
}

void TextMediaItem::setText(const QString& text) {
    if (m_text != text) {
        m_text = text;
        if (m_inlineEditor && !m_isEditing) {
            m_inlineEditor->setPlainText(m_text);
        }
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

    m_inlineEditor->setPlainText(m_text);
    m_inlineEditor->setDefaultTextColor(m_textColor);
    m_inlineEditor->setEnabled(true);
    m_inlineEditor->setVisible(true);
    m_inlineEditor->setTextInteractionFlags(Qt::TextEditorInteraction);
    applyCenterAlignment(m_inlineEditor);

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
        doc->setDocumentMargin(0.0);
    QTextOption opt = doc->defaultTextOption();
    opt.setWrapMode(QTextOption::WordWrap);
    opt.setAlignment(Qt::AlignHCenter);
    doc->setDefaultTextOption(opt);
    }

    applyCenterAlignment(editor);

    m_inlineEditor = editor;
}

void TextMediaItem::updateInlineEditorGeometry() {
    if (!m_inlineEditor) {
        return;
    }

    if (QTextDocument* doc = m_inlineEditor->document()) {
    QTextOption opt = doc->defaultTextOption();
    opt.setWrapMode(QTextOption::WordWrap);
        opt.setAlignment(Qt::AlignHCenter);
    doc->setDefaultTextOption(opt);
        doc->setDocumentMargin(0.0);
    }

    const QRectF bounds = boundingRect();
    const qreal margin = 10.0;
    const qreal effectiveOpacity = m_contentOpacity * m_contentDisplayOpacity;
    m_inlineEditor->setDefaultTextColor(m_textColor);
    m_inlineEditor->setOpacity(effectiveOpacity);

    if (m_fillContentWithoutAspect &&
        m_initialContentSize.width() > 0 &&
        m_initialContentSize.height() > 0) {
        const qreal widthRatio = bounds.width() / static_cast<qreal>(m_initialContentSize.width());
        const qreal heightRatio = bounds.height() / static_cast<qreal>(m_initialContentSize.height());
        const qreal baseMarginX = (widthRatio != 0.0) ? margin / widthRatio : margin;
        const qreal baseMarginY = (heightRatio != 0.0) ? margin / heightRatio : margin;
        const qreal editableWidthBase = std::max<qreal>(1.0, static_cast<qreal>(m_initialContentSize.width()) - (baseMarginX * 2.0));
        const qreal editableHeightBase = std::max<qreal>(1.0, static_cast<qreal>(m_initialContentSize.height()) - (baseMarginY * 2.0));

        QFont editorFont = m_font;
        editorFont.setPointSize(fontSizeForHeight(m_initialContentSize.height()));
        m_inlineEditor->setFont(editorFont);

        m_inlineEditor->setTransform(QTransform());
        m_inlineEditor->setTextWidth(editableWidthBase);
        if (QTextDocument* doc = m_inlineEditor->document()) {
            doc->adjustSize();
        }
        
        qreal docHeightBase = editableHeightBase;
        if (auto* layout = m_inlineEditor->document() ? m_inlineEditor->document()->documentLayout() : nullptr) {
            docHeightBase = std::max<qreal>(1.0, layout->documentSize().height());
        }
        
        qreal offsetBaseY = std::max<qreal>(0.0, (editableHeightBase - docHeightBase) / 2.0);

        QTransform transform;
        transform.scale(widthRatio, heightRatio);
        m_inlineEditor->setTransform(transform, false);

        QPointF basePos(baseMarginX, baseMarginY + offsetBaseY);
        QPointF finalPos(basePos.x() * widthRatio, basePos.y() * heightRatio);
        m_inlineEditor->setPos(finalPos);
    } else {
        QRectF contentRect = bounds.adjusted(margin, margin, -margin, -margin);
        if (contentRect.width() < 1.0) {
            contentRect.setWidth(1.0);
        }
        if (contentRect.height() < 1.0) {
            contentRect.setHeight(1.0);
        }

        QFont editorFont = m_font;
        editorFont.setPointSize(calculateFontSize());
        m_inlineEditor->setFont(editorFont);

        m_inlineEditor->setTransform(QTransform());
        m_inlineEditor->setTextWidth(std::max<qreal>(1.0, contentRect.width()));
        if (QTextDocument* doc = m_inlineEditor->document()) {
            doc->adjustSize();
        }
        
        qreal docWidth = contentRect.width();
        qreal docHeight = contentRect.height();
        if (auto* layout = m_inlineEditor->document() ? m_inlineEditor->document()->documentLayout() : nullptr) {
            QSizeF docSize = layout->documentSize();
            docWidth = std::max<qreal>(1.0, docSize.width());
            docHeight = std::max<qreal>(1.0, docSize.height());
        }
        
        qreal offsetX = std::max<qreal>(0.0, (contentRect.width() - docWidth) / 2.0);
        qreal offsetY = std::max<qreal>(0.0, (contentRect.height() - docHeight) / 2.0);

        m_inlineEditor->setPos(contentRect.topLeft() + QPointF(offsetX, offsetY));
    }

    applyCenterAlignment(m_inlineEditor);
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
    m_inlineEditor->setPlainText(m_text);

    m_isEditing = false;

    if (commitChanges && editedText != m_text) {
        setText(editedText);
    }

    updateInlineEditorGeometry();
    update();
}

int TextMediaItem::fontSizeForHeight(int pixelHeight) const {
    if (pixelHeight <= 0) {
        return 12;
    }
    return std::clamp(static_cast<int>(pixelHeight * 0.4), 12, 200);
}

int TextMediaItem::calculateFontSize() const {
    QRectF bounds = boundingRect();
    return fontSizeForHeight(static_cast<int>(bounds.height()));
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

    const bool allowNonUniformStretch = m_fillContentWithoutAspect &&
        m_initialContentSize.width() > 0 && m_initialContentSize.height() > 0;

    bool drewWithStretch = false;
    if (allowNonUniformStretch) {
        const qreal widthRatio = bounds.width() / static_cast<qreal>(m_initialContentSize.width());
        const qreal heightRatio = bounds.height() / static_cast<qreal>(m_initialContentSize.height());
        if (!qFuzzyIsNull(widthRatio) && !qFuzzyIsNull(heightRatio)) {
            QFont stretchedFont = m_font;
            stretchedFont.setPointSize(fontSizeForHeight(m_initialContentSize.height()));
            painter->save();
            painter->scale(widthRatio, heightRatio);
            painter->setFont(stretchedFont);

            const qreal horizontalMargin = (widthRatio != 0.0) ? 10.0 / widthRatio : 10.0;
            const qreal verticalMargin = (heightRatio != 0.0) ? 10.0 / heightRatio : 10.0;
            QRectF referenceRect(0.0, 0.0,
                                 static_cast<qreal>(m_initialContentSize.width()),
                                 static_cast<qreal>(m_initialContentSize.height()));
            QRectF textRect = referenceRect.adjusted(horizontalMargin, verticalMargin,
                                                     -horizontalMargin, -verticalMargin);
            painter->drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap, m_text);
            painter->restore();
            drewWithStretch = true;
        }
    }

    if (!drewWithStretch) {
        QFont renderFont = m_font;
        renderFont.setPointSize(calculateFontSize());
        painter->setFont(renderFont);
        QRectF textRect = bounds.adjusted(10, 10, -10, -10);
        painter->drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap, m_text);
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

    QVariant result = ResizableMediaBase::itemChange(change, value);

    if (change == ItemTransformHasChanged ||
        change == ItemPositionHasChanged ||
        change == ItemSelectedChange ||
        change == ItemSelectedHasChanged) {
        updateInlineEditorGeometry();
    }

    return result;
}

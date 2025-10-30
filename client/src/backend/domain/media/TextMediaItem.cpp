// TextMediaItem.cpp - Implementation of text media item
#include "TextMediaItem.h"
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QFontMetrics>
#include <algorithm>

TextMediaItem::TextMediaItem(
    const QSize& initialSize,
    int visualSizePx,
    int selectionSizePx,
    const QString& initialText
)
    : ResizableMediaBase(initialSize, visualSizePx, selectionSizePx, QStringLiteral("Text"))
    , m_text(initialText)
    , m_textColor(Qt::white) // Default white text
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
        update(); // Trigger repaint
    }
}

void TextMediaItem::setFont(const QFont& font) {
    m_font = font;
    update();
}

void TextMediaItem::setTextColor(const QColor& color) {
    m_textColor = color;
    update();
}

int TextMediaItem::calculateFontSize() const {
    // Calculate font size based on item height
    // Use approximately 60% of the item height for the font
    QRectF bounds = boundingRect();
    int height = static_cast<int>(bounds.height());
    
    // Minimum 12pt, maximum 200pt
    int fontSize = std::clamp(static_cast<int>(height * 0.4), 12, 200);
    return fontSize;
}

void TextMediaItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
    Q_UNUSED(option);
    Q_UNUSED(widget);
    
    if (!painter) return;
    
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
    
    // Set up font with dynamic size
    QFont renderFont = m_font;
    renderFont.setPointSize(calculateFontSize());
    painter->setFont(renderFont);
    
    // Set text color
    painter->setPen(m_textColor);
    
    // Draw text centered in the bounds
    QRectF textRect = bounds.adjusted(10, 10, -10, -10); // Add some padding
    painter->drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap, m_text);
    
    painter->restore();
    
    // Paint selection chrome and overlays (handles, buttons, etc.)
    paintSelectionAndLabel(painter);
}

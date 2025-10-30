// TextMediaItem.cpp - Implementation of text media item
#include "TextMediaItem.h"
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QFontMetrics>
#include <QtGlobal>
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

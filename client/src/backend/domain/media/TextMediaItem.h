// TextMediaItem.h - Text media item that can be positioned and resized on canvas
#pragma once

#include "MediaItems.h"
#include <QString>
#include <QFont>
#include <QColor>

// Text media item - displays editable text with same interaction as image/video media
class TextMediaItem : public ResizableMediaBase {
public:
    explicit TextMediaItem(
        const QSize& initialSize,
        int visualSizePx,
        int selectionSizePx,
        const QString& initialText = QStringLiteral("No Text")
    );
    
    ~TextMediaItem() override = default;

    // Text content accessors
    QString text() const { return m_text; }
    void setText(const QString& text);
    
    // Font/styling accessors (for future editing features)
    QFont font() const { return m_font; }
    void setFont(const QFont& font);
    
    QColor textColor() const { return m_textColor; }
    void setTextColor(const QColor& color);
    
    // QGraphicsItem override
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    
    // Override to indicate this is text media
    bool isTextMedia() const override { return true; }

private:
    QString m_text;
    QFont m_font;
    QColor m_textColor;
    
    // Helper to calculate appropriate font size based on item size
    int calculateFontSize() const;
};

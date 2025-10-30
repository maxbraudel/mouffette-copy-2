// TextMediaItem.h - Text media item that can be positioned and resized on canvas
#pragma once

#include "MediaItems.h"
#include <QString>
#include <QFont>
#include <QColor>

class QWidget;

class QGraphicsSceneMouseEvent;

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

    // Launches the text editing dialog. Returns true if the text was updated.
    bool promptTextEdit(QWidget* parentWidget = nullptr);
    
    // QGraphicsItem override
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;
    
    // Override to indicate this is text media
    bool isTextMedia() const override { return true; }

private:
    QString m_text;
    QFont m_font;
    QColor m_textColor;
    QSize m_initialContentSize;
    
    // Helper to calculate appropriate font size based on item size
    int calculateFontSize() const;
    int fontSizeForHeight(int pixelHeight) const;
};

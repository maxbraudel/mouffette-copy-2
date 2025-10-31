// TextMediaItem.h - Text media item that can be positioned and resized on canvas
#pragma once

#include "MediaItems.h"
#include <QString>
#include <QFont>
#include <QColor>
#include <QSizeF>
#include <QPointF>

class QGraphicsTextItem;

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

    // Inline editing lifecycle
    bool beginInlineEditing();
    void commitInlineEditing();
    void cancelInlineEditing();
    bool isEditing() const { return m_isEditing; }
    void normalizeEditorFormatting();
    
    // QGraphicsItem override
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;
    
    // Override to indicate this is text media
    bool isTextMedia() const override { return true; }

protected:
    void onInteractiveGeometryChanged() override;

private:
    QString m_text;
    QFont m_font;
    QColor m_textColor;
    QGraphicsTextItem* m_inlineEditor = nullptr;
    bool m_isEditing = false;
    QString m_textBeforeEditing;
    QString m_editorRenderingText;
    bool m_isUpdatingInlineGeometry = false;
    qreal m_lastKnownScale = 1.0;
    bool m_ignoreDocumentChange = false;
    bool m_pendingAutoSize = false;
    bool m_wasMovableBeforeEditing = false;
    bool m_documentMetricsDirty = true;
    qreal m_cachedTextWidth = -1.0;
    QSizeF m_cachedDocumentSize;
    qreal m_cachedIdealWidth = -1.0;
    QColor m_cachedTextColor;
    qreal m_cachedEditorOpacity = -1.0;
    QPointF m_cachedEditorPos;
    bool m_cachedEditorPosValid = false;
    
    // Rasterization cache for performance
    QImage m_rasterizedText;
    bool m_needsRasterization = true;
    QSize m_lastRasterizedSize;
    QImage m_scaledRasterizedText;
    qreal m_lastRasterizedScale = 1.0;
    bool m_scaledRasterDirty = true;
    
    void ensureInlineEditor();
    void updateInlineEditorGeometry();
    void finishInlineEditing(bool commitChanges);
    void rasterizeText();
    void ensureScaledRaster(qreal scaleFactor);
    void handleInlineEditorTextChanged(const QString& newText);
    const QString& textForRendering() const;
    void renderTextToImage(QImage& target, const QSize& imageSize, qreal scaleFactor);
};

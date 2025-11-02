// TextMediaItem.h - Text media item that can be positioned and resized on canvas
#pragma once

#include "MediaItems.h"
#include <QString>
#include <QFont>
#include <QColor>
#include <QSizeF>
#include <QPointF>

class QGraphicsTextItem;
class QGraphicsRectItem;
class QGraphicsSvgItem;
class QGraphicsSceneMouseEvent;
class RoundedRectItem;

// Global configuration namespace for text media defaults
namespace TextMediaDefaults {
    extern const QString FONT_FAMILY;
    extern const int FONT_SIZE;
    extern const QFont::Weight FONT_WEIGHT;
    extern const int FONT_WEIGHT_VALUE;
    extern const bool FONT_ITALIC;
    extern const QColor TEXT_COLOR;
    extern const qreal TEXT_BORDER_WIDTH_PERCENT;
    extern const QColor TEXT_BORDER_COLOR;
    
    // Default text content when creating new text media
    extern const QString DEFAULT_TEXT;
    
    // Default size when creating new text media (width x height in pixels)
    extern const int DEFAULT_WIDTH;
    extern const int DEFAULT_HEIGHT;
    
    // Default text scale
    extern const qreal DEFAULT_SCALE;
}

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

    qreal textBorderWidth() const { return m_textBorderWidthPercent; }
    void setTextBorderWidth(qreal percent);

    QColor textBorderColor() const { return m_textBorderColor; }
    void setTextBorderColor(const QColor& color);

    int textFontWeightValue() const { return m_fontWeightValue; }
    void setTextFontWeightValue(int weight);

    qreal uniformScaleFactor() const { return m_uniformScaleFactor; }
    
    // Text alignment
    enum class HorizontalAlignment { Left, Center, Right };
    enum class VerticalAlignment { Top, Center, Bottom };
    
    HorizontalAlignment horizontalAlignment() const { return m_horizontalAlignment; }
    void setHorizontalAlignment(HorizontalAlignment align);
    
    VerticalAlignment verticalAlignment() const { return m_verticalAlignment; }
    void setVerticalAlignment(VerticalAlignment align);

    bool fitToTextEnabled() const { return m_fitToTextEnabled; }
    void setFitToTextEnabled(bool enabled);

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
    
    // Alignment controls interaction (called by canvas, similar to video controls)
    bool handleAlignmentControlsPressAtItemPos(const QPointF& itemPos);
    void refreshAlignmentControlsLayout();
    void syncInlineEditorToBaseSize();

protected:
    void onAltResizeModeEngaged() override;
    void onInteractiveGeometryChanged() override;

private:
    void scheduleFitToTextUpdate();
    void applyFitToTextNow();
    void applyFontChange(const QFont& font);
    QString m_text;
    QFont m_font;
    QColor m_textColor;
    qreal borderStrokeWidthPx() const;

    qreal m_textBorderWidthPercent = 0.0;
    QColor m_textBorderColor;
    int m_fontWeightValue = TextMediaDefaults::FONT_WEIGHT_VALUE;
    QGraphicsTextItem* m_inlineEditor = nullptr;
    bool m_isEditing = false;
    QString m_textBeforeEditing;
    QString m_editorRenderingText;
    bool m_isUpdatingInlineGeometry = false;
    bool m_ignoreDocumentChange = false;
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
    qreal m_uniformScaleFactor = 1.0;
    qreal m_lastObservedScale = 1.0;
    
    // Text alignment settings
    HorizontalAlignment m_horizontalAlignment = HorizontalAlignment::Center;
    VerticalAlignment m_verticalAlignment = VerticalAlignment::Center;
    bool m_fitToTextEnabled = false;
    bool m_fitToTextUpdatePending = false;
    bool m_applyingFitToText = false;
    
    // Alignment controls (scene items, similar to video controls)
    class SegmentedButtonItem* m_fitToTextBtn = nullptr;
    class QGraphicsSvgItem* m_fitToTextIcon = nullptr;
    QGraphicsRectItem* m_alignmentControlsBg = nullptr;
    class SegmentedButtonItem* m_alignLeftBtn = nullptr;
    class SegmentedButtonItem* m_alignCenterHBtn = nullptr;
    class SegmentedButtonItem* m_alignRightBtn = nullptr;
    class SegmentedButtonItem* m_alignTopBtn = nullptr;
    class SegmentedButtonItem* m_alignCenterVBtn = nullptr;
    class SegmentedButtonItem* m_alignBottomBtn = nullptr;
    QGraphicsRectItem* m_hDivider1 = nullptr;  // Divider between left and center-h
    QGraphicsRectItem* m_hDivider2 = nullptr;  // Divider between center-h and right
    QGraphicsRectItem* m_vDivider1 = nullptr;  // Divider between top and center-v
    QGraphicsRectItem* m_vDivider2 = nullptr;  // Divider between center-v and bottom
    QGraphicsSvgItem* m_alignLeftIcon = nullptr;
    QGraphicsSvgItem* m_alignCenterHIcon = nullptr;
    QGraphicsSvgItem* m_alignRightIcon = nullptr;
    QGraphicsSvgItem* m_alignTopIcon = nullptr;
    QGraphicsSvgItem* m_alignCenterVIcon = nullptr;
    QGraphicsSvgItem* m_alignBottomIcon = nullptr;
    
    // Button hit test rectangles in item coordinates (similar to video controls)
    QRectF m_alignLeftBtnRect;
    QRectF m_alignCenterHBtnRect;
    QRectF m_alignRightBtnRect;
    QRectF m_alignTopBtnRect;
    QRectF m_alignCenterVBtnRect;
    QRectF m_alignBottomBtnRect;
    QRectF m_fitToTextBtnRect;
    
    void ensureInlineEditor();
    void updateInlineEditorGeometry();
    void finishInlineEditing(bool commitChanges);
    void rasterizeText();
    void ensureScaledRaster(qreal visualScaleFactor, qreal geometryScale);
    void handleInlineEditorTextChanged(const QString& newText);
    const QString& textForRendering() const;
    void renderTextToImage(QImage& target, const QSize& imageSize, qreal scaleFactor);
    void ensureAlignmentControls();
    void updateAlignmentControlsLayout();
    void updateAlignmentButtonStates();
    void applyAlignmentToEditor();
    void applyFitModeConstraintsToEditor();
};

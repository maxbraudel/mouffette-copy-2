// TextMediaItem.h - Text media item that can be positioned and resized on canvas
#pragma once

#include "MediaItems.h"
#include <QString>
#include <QFont>
#include <QColor>
#include <QSizeF>
#include <QPointF>
#include <QPainter>
#include <QImage>
#include <cmath>
#include <chrono>
#include <memory>
#include <optional>
#include <QPixmap>

class QGraphicsTextItem;
class QGraphicsSceneMouseEvent;
class RoundedRectItem;

// Global configuration namespace for text media defaults
namespace TextMediaDefaults {
    extern const QString FONT_FAMILY;
    extern const int FONT_SIZE;
    extern const QFont::Weight FONT_WEIGHT;
    extern const int FONT_WEIGHT_VALUE;
    extern const bool FONT_ITALIC;
    extern const bool FONT_UNDERLINE;
    extern const bool FONT_ALL_CAPS;
    extern const QColor TEXT_COLOR;
    extern const qreal TEXT_BORDER_WIDTH_PERCENT;
    extern const QColor TEXT_BORDER_COLOR;
    extern const bool TEXT_HIGHLIGHT_ENABLED;
    extern const QColor TEXT_HIGHLIGHT_COLOR;
    
    // Default text content when creating new text media
    extern const QString DEFAULT_TEXT;
    
    // Default size when creating new text media (width x height in pixels)
    extern const int DEFAULT_WIDTH;
    extern const int DEFAULT_HEIGHT;
    
    // Default text scale (base scale in viewport pixels, independent of canvas zoom)
    // This represents the desired text size in screen pixels, which will be adjusted
    // inversely proportional to canvas zoom to maintain constant visual size
    extern const qreal DEFAULT_VIEWPORT_SCALE;
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
    QString text() const { return m_isEditing ? m_editorRenderingText : m_text; }
    void setText(const QString& text);
    
    // Font/styling accessors (for future editing features)
    QFont font() const { return m_font; }
    void setFont(const QFont& font);
    
    QColor textColor() const { return m_textColor; }
    void setTextColor(const QColor& color);
    bool textColorOverrideEnabled() const { return m_textColorOverrideEnabled; }
    void setTextColorOverrideEnabled(bool enabled);

    qreal textBorderWidth() const { return m_textBorderWidthPercent; }
    void setTextBorderWidth(qreal percent);
    bool textBorderWidthOverrideEnabled() const { return m_textBorderWidthOverrideEnabled; }
    void setTextBorderWidthOverrideEnabled(bool enabled);

    QColor textBorderColor() const { return m_textBorderColor; }
    void setTextBorderColor(const QColor& color);
    bool textBorderColorOverrideEnabled() const { return m_textBorderColorOverrideEnabled; }
    void setTextBorderColorOverrideEnabled(bool enabled);

    bool highlightEnabled() const { return m_highlightEnabled; }
    void setHighlightEnabled(bool enabled);
    QColor highlightColor() const { return m_highlightColor; }
    void setHighlightColor(const QColor& color);

    int textFontWeightValue() const { return m_fontWeightValue; }
    void setTextFontWeightValue(int weight, bool markOverride = true);
    bool textFontWeightOverrideEnabled() const { return m_fontWeightOverrideEnabled; }
    void setTextFontWeightOverrideEnabled(bool enabled);

    bool italicEnabled() const { return m_italicEnabled; }
    void setItalicEnabled(bool enabled);
    bool underlineEnabled() const { return m_underlineEnabled; }
    void setUnderlineEnabled(bool enabled);
    bool uppercaseEnabled() const { return m_uppercaseEnabled; }
    void setUppercaseEnabled(bool enabled);

    qreal uniformScaleFactor() const { return m_uniformScaleFactor; }

    static void setMaxRasterDimension(int pixels);
    static int maxRasterDimension();
    
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
    bool m_italicEnabled = TextMediaDefaults::FONT_ITALIC;
    bool m_underlineEnabled = TextMediaDefaults::FONT_UNDERLINE;
    bool m_uppercaseEnabled = TextMediaDefaults::FONT_ALL_CAPS;
    QColor m_textColor;
    bool m_textColorOverrideEnabled = false;
    qreal borderStrokeWidthPx() const;

    qreal m_textBorderWidthPercent = 0.0;
    QColor m_textBorderColor;
    bool m_textBorderWidthOverrideEnabled = false;
    bool m_textBorderColorOverrideEnabled = false;
    bool m_highlightEnabled = false;
    QColor m_highlightColor;
    int m_fontWeightValue = TextMediaDefaults::FONT_WEIGHT_VALUE;
    bool m_fontWeightOverrideEnabled = false;
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
    qreal m_lastVisualScaleFactor = 1.0;
    qreal m_lastCanvasZoomForRaster = 1.0;
    std::chrono::steady_clock::time_point m_lastScaledRasterUpdate{};
    bool m_scaledRasterThrottleActive = false;
    bool m_baseRasterInProgress = false;
    quint64 m_baseRasterGeneration = 0;
    std::optional<QSize> m_pendingBaseRasterRequest;
    QSize m_activeBaseRasterSize;
    bool m_baseRasterDispatchQueued = false;
    
    // Async rasterization
    struct AsyncRasterRequest {
        QSize targetSize;
        qreal scale = 1.0;
        qreal canvasZoom = 1.0;
        quint64 requestId = 0;
        quint64 generation = 0;

        bool isEquivalentTo(const QSize& size, qreal scaleFactor, qreal zoom) const {
            constexpr qreal tolerance = 1e-4;
            return targetSize == size &&
                   std::abs(scale - scaleFactor) < tolerance &&
                   std::abs(canvasZoom - zoom) < tolerance;
        }
    };

    quint64 m_rasterRequestId = 0;
    quint64 m_pendingRasterRequestId = 0;
    bool m_asyncRasterInProgress = false;
    quint64 m_rasterJobGeneration = 0;
    std::optional<AsyncRasterRequest> m_activeAsyncRasterRequest;
    std::optional<AsyncRasterRequest> m_pendingAsyncRasterRequest;
    bool m_rasterDispatchQueued = false;

    QPixmap m_scaledRasterPixmap;
    bool m_scaledRasterPixmapValid = false;
    QRectF m_scaledRasterVisibleRegion;  // Region of item that was rasterized (for viewport optimization)
    
    // Viewport tracking for cache management (Étape 3)
    QRectF m_lastViewportRect;   // Last viewport rectangle calculated (item coords)
    qreal m_lastViewportScale = 1.0;  // Scale factor of last viewport calculation
    
    // Freeze zone fallback cache - low-res full text for out-of-viewport regions (Phase 1)
    QPixmap m_frozenFallbackPixmap;      // Full text at low res (background for non-viewport areas)
    bool m_frozenFallbackValid = false;  // Is fallback cache valid
    qreal m_frozenFallbackScale = 1.0;   // Scale at which fallback was created
    QSize m_frozenFallbackSize;          // Size of text when fallback was created (detect resize)
    
    // Text alignment settings
    HorizontalAlignment m_horizontalAlignment = HorizontalAlignment::Center;
    VerticalAlignment m_verticalAlignment = VerticalAlignment::Center;
    bool m_fitToTextEnabled = false;
    bool m_fitToTextUpdatePending = false;
    bool m_applyingFitToText = false;
    qreal m_appliedContentPaddingPx = 0.0;
    
    // Alignment controls (overlay panel with unified buttons)
    std::unique_ptr<OverlayPanel> m_alignmentPanel;
    std::shared_ptr<OverlayButtonElement> m_fitToTextBtn;
    std::shared_ptr<OverlayButtonElement> m_alignLeftBtn;
    std::shared_ptr<OverlayButtonElement> m_alignCenterHBtn;
    std::shared_ptr<OverlayButtonElement> m_alignRightBtn;
    std::shared_ptr<OverlayButtonElement> m_alignTopBtn;
    std::shared_ptr<OverlayButtonElement> m_alignCenterVBtn;
    std::shared_ptr<OverlayButtonElement> m_alignBottomBtn;
    
    void ensureInlineEditor();
    void updateInlineEditorGeometry();
    void finishInlineEditing(bool commitChanges);
    void rasterizeText();
    struct VectorDrawSnapshot {
        QString text;
        QFont font;
        QColor fillColor;
        QColor outlineColor;
        qreal outlineWidthPercent = 0.0;
        bool highlightEnabled = false;
        QColor highlightColor;
        qreal contentPaddingPx = 0.0;
        bool fitToTextEnabled = false;
        HorizontalAlignment horizontalAlignment = HorizontalAlignment::Center;
        VerticalAlignment verticalAlignment = VerticalAlignment::Center;
    };

    struct TextRasterJob {
        VectorDrawSnapshot snapshot;
        QSize targetSize;
        qreal scaleFactor = 1.0;
        QRectF targetRect;  // Region to rasterize in item coordinates (empty = full raster)

        QImage execute() const;
    };

    VectorDrawSnapshot captureVectorSnapshot() const;
    static void paintVectorSnapshot(QPainter* painter, const VectorDrawSnapshot& snapshot, const QSize& targetSize, qreal scaleFactor);

    QRectF computeVisibleRegion() const;
    void ensureScaledRaster(qreal visualScaleFactor, qreal geometryScale, qreal canvasZoom);
    void ensureFrozenFallbackCache(qreal currentCanvasZoom);  // Phase 1: Create/update low-res fallback cache
    void startRasterJob(const QSize& targetSize, qreal visualScaleFactor, qreal canvasZoom, quint64 requestId);
    void handleRasterJobFinished(quint64 generation, QImage&& raster, const QSize& size, qreal scale, qreal canvasZoom, const QRectF& visibleRegion = QRectF());
    void startAsyncRasterRequest(const QSize& targetSize, qreal visualScaleFactor, qreal canvasZoom, quint64 requestId);
    void startNextPendingAsyncRasterRequest();
    void queueRasterJobDispatch();
    void dispatchPendingRasterRequest();
    void startBaseRasterRequest(const QSize& targetSize);
    void handleBaseRasterJobFinished(quint64 generation, QImage&& raster, const QSize& size);
    void queueBaseRasterDispatch();
    void dispatchPendingBaseRasterRequest();
    void startNextPendingBaseRasterRequest();
    void handleInlineEditorTextChanged(const QString& newText);
    const QString& textForRendering() const;
    void renderTextToImage(QImage& target, const QSize& imageSize, qreal scaleFactor, const QRectF& visibleRegion = QRectF());
    void ensureAlignmentControls();
    void updateAlignmentControlsLayout();
    void updateAlignmentButtonStates();
    void applyAlignmentToEditor();
    void applyFitModeConstraintsToEditor();
    qreal contentPaddingPx() const;
    void handleContentPaddingChanged(qreal oldPadding, qreal newPadding);

    static int s_maxRasterDimension;
};

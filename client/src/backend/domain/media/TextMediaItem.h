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
#include <QPixmap>
#include <QVector>
#include <memory>

class QGraphicsTextItem;
class QGraphicsSceneMouseEvent;
class RoundedRectItem;
class QAbstractTextDocumentLayout;
class QAbstractTextDocumentLayout;
class QRawFont;

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
    enum class StrokeRenderMode {
        Normal
    };


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
    bool allowAltResize() const override { return true; }
    
    // Alignment controls interaction (called by canvas, similar to video controls)
    void refreshAlignmentControlsLayout();
    void syncInlineEditorToBaseSize();

protected:
    void onInteractiveGeometryChanged() override;
    void onOverlayLayoutUpdated() override;
    bool onAltResizeModeEngaged() override;

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

    enum class InvalidationReason {
        Content,
        Geometry,
        VisualStyle,
        Zoom
    };

    struct TextRenderTarget {
        qreal visualScaleFactor = 1.0;
        qreal geometryScale = 1.0;
        qreal canvasZoom = 1.0;

        bool isEquivalentTo(const TextRenderTarget& other, qreal tolerance = 1e-4) const {
            return std::abs(visualScaleFactor - other.visualScaleFactor) < tolerance &&
                   std::abs(geometryScale - other.geometryScale) < tolerance &&
                   std::abs(canvasZoom - other.canvasZoom) < tolerance;
        }
    };

    struct TextRenderScheduler {
        bool dirty = true;
        InvalidationReason reason = InvalidationReason::Content;
        std::optional<TextRenderTarget> highResTarget;
        bool highResPending = false;

        void invalidate(InvalidationReason newReason) {
            dirty = true;
            reason = newReason;
            highResTarget.reset();
            highResPending = false;
        }

        void requestHighRes(const TextRenderTarget& target) {
            highResTarget = target;
            highResPending = true;
        }

        std::optional<TextRenderTarget> takePresentTarget() {
            if (highResPending && highResTarget.has_value()) {
                return *highResTarget;
            }

            return std::nullopt;
        }

        void markPresented() {
            highResPending = false;
            dirty = false;
        }

        bool hasPendingPresentation() const {
            return highResPending && highResTarget.has_value();
        }
    };

    struct TextRenderCacheKey {
        quint64 textHash = 0;
        quint64 styleHash = 0;
        int wrapWidthPx = 0;
        int targetWidthPx = 0;
        int targetHeightPx = 0;
        int scalePermille = 1000;
        int dprPermille = 1000;
        bool fitToText = false;

        bool operator==(const TextRenderCacheKey& other) const {
            return textHash == other.textHash &&
                   styleHash == other.styleHash &&
                   wrapWidthPx == other.wrapWidthPx &&
                   targetWidthPx == other.targetWidthPx &&
                   targetHeightPx == other.targetHeightPx &&
                   scalePermille == other.scalePermille &&
                   dprPermille == other.dprPermille &&
                   fitToText == other.fitToText;
        }
    };

    struct TextLayoutSnapshot {
        quint64 key = 0;
        qreal wrapWidth = 0.0;
        QSizeF docSize;
        qreal idealWidth = 0.0;
        int lineCount = 0;
        bool valid = false;
    };

    struct TextRasterJob;

    struct ITextRenderer {
        virtual ~ITextRenderer() = default;
        virtual QImage render(const TextRasterJob& job) const = 0;
        virtual const char* backendName() const = 0;
    };

    TextRenderScheduler m_renderScheduler;
    TextLayoutSnapshot m_layoutSnapshot;
    TextRenderCacheKey m_activeHighResCacheKey;
    bool m_activeHighResCacheKeyValid = false;
    static constexpr qint64 kPerItemCacheBudgetBytes = 24LL * 1024LL * 1024LL;
    std::shared_ptr<ITextRenderer> m_textRenderer;
    QString m_textRendererBackend;
    
    // Rasterization cache for performance
    QImage m_rasterizedText;
    bool m_needsRasterization = true;
    QSize m_lastRasterizedSize;
    QImage m_scaledRasterizedText;
    qreal m_lastRasterizedScale = 1.0;
    bool m_scaledRasterDirty = true;
    QSize m_lastScaledTargetSize;
    QSize m_lastScaledBaseSize;
    bool m_forceScaledRasterRefresh = false;
    qreal m_uniformScaleFactor = 1.0;
    qreal m_lastObservedScale = 1.0;
    bool m_pendingUniformScaleBake = false;
    qreal m_pendingUniformScaleAmount = 1.0;
    bool m_customScaleBaking = false;  // Set during Alt-resize to prevent itemChange interference
    qreal m_lastVisualScaleFactor = 1.0;
    qreal m_lastCanvasZoomForRaster = 1.0;
    std::chrono::steady_clock::time_point m_lastScaledRasterUpdate{};
    bool m_scaledRasterThrottleActive = false;
    bool m_baseRasterInProgress = false;
    quint64 m_baseRasterGeneration = 0;
    quint64 m_contentRevision = 1;
    std::optional<QSize> m_pendingBaseRasterRequest;
    QSize m_activeBaseRasterSize;
    bool m_baseRasterDispatchQueued = false;
    quint64 m_rasterSupersessionToken = 1;
    
    // Async rasterization
    struct AsyncRasterRequest {
        QSize targetSize;
        QSize baseSizeAtRequest;
        QRectF visibleRegionAtRequest;
        qreal scale = 1.0;
        qreal canvasZoom = 1.0;
        quint64 contentRevision = 0;
        quint64 requestId = 0;
        quint64 generation = 0;
        quint64 supersessionToken = 0;
        std::chrono::steady_clock::time_point startedAt{};

        bool isEquivalentTo(const QSize& size,
                            qreal scaleFactor,
                            qreal zoom,
                            const QRectF& visibleRegion,
                            quint64 revision,
                            quint64 token) const {
            constexpr qreal tolerance = 1e-4;
            constexpr qreal rectTolerance = 0.5;
            const auto rectsCloseEnough = [&](const QRectF& lhs, const QRectF& rhs) {
                if (lhs.isEmpty() && rhs.isEmpty()) {
                    return true;
                }
                if (lhs.isValid() != rhs.isValid()) {
                    return false;
                }
                return std::abs(lhs.x() - rhs.x()) <= rectTolerance &&
                       std::abs(lhs.y() - rhs.y()) <= rectTolerance &&
                       std::abs(lhs.width() - rhs.width()) <= rectTolerance &&
                       std::abs(lhs.height() - rhs.height()) <= rectTolerance;
            };
            return targetSize == size &&
                   std::abs(scale - scaleFactor) < tolerance &&
                   std::abs(canvasZoom - zoom) < tolerance &&
                   rectsCloseEnough(visibleRegionAtRequest, visibleRegion) &&
                   contentRevision == revision &&
                   supersessionToken == token;
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
    quint64 m_scaledRasterContentRevision = 0;
    QRectF m_scaledRasterVisibleRegion;  // Region of item that was rasterized (for viewport optimization)
    enum class RenderState {
        Idle,
        HighResPending,
        HighResReady
    };
    RenderState m_renderState = RenderState::Idle;
    bool m_scaledRasterUpdateQueued = false;
    qreal m_queuedVisualScaleFactor = 1.0;
    qreal m_queuedGeometryScale = 1.0;
    qreal m_queuedCanvasZoom = 1.0;
    
    // Viewport tracking for cache management (Étape 3)
    QRectF m_lastViewportRect;   // Last viewport rectangle calculated (item coords)
    qreal m_lastViewportScale = 1.0;  // Scale factor of last viewport calculation
    
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // TIER 2 OPTIMIZATION STRUCTURES
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    
    // Phase 6: Glyph Layout Pre-computation Cache
    // Note: Simplified to avoid QRawFont storage complexity
    // Actual Phase 6 optimization deferred - cache infrastructure ready but not used
    struct GlyphLayoutCache {
        quint64 fingerprint = 0;           // Hash of text + font + size + width
        bool valid = false;
        
        void clear() {
            fingerprint = 0;
            valid = false;
        }
    };
    GlyphLayoutCache m_glyphLayoutCache;
    
    // Phase 7: Per-Block Viewport Culling
    struct BlockVisibility {
        int blockIndex = -1;
        QRectF blockBounds;                // Block bounds in item coordinates
        bool fullyVisible = false;         // Entire block inside viewport
        bool fullyInvisible = false;       // Entire block outside viewport
        bool partiallyVisible = false;     // Block intersects viewport edge
        QRectF visibleIntersection;        // Visible portion if partially visible
    };
    QVector<BlockVisibility> m_blockVisibilityCache;
    QRectF m_lastCullingViewport;          // Viewport used for last cull calculation
    bool m_blockVisibilityCacheValid = false;
    
    // Phase 8: Stroke Quality LOD (Level-of-Detail)
    enum class StrokeQuality {
        Full,        // Per-glyph precision (zoom >= 20%)
        Medium,      // Per-word grouping (zoom 5-20%)
        Low          // Unified text outline (zoom < 5%)
    };
    StrokeQuality m_lastStrokeQuality = StrokeQuality::Full;
    qreal m_lastStrokeQualityZoom = 1.0;
    
    quint64 m_baseRasterContentRevision = 0;

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // TIER 3 OPTIMIZATION STRUCTURES
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    
    // Phase 3: Texture Atlas for Glyph Batching
    struct AtlasGlyphEntry {
        QRect atlasRect;        // UV rectangle in atlas texture
        int atlasSheetIndex;    // Which atlas sheet (0-3)
        QPointF offset;         // Rendering offset from glyph origin
    };
    
    struct TextureAtlas {
        QImage sheet;           // 2048x2048 ARGB32 texture
        int nextX = 0;          // Next shelf position
        int nextY = 0;          // Current shelf Y position
        int rowHeight = 0;      // Height of current shelf row
        bool isFull = false;
        
        TextureAtlas() : sheet(2048, 2048, QImage::Format_ARGB32_Premultiplied) {
            sheet.fill(Qt::transparent);
        }
        
        // Shelf packing: try to insert glyph, return rect or null
        std::optional<QRect> tryInsert(const QSize& glyphSize) {
            const int padding = 2; // Prevent texture bleeding
            const int requiredWidth = glyphSize.width() + padding * 2;
            const int requiredHeight = glyphSize.height() + padding * 2;
            
            // Try current shelf
            if (nextX + requiredWidth <= 2048 && nextY + requiredHeight <= 2048) {
                QRect rect(nextX + padding, nextY + padding, glyphSize.width(), glyphSize.height());
                nextX += requiredWidth;
                rowHeight = std::max(rowHeight, requiredHeight);
                return rect;
            }
            
            // Move to next shelf
            if (nextY + rowHeight + requiredHeight <= 2048) {
                nextY += rowHeight;
                nextX = 0;
                rowHeight = 0;
                return tryInsert(glyphSize); // Retry on new shelf
            }
            
            // Atlas full
            isFull = true;
            return std::nullopt;
        }
    };
    
    QVector<TextureAtlas> m_glyphAtlases;  // Dynamic pool of atlas sheets
    QHash<quint64, AtlasGlyphEntry> m_atlasGlyphMap; // renderedGlyphKey -> atlas location
    
    // Phase 4: Text Diff for Incremental Updates
    struct TextEditDiff {
        int changeStartIdx = -1;
        int changeEndIdx = -1;
        int insertedCount = 0;
        int deletedCount = 0;
        bool geometryChanged = false;  // Line wrap or alignment affected
        
        bool hasChanges() const {
            return changeStartIdx >= 0 || geometryChanged;
        }
    };
    
    QString m_previousText;  // For diff computation
    QFont m_previousFont;
    int m_previousTextWidth = -1;
    
    // Phase 5: Partial Raster Compositing
    QImage m_baseRaster;           // Full text at last stable state
    QImage m_dirtyOverlay;         // Only changed glyphs
    QRect m_dirtyBounds;           // Bounding box of changes in image coords
    bool m_incrementalRenderValid = false;
    bool m_baseRasterValid = false;

    // Text alignment settings
    HorizontalAlignment m_horizontalAlignment = HorizontalAlignment::Center;
    VerticalAlignment m_verticalAlignment = VerticalAlignment::Center;
    bool m_fitToTextEnabled = false;
    bool m_fitToTextUpdatePending = false;
    bool m_applyingFitToText = false;
    std::optional<QSize> m_pendingGeometryCommitSize;
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
    QString m_alignmentPanelStyleSignature;
    
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
        qreal uniformScaleFactor = 1.0;
    };

    struct TextRasterJob {
        VectorDrawSnapshot snapshot;
        QSize targetSize;
        qreal scaleFactor = 1.0;
        qreal canvasZoom = 1.0;  // Tier 2: Canvas zoom for Phase 8 LOD selection
        QRectF targetRect;  // Region to rasterize in item coordinates (empty = full raster)

        QImage execute() const;
    };

    VectorDrawSnapshot captureVectorSnapshot(StrokeRenderMode mode = StrokeRenderMode::Normal) const;
    static void paintVectorSnapshot(QPainter* painter, const VectorDrawSnapshot& snapshot, const QSize& targetSize, qreal scaleFactor, qreal canvasZoom = 1.0, const QRectF& viewport = QRectF());
    void invalidateRenderPipeline(InvalidationReason reason, bool invalidateLayout = false);
    TextRenderCacheKey makeCacheKey(const VectorDrawSnapshot& snapshot, const QSize& targetSize, qreal scaleFactor, qreal dpr) const;
    bool cacheKeyMatches(const TextRenderCacheKey& lhs, const TextRenderCacheKey& rhs) const;
    void enforceCacheBudget();
    quint64 computeLayoutSnapshotKey(const QString& text, const QFont& font, qreal wrapWidth) const;
    bool canReuseLayoutSnapshot(const QString& text, const QFont& font, qreal wrapWidth) const;
    void updateLayoutSnapshot(const QString& text, const QFont& font, qreal wrapWidth, const QSizeF& docSize, qreal idealWidth, int lineCount);
    void ensureTextRenderer();
    QImage renderRasterJobWithBackend(const TextRasterJob& job) const;

    QRectF computeVisibleRegion() const;
    void ensureScaledRaster(qreal visualScaleFactor, qreal geometryScale, qreal canvasZoom);
    void startRasterJob(const QSize& targetSize, qreal visualScaleFactor, qreal canvasZoom, const QRectF& visibleRegion, quint64 requestId);
    void handleRasterJobFinished(quint64 generation, QImage&& raster, const QSize& size, qreal scale, qreal canvasZoom, const QRectF& visibleRegion = QRectF());
    void startAsyncRasterRequest(const QSize& targetSize, qreal visualScaleFactor, qreal canvasZoom, const QRectF& visibleRegion, quint64 requestId);
    void startNextPendingAsyncRasterRequest();
    void queueRasterJobDispatch();
    void dispatchPendingRasterRequest();
    void startBaseRasterRequest(const QSize& targetSize);
    void handleBaseRasterJobFinished(quint64 generation, quint64 contentRevision, QImage&& raster, const QSize& size);
    void queueBaseRasterDispatch();
    void dispatchPendingBaseRasterRequest();
    void startNextPendingBaseRasterRequest();
    void handleInlineEditorTextChanged(const QString& newText);
    const QString& textForRendering() const;
    void renderTextToImage(QImage& target, const QSize& imageSize, qreal scaleFactor, const QRectF& visibleRegion = QRectF(), StrokeRenderMode mode = StrokeRenderMode::Normal);
    void queueScaledRasterUpdate(qreal visualScaleFactor, qreal geometryScale, qreal canvasZoom);
    void dispatchQueuedScaledRasterUpdate();
    void ensureAlignmentControls();
    void updateAlignmentControlsLayout();
    void updateAlignmentButtonStates();
    void applyAlignmentToEditor();
    void applyFitModeConstraintsToEditor();
    QPointF anchorPointForAlignment(const QSize& size) const;
    void commitBaseSizeKeepingAnchor(const QSize& newBase);
    qreal contentPaddingPx() const;
    void handleContentPaddingChanged(qreal oldPadding, qreal newPadding);
    
    
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // TIER 2 OPTIMIZATION METHODS
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    
    // Phase 6: Glyph Layout Pre-computation
    quint64 computeLayoutFingerprint(const QString& text, const QFont& font, qreal width) const;
    void updateGlyphLayoutCache(const VectorDrawSnapshot& snapshot, const QSize& targetSize);
    bool isGlyphLayoutCacheValid(const VectorDrawSnapshot& snapshot, const QSize& targetSize) const;
    
    // Phase 7: Viewport Culling
    void updateBlockVisibilityCache(const QRectF& viewport, QTextDocument& doc, QAbstractTextDocumentLayout* layout);
    bool shouldSkipBlock(int blockIndex) const;
    
    // Phase 8: Stroke Quality LOD
    StrokeQuality selectStrokeQuality(qreal canvasZoom) const;
    void paintSimplifiedStroke(QPainter* painter, const VectorDrawSnapshot& snapshot, const QSize& targetSize, qreal scaleFactor, qreal canvasZoom);

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // TIER 3 OPTIMIZATION METHODS
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    
    // Phase 3: Texture Atlas Management
    quint64 getAtlasKey(const QPixmap& glyph, const QColor& fill, const QColor& stroke, qreal strokeWidth, qreal scale) const;
    bool insertGlyphIntoAtlas(quint64 key, const QPixmap& glyph);
    std::optional<AtlasGlyphEntry> getGlyphFromAtlas(quint64 key) const;
    void clearAtlases();
    
    // Phase 4: Text Diff Algorithm
    TextEditDiff computeTextDiff(const QString& newText, const QFont& newFont, int newWidth) const;
    void applyTextDiff(const TextEditDiff& diff);
    
    // Phase 5: Partial Compositing
    void initializeBaseRaster(const QSize& size);
    void renderDirtyRegion(const TextEditDiff& diff, const VectorDrawSnapshot& snapshot, const QSize& targetSize, qreal scaleFactor);
    QImage compositeRasterLayers() const;
    void flattenCompositeToBase();
    bool shouldUseIncrementalRender(const TextEditDiff& diff) const;

    static int s_maxRasterDimension;
};


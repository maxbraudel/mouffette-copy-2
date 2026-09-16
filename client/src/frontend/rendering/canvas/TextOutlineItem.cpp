#include "frontend/rendering/canvas/TextOutlineItem.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QGlyphRun>
#include <QHash>
#include <QPainter>
#include <QPointer>
#include <QRawFont>
#include <QQuickWindow>
#include <QSGTransformNode>
#include <QSGImageNode>
#include <QSet>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <QtGui/private/qtextengine_p.h>
#include <QtQuick/private/qquicktextedit_p_p.h>
#include <cmath>
#include <algorithm>
#include <atomic>

namespace {
constexpr int glyphsPerChunk = 64;

struct GlyphMesh {
    QImage mask;
    QRectF bounds;
    QPainterPath path;
    QColor maskColor = Qt::white;
    qreal rasterScale = 1;
};
using Mesh = std::shared_ptr<const GlyphMesh>;

Mesh rasterizeGlyph(const QPainterPath& path, qreal width, qreal rasterScale,
                    const QColor& color)
{
    auto mesh = std::make_shared<GlyphMesh>();
    mesh->path = path;
    mesh->maskColor = color;
    mesh->rasterScale = rasterScale;
    if (!path.isEmpty()) {
        const QRectF ink = path.boundingRect().adjusted(-width, -width, width, width);
        // Guard extreme font sizes without allocating unbounded images.
        const qreal scale = qMin(rasterScale,
            2044.0 / qMax(qreal(1), qMax(ink.width(), ink.height())));
        const QRect pixels = QRectF(ink.topLeft() * scale,
            ink.size() * scale).toAlignedRect().adjusted(-2, -2, 2, 2);
        mesh->bounds = QRectF(pixels.topLeft() / scale, QSizeF(pixels.size()) / scale);
        mesh->mask = QImage(pixels.size(), QImage::Format_ARGB32_Premultiplied);
        if (mesh->mask.isNull())
            return {};
        mesh->mask.fill(Qt::transparent);
        QPainter painter(&mesh->mask);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(scale, scale);
        painter.translate(-mesh->bounds.topLeft());
        painter.strokePath(path, QPen(color, width * 2, Qt::SolidLine,
                                     Qt::RoundCap, Qt::RoundJoin));
    }
    return mesh;
}

struct RefinementGlyph {
    Mesh previous;
    QPainterPath path;
};
struct RefinementResult {
    QHash<const GlyphMesh*, Mesh> replacements;
    qint64 nanoseconds = 0;
    bool complete = false;
};

QThreadPool* outlineQualityPool()
{
    // This is quality work for existing content, never an input/render task.
    // One shared lane bounds CPU/memory pressure across all open canvases.
    static QPointer<QThreadPool> pool;
    if (!pool) {
        pool = new QThreadPool(QCoreApplication::instance());
        pool->setMaxThreadCount(1);
    }
    return pool;
}
struct CachedGlyph {
    Mesh glyph;
    quint64 lastUse = 0;
};

struct PlacedGlyph {
    Mesh mesh;
    QPointF position;
    bool operator==(const PlacedGlyph& other) const
    { return mesh == other.mesh && position == other.position; }
};

bool sameGlyphSequence(const QList<PlacedGlyph>& left,
                       const QList<PlacedGlyph>& right)
{
    if (left.size() != right.size())
        return false;
    for (qsizetype i = 0; i < left.size(); ++i) {
        if (left[i].mesh != right[i].mesh)
            return false;
    }
    return true;
}

struct Chunk {
    QList<PlacedGlyph> glyphs;
    QPointF origin;
    size_t key = 0;
};

struct ChunkNode : QSGTransformNode {
    QList<PlacedGlyph> glyphs;
    QList<QSGImageNode*> images;
    size_t key = 0;
};
struct GlyphTexture {
    Mesh glyph;
    QSGTexture* texture = nullptr;
};
struct OutlineNode : QSGTransformNode {
    QList<ChunkNode*> chunks;
    QHash<const GlyphMesh*, GlyphTexture> textures;
    QColor color;
    ~OutlineNode() override
    {
        // Image nodes borrow textures; release consumers first, all on the
        // scene-graph thread. No GUI-thread font object lives in this cache.
        while (firstChild())
            delete firstChild();
        for (const auto& entry : std::as_const(textures))
            delete entry.texture;
    }
};
}

struct TextOutlineItem::Private {
    QPointer<QQuickTextEdit> source;
    QList<QMetaObject::Connection> connections;
    QList<QMetaObject::Connection> windowConnections;
    qreal width = 0;
    QColor color = Qt::black;
    QRectF renderedRect;
    QSize renderedPixelSize;
    bool layoutDirty = true;
    bool viewportDirty = true;
    bool rasterUpdatesDeferred = false;
    bool refinementRequested = false;
    bool refinementInFlight = false;
    bool refinementFailed = false;
    QTimer refinementTimer;
    quint64 refinementGeneration = 0;
    std::shared_ptr<std::atomic_bool> refinementCancelled;
    qreal rasterScale = 1;
    // GUI-thread-only font objects. Only immutable, font-free masks cross
    // into updatePaintNode while the GUI thread is blocked by Qt's sync phase.
    QHash<QRawFont, QHash<quint32, CachedGlyph>> cache;
    qint64 cacheBytes = 0;
    quint64 cacheEpoch = 0;
    QList<Chunk> chunks;
    Statistics stats;

    void invalidateRefinement()
    {
        ++refinementGeneration;
        refinementFailed = false;
        if (refinementCancelled)
            refinementCancelled->store(true, std::memory_order_relaxed);
    }

    void clearCache()
    {
        invalidateRefinement();
        cache.clear();
        cacheBytes = 0;
    }

    void trimCache()
    {
        if (cacheBytes > maskCacheBudgetBytes) {
            struct Candidate { QRawFont font; quint32 index; quint64 age; };
            QList<Candidate> candidates;
            for (auto font = cache.cbegin(); font != cache.cend(); ++font) {
                for (auto glyph = font->cbegin(); glyph != font->cend(); ++glyph) {
                    if (glyph->lastUse != cacheEpoch)
                        candidates.append({font.key(), glyph.key(), glyph->lastUse});
                }
            }
            std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                return a.age < b.age;
            });
            for (const auto& candidate : candidates) {
                if (cacheBytes <= maskCacheBudgetBytes)
                    break;
                auto font = cache.find(candidate.font);
                auto glyph = font->find(candidate.index);
                cacheBytes -= glyph->glyph->mask.sizeInBytes();
                font->erase(glyph);
                if (font->isEmpty())
                    cache.erase(font);
            }
        }
        // The current visible working set is never evicted, even if it alone
        // exceeds the history budget; otherwise every frame would regenerate it.
        stats.cachedMaskBytes = cacheBytes;
    }

    Mesh glyphMesh(const QRawFont& font, quint32 index, qreal scale)
    {
        auto& glyphs = cache[font];
        auto it = glyphs.find(index);
        if (it != glyphs.end()) {
            it->lastUse = cacheEpoch;
            return it->glyph;
        }
        const Mesh mesh = rasterizeGlyph(font.pathForGlyph(index), width, scale,
                                         Qt::white);
        if (!mesh)
            return std::make_shared<GlyphMesh>();
        ++stats.generatedGlyphs;
        cacheBytes += mesh->mask.sizeInBytes();
        glyphs.insert(index, {mesh, cacheEpoch});
        return mesh;
    }
};

TextOutlineItem::TextOutlineItem(QQuickItem* parent)
    : QQuickItem(parent), d(std::make_unique<Private>())
{
    d->refinementTimer.setSingleShot(true);
    d->refinementTimer.setInterval(120);
    connect(&d->refinementTimer, &QTimer::timeout, this, [this] {
        if (d->refinementRequested && !d->rasterUpdatesDeferred) {
            d->viewportDirty = true;
            polish();
        }
    });
    setFlag(ItemHasContents, false);
    setVisible(false);
    // A disabled outline must be completely dormant. ItemObservesViewport
    // propagates every ancestor camera transform to itemChange(); keeping it on
    // at zero width made every borderless TextItem polish and synchronize an
    // empty scene-graph node on every pan frame.
    setFlag(ItemObservesViewport, false);
    if (window())
        itemChange(ItemSceneChange, ItemChangeData(window()));
}

TextOutlineItem::~TextOutlineItem() { d->invalidateRefinement(); }
QQuickItem* TextOutlineItem::source() const { return d->source; }
qreal TextOutlineItem::outlinePixels() const { return d->width; }
QColor TextOutlineItem::color() const { return d->color; }
bool TextOutlineItem::rasterUpdatesDeferred() const { return d->rasterUpdatesDeferred; }
QRectF TextOutlineItem::renderedRect() const { return d->renderedRect; }
QSize TextOutlineItem::renderedPixelSize() const { return d->renderedPixelSize; }
TextOutlineItem::Statistics TextOutlineItem::statistics() const { return d->stats; }
bool TextOutlineItem::qualityRefinementPending() const
{ return (d->refinementRequested && !d->refinementFailed) || d->refinementInFlight; }

void TextOutlineItem::scheduleLayout()
{
    // Invalidate at notification time, before any already-queued worker result
    // can run ahead of the next polish and install obsolete content.
    d->invalidateRefinement();
    // Text/source geometry may keep changing during Alt-resize even though no
    // border exists. Once the old node is gone, none of those signals require
    // polish or a render-thread update until a positive width is enabled.
    if ((!d->source || d->width <= 0) && d->chunks.isEmpty())
        return;
    d->layoutDirty = true;
    polish();
    update();
}

void TextOutlineItem::scheduleViewport()
{
    d->invalidateRefinement();
    if (!d->source || d->width <= 0)
        return;
    if (d->refinementRequested && !d->rasterUpdatesDeferred)
        d->refinementTimer.start();
    d->viewportDirty = true;
    polish();
    // The inherited scene-graph transform already moves the existing quads.
    // Only updatePolish can decide whether new visible content is needed.
}

void TextOutlineItem::setSource(QQuickItem* item)
{
    auto* edit = qobject_cast<QQuickTextEdit*>(item);
    if (d->source == edit)
        return;
    for (const auto& connection : std::as_const(d->connections))
        disconnect(connection);
    d->connections.clear();
    d->clearCache();
    d->source = edit;
    const bool renderable = d->source && d->width > 0;
    setFlag(ItemHasContents, renderable);
    setFlag(ItemObservesViewport, renderable);
    setVisible(renderable);
    if (edit) {
        const auto watch = [this, edit](auto signal) {
            d->connections.append(connect(edit, signal, this, &TextOutlineItem::scheduleLayout));
        };
        watch(&QQuickTextEdit::textChanged);
        watch(&QQuickTextEdit::preeditTextChanged);
        watch(&QQuickTextEdit::contentSizeChanged);
        d->connections.append(connect(edit, &QQuickTextEdit::fontChanged, this, [this] {
            d->clearCache();
            scheduleLayout();
        }));
        watch(&QQuickTextEdit::effectiveHorizontalAlignmentChanged);
        watch(&QQuickTextEdit::verticalAlignmentChanged);
        watch(&QQuickTextEdit::wrapModeChanged);
        watch(&QQuickTextEdit::topPaddingChanged);
        watch(&QQuickTextEdit::bottomPaddingChanged);
        watch(&QQuickTextEdit::leftPaddingChanged);
        watch(&QQuickTextEdit::rightPaddingChanged);
        watch(&QQuickItem::widthChanged);
        watch(&QQuickItem::heightChanged);
        watch(&QQuickItem::xChanged);
        watch(&QQuickItem::yChanged);
        watch(&QQuickItem::parentChanged);
        d->connections.append(connect(edit, &QObject::destroyed, this, [this] {
            d->clearCache();
            setFlag(ItemHasContents, false);
            setFlag(ItemObservesViewport, false);
            setVisible(false);
            scheduleLayout();
            emit sourceChanged();
        }));
        auto* doc = QQuickTextEditPrivate::get(edit)->document;
        d->connections.append(connect(doc->documentLayout(), &QAbstractTextDocumentLayout::update,
                                      this, &TextOutlineItem::scheduleLayout));
    }
    scheduleLayout();
    emit sourceChanged();
}

void TextOutlineItem::setOutlinePixels(qreal width)
{
    width = std::isfinite(width) ? qMax(qreal(0), width) : 0;
    if (d->width == width)
        return;
    d->width = width;
    const bool renderable = d->source && d->width > 0;
    setFlag(ItemHasContents, renderable);
    setFlag(ItemObservesViewport, renderable);
    setVisible(renderable);
    d->clearCache();
    scheduleLayout();
    emit outlinePixelsChanged();
}

void TextOutlineItem::setColor(const QColor& color)
{
    if (d->color == color)
        return;
    d->invalidateRefinement();
    d->color = color;
    if (d->refinementRequested)
        scheduleViewport();
    update();
    emit colorChanged();
}

void TextOutlineItem::setRasterUpdatesDeferred(bool deferred)
{
    if (d->rasterUpdatesDeferred == deferred)
        return;
    d->invalidateRefinement();
    d->rasterUpdatesDeferred = deferred;
    // Uniform resizing is a scene-graph transform. Keep its existing masks
    // throughout the gesture. The next polish requests background refinement;
    // neither release nor a quick following swipe must pay the mask cost.
    // Document edits/newly visible glyphs keep the normal layout/culling path.
    d->refinementRequested = !deferred && d->source && d->width > 0;
    if (!deferred) {
        scheduleViewport();
        // An explicit resize release already tells us the gesture ended.
        // Subsequent viewport motion will defer/cancel this work again.
        d->refinementTimer.stop();
    }
    emit rasterUpdatesDeferredChanged();
}

void TextOutlineItem::startQualityRefinement(qreal rasterScale)
{
    if (!d->refinementRequested || d->rasterUpdatesDeferred || d->refinementInFlight
        || d->refinementFailed || d->refinementTimer.isActive())
        return;
    QList<RefinementGlyph> glyphs;
    QSet<const GlyphMesh*> seen;
    for (const Chunk& chunk : std::as_const(d->chunks)) {
        for (const PlacedGlyph& glyph : chunk.glyphs) {
            if (seen.contains(glyph.mesh.get()))
                continue;
            seen.insert(glyph.mesh.get());
            // Own the contour value on the worker, including its lazy bounds
            // caches. No QRawFont, QTextDocument or QObject crosses threads.
            QPainterPath path;
            path.setFillRule(glyph.mesh->path.fillRule());
            path.addPath(glyph.mesh->path);
            glyphs.append({glyph.mesh, std::move(path)});
        }
    }
    if (glyphs.isEmpty()) {
        d->refinementRequested = false;
        return;
    }
    const quint64 generation = d->refinementGeneration;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    d->refinementCancelled = cancelled;
    d->refinementInFlight = true;
    ++d->stats.refinementJobsStarted;
    auto* watcher = new QFutureWatcher<RefinementResult>(this);
    connect(watcher, &QFutureWatcher<RefinementResult>::finished, this,
        [this, watcher, generation, cancelled, rasterScale] {
        const RefinementResult result = watcher->result();
        watcher->deleteLater();
        d->refinementInFlight = false;
        d->refinementCancelled.reset();
        if (generation != d->refinementGeneration
            || cancelled->load(std::memory_order_relaxed)
            || d->rasterUpdatesDeferred || !d->refinementRequested) {
            ++d->stats.refinementJobsDiscarded;
            // There is at most one queued/running job for this item. Changes
            // coalesce while it exits, then this requests only the latest state.
            if (d->refinementRequested && !d->rasterUpdatesDeferred)
                scheduleViewport();
            return;
        }
        if (!result.complete) {
            // Keep usable old masks after an allocation failure, without an
            // immediate retry loop or a synchronous fallback on the GUI. A
            // later content/viewport change may request another background try.
            d->refinementFailed = true;
            return;
        }
        QElapsedTimer applyTimer;
        applyTimer.start();
        d->refinementRequested = false;
        d->rasterScale = rasterScale;
        d->cacheBytes = 0;
        // Historical masks have the previous density; retain just the current
        // working set, preserving the font/index keys exclusively on the GUI.
        for (auto font = d->cache.begin(); font != d->cache.end();) {
            for (auto glyph = font->begin(); glyph != font->end();) {
                const auto replacement = result.replacements.constFind(glyph->glyph.get());
                if (replacement == result.replacements.cend()) {
                    glyph = font->erase(glyph);
                } else {
                    glyph->glyph = *replacement;
                    d->cacheBytes += glyph->glyph->mask.sizeInBytes();
                    ++glyph;
                }
            }
            if (font->isEmpty())
                font = d->cache.erase(font);
            else
                ++font;
        }
        for (Chunk& chunk : d->chunks) {
            chunk.key = 0;
            for (PlacedGlyph& glyph : chunk.glyphs) {
                glyph.mesh = result.replacements.value(glyph.mesh.get());
                Q_ASSERT(glyph.mesh);
                chunk.key = qHashMulti(chunk.key, quintptr(glyph.mesh.get()));
            }
        }
        d->stats.cachedMaskBytes = d->cacheBytes;
        ++d->stats.refinementJobsApplied;
        d->stats.refinementNanoseconds = result.nanoseconds;
        const auto extent = [rasterScale](qreal size) {
            return int(qBound(qreal(1), std::ceil(size * rasterScale), qreal(4096)));
        };
        const QSize pixelSize(extent(d->renderedRect.width()), extent(d->renderedRect.height()));
        if (d->renderedPixelSize != pixelSize) {
            d->renderedPixelSize = pixelSize;
            emit viewportChanged();
        }
        d->stats.refinementApplyNanoseconds = applyTimer.nsecsElapsed();
        update();
    });
    const qreal width = d->width;
    // Opaque RGB can be baked without losing coverage. Keep white coverage for
    // direct C++ users supplying translucent colors, so a later SourceIn color
    // change can still recover full opacity instead of multiplying old alpha.
    const QColor color = d->color.alpha() == 255 ? d->color : QColor(Qt::white);
    watcher->setFuture(QtConcurrent::run(outlineQualityPool(),
        [glyphs = std::move(glyphs), cancelled, rasterScale, width, color] {
        RefinementResult result;
        QElapsedTimer timer;
        timer.start();
        for (const RefinementGlyph& glyph : glyphs) {
            if (cancelled->load(std::memory_order_relaxed))
                return result;
            const Mesh replacement = rasterizeGlyph(glyph.path, width, rasterScale, color);
            if (!replacement)
                return result;
            result.replacements.insert(glyph.previous.get(), replacement);
        }
        result.nanoseconds = timer.nsecsElapsed();
        result.complete = !cancelled->load(std::memory_order_relaxed);
        return result;
    }));
}

void TextOutlineItem::geometryChange(const QRectF& now, const QRectF& before)
{
    QQuickItem::geometryChange(now, before);
    // Moving this adapter relative to its source changes the document offset.
    // Actual canvas drag/pan moves their common ancestor, handled separately.
    if (d->source && d->width > 0)
        scheduleLayout();
}

void TextOutlineItem::itemChange(ItemChange change, const ItemChangeData& data)
{
    QQuickItem::itemChange(change, data);
    if (change == ItemSceneChange) {
        for (const auto& connection : std::as_const(d->windowConnections))
            disconnect(connection);
        d->windowConnections.clear();
        if (data.window) {
            d->windowConnections.append(connect(data.window, &QQuickWindow::widthChanged,
                                               this, &TextOutlineItem::scheduleViewport));
            d->windowConnections.append(connect(data.window, &QQuickWindow::heightChanged,
                                               this, &TextOutlineItem::scheduleViewport));
        }
        scheduleLayout();
    } else if (change == ItemParentHasChanged) {
        scheduleLayout();
    } else if (change == ItemTransformHasChanged || change == ItemDevicePixelRatioHasChanged) {
        // ItemObservesViewport propagates ancestor pan/zoom changes here. Qt
        // can still deliver a queued transform notification just after the
        // flag is disabled, so also gate the callback on live renderability.
        if (d->source && d->width > 0)
            scheduleViewport();
    }
}

void TextOutlineItem::updatePolish()
{
    if (!d->layoutDirty && !d->viewportDirty)
        return;
    QElapsedTimer timer;
    timer.start();
    bool contentChanged = d->layoutDirty;
    d->layoutDirty = d->viewportDirty = false;
    d->stats.generatedGlyphs = d->stats.rebuiltChunks = d->stats.movedChunks = 0;
    d->stats.layoutPasses = d->stats.uploadedGlyphs = 0;
    d->stats.polishNanoseconds = d->stats.syncNanoseconds = 0;
    if (!d->source || d->width <= 0) {
        d->refinementRequested = false;
        const bool hadSceneGraphContent = !d->chunks.isEmpty();
        d->chunks.clear();
        d->stats = {};
        // One update is required when disabling an existing border so Qt can
        // release its old node. An already empty renderer stays render-thread
        // dormant instead of synchronizing on every camera transform.
        if (hadSceneGraphContent)
            update();
        return;
    }
    QRectF visibleBounds = clipRect();
    if (flags().testFlag(ItemObservesViewport) && window()) {
        // An intermediate clipped item need not itself observe the viewport.
        // Intersect the active Qt Quick window as well.
        visibleBounds &= mapRectFromItem(window()->contentItem(),
                                        window()->contentItem()->boundingRect());
    }
    // Qt 6.11 ShaderEffectSource rounds a fractional destination rectangle
    // outward without matching its source crop (QTBUG-149373). Use one aligned
    // rectangle for BOTH sides so fractional pan/zoom cannot stretch the mask.
    if (!visibleBounds.isEmpty())
        visibleBounds = visibleBounds.toAlignedRect();
    // A translucent border needs a single alpha mask, but a document-sized
    // Item layer can exceed the GPU texture limit. Expose a viewport crop and
    // screen-resolution texture size for an explicitly positioned Qt source.
    const QPointF sceneOrigin = mapToScene(QPointF());
    const QPointF sceneX = mapToScene(QPointF(1, 0)) - sceneOrigin;
    const QPointF sceneY = mapToScene(QPointF(0, 1)) - sceneOrigin;
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1;
    const qreal density = qMax(std::hypot(sceneX.x(), sceneX.y()),
                              std::hypot(sceneY.x(), sceneY.y())) * dpr;
    // Upgrade before magnification, but downgrade only below half resolution.
    // Hysteresis also prevents tiny floating-point translation errors at exact
    // zoom powers from repeatedly invalidating every glyph in the cache.
    const qreal wantedScale = qMax(qreal(0.0625), density);
    const bool needsDensityChange = wantedScale > d->rasterScale * (1 + 1e-5)
        || wantedScale < d->rasterScale * 0.5;
    const qreal refinementScale = std::exp2(std::ceil(std::log2(wantedScale) * 2 - 1e-5) / 2);
    bool visibleGlyphsNeedRefinement = false;
    if (d->refinementRequested && !needsDensityChange) {
        for (const Chunk& chunk : std::as_const(d->chunks)) {
            for (const PlacedGlyph& glyph : chunk.glyphs) {
                if (glyph.mesh->rasterScale < wantedScale * (1 - 1e-5)) {
                    visibleGlyphsNeedRefinement = true;
                    break;
                }
            }
            if (visibleGlyphsNeedRefinement)
                break;
        }
    }
    if (d->refinementRequested && !needsDensityChange && !visibleGlyphsNeedRefinement) {
        d->invalidateRefinement();
        d->refinementRequested = false;
        d->refinementTimer.stop();
    }
    if (!d->rasterUpdatesDeferred && !d->refinementRequested && needsDensityChange) {
        if (d->cache.isEmpty()) {
            d->rasterScale = refinementScale;
            contentChanged = true;
        } else {
            // Camera zoom, fitting and inherited transforms need the same
            // retained-mask path as Alt-resize. Density is quality, not text
            // content: changing it must never clear/rasterize the cache in
            // polish. Coalesce successive transforms until motion settles.
            d->refinementRequested = true;
            d->refinementTimer.start();
        }
    }
    // Keep a small screen-space guard around the visible area. Panning or
    // dragging within it needs neither document access nor node rebuilding,
    // and the optional translucent mask retains an identical source rectangle.
    const QRectF itemBounds = boundingRect().toAlignedRect();
    const QRectF previousBounds = d->renderedRect;
    if (!visibleBounds.isEmpty()) {
        const qreal guard = 96 * dpr / qMax(qreal(0.0625), density);
        const QRectF retentionBounds = visibleBounds.adjusted(-2 * guard, -2 * guard,
                                                              2 * guard, 2 * guard);
        if (previousBounds.contains(visibleBounds) && itemBounds.contains(previousBounds)
            && retentionBounds.contains(previousBounds)) {
            visibleBounds = previousBounds;
        } else {
            visibleBounds = visibleBounds.adjusted(-guard, -guard, guard, guard)
                .toAlignedRect().intersected(itemBounds.toRect());
        }
    }
    // During zoom-out, existing glyph masks may still be very dense. The
    // translucent composition target only needs the current screen density;
    // using the old mask density would repeatedly allocate 4096-square layers.
    const qreal textureScale = qMin(d->rasterScale, refinementScale);
    const auto textureExtent = [textureScale](qreal extent) {
        return int(qBound(qreal(1), std::ceil(extent * textureScale), qreal(4096)));
    };
    const QSize pixelSize(textureExtent(visibleBounds.width()), textureExtent(visibleBounds.height()));
    if (d->renderedRect != visibleBounds || d->renderedPixelSize != pixelSize) {
        d->renderedRect = visibleBounds;
        d->renderedPixelSize = pixelSize;
        emit viewportChanged();
    }
    if (!contentChanged && previousBounds == visibleBounds) {
        startQualityRefinement(refinementScale);
        d->stats.polishNanoseconds = timer.nsecsElapsed();
        return;
    }
    const auto previous = std::move(d->chunks);
    d->chunks = {};
    d->stats.glyphs = d->stats.chunks = 0;
    d->stats.triangles = 0;
    d->stats.layoutPasses = 1;
    ++d->cacheEpoch;
    auto* edit = d->source.data();
    edit->ensurePolished();
    auto* text = QQuickTextEditPrivate::get(edit);
    auto* doc = text->document;
    auto* layout = doc->documentLayout();
    layout->documentSize(); // Finish any pending QTextDocument layout.
    // Read Qt's actual offset, including padding and RTL alignment. Recreating
    // its alignment formulas was the cause of fill/border drift in the SVG path.
    const QPointF offset = edit->mapToItem(this, QPointF(text->xoff, text->yoff));
    QList<PlacedGlyph> placed;
    // Panning/zooming out can reveal uncached letters even within the current
    // density bucket. Give these a cheap preview instead of rasterizing at the
    // previous extreme zoom. Visible preview masks are refined after motion.
    const qreal newGlyphScale = !contentChanged || d->refinementRequested || d->rasterUpdatesDeferred
        ? std::min({qreal(1), d->rasterScale, refinementScale}) : d->rasterScale;

    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        if (!block.isVisible() || !block.layout())
            continue;
        const QPointF blockOffset = offset + layout->blockBoundingRect(block).topLeft();
        const QRectF blockBounds = block.layout()->boundingRect().translated(blockOffset)
            .adjusted(-d->width, -d->width, d->width, d->width);
        if (!visibleBounds.intersects(blockBounds))
            continue;
        // QTextLayout::text() can be empty for a document-owned layout after
        // Qt releases its shaping cache. Read each existing line, whose range
        // remains valid, instead of glyphRuns()'s default text().size() range.
        for (int lineIndex = 0; lineIndex < block.layout()->lineCount(); ++lineIndex) {
            const QTextLine line = block.layout()->lineAt(lineIndex);
            // A wrapped paragraph can contain thousands of offscreen lines.
            // Reject those before retrieving/shaping their glyph runs. Keep a
            // generous line-height guard for italic bearings and font fallback.
            const qreal lineGuard = d->width + line.height();
            if (!visibleBounds.intersects(line.rect().translated(blockOffset)
                    .adjusted(-lineGuard, -lineGuard, lineGuard, lineGuard)))
                continue;
            const auto runs = line.glyphRuns();
            for (const auto& run : runs) {
                const QRawFont font = run.rawFont();
                const auto indexes = run.glyphIndexes();
                const auto positions = run.positions();
                for (qsizetype i = 0; i < indexes.size(); ++i) {
                    const QPointF position = positions[i] + blockOffset;
                    // Do not rasterize previously unseen offscreen glyphs.
                    const QRectF bounds = font.boundingRect(indexes[i])
                        .adjusted(-d->width - 2, -d->width - 2, d->width + 2, d->width + 2);
                    if (!visibleBounds.intersects(bounds.translated(position)))
                        continue;
                    const Mesh mesh = d->glyphMesh(font, indexes[i], newGlyphScale);
                    if (mesh->mask.isNull())
                        continue;
                    if (!visibleBounds.intersects(mesh->bounds.translated(position)))
                        continue;
                    if (mesh->rasterScale < wantedScale * (1 - 1e-5)
                        && !d->rasterUpdatesDeferred && !d->refinementRequested) {
                        d->refinementRequested = true;
                        d->refinementTimer.start();
                    }
                    placed.append({mesh, position});
                    ++d->stats.glyphs;
                    d->stats.triangles += 2; // One ordinary Qt image quad.
                }
            }
        }
        // QQuickTextEdit also does this before scene-graph synchronization:
        // font engines created by glyphRuns() on the GUI thread must not leak
        // into its native fill renderer on the render thread.
        block.layout()->engine()->resetFontEngineCache();
    }

    // Keep the old chunk boundaries on BOTH sides of an edit. An insertion
    // near the start must not shift every subsequent 64-glyph group and force
    // the complete document's GPU geometry to be uploaded again.
    QList<const GlyphMesh*> oldGlyphs;
    QList<qsizetype> boundaries {0};
    for (const auto& chunk : previous) {
        for (const auto& glyph : chunk.glyphs)
            oldGlyphs.append(glyph.mesh.get());
        boundaries.append(oldGlyphs.size());
    }
    qsizetype prefix = 0;
    while (prefix < qMin(oldGlyphs.size(), placed.size())
           && oldGlyphs[prefix] == placed[prefix].mesh.get())
        ++prefix;
    qsizetype suffix = 0;
    while (suffix < qMin(oldGlyphs.size(), placed.size()) - prefix
           && oldGlyphs[oldGlyphs.size() - suffix - 1]
               == placed[placed.size() - suffix - 1].mesh.get())
        ++suffix;

    const auto appendChunk = [this, &placed](qsizetype begin, qsizetype end) {
        if (begin == end)
            return;
        const qsizetype count = end - begin;
        const bool mergeWithPrevious = !d->chunks.isEmpty()
            && d->chunks.constLast().glyphs.size() + count <= glyphsPerChunk;
        if (!mergeWithPrevious) {
            Chunk chunk;
            chunk.origin = placed[begin].position;
            chunk.glyphs.reserve(count);
            d->chunks.append(std::move(chunk));
        }
        Chunk& chunk = d->chunks.last();
        if (mergeWithPrevious)
            chunk.glyphs.reserve(chunk.glyphs.size() + count);
        for (qsizetype i = begin; i < end; ++i) {
            PlacedGlyph glyph {placed[i].mesh, placed[i].position - chunk.origin};
            // Keep the node identity stable when wrapping only changes glyph
            // positions. Alt-resize can then update retained image quads rather
            // than destroy and recreate the whole scene-graph subtree.
            chunk.key = qHashMulti(chunk.key, quintptr(glyph.mesh.get()));
            chunk.glyphs.append(std::move(glyph));
        }
    };
    qsizetype begin = 0;
    qsizetype firstSuffixChunk = 0;
    for (qsizetype i = 1; i < boundaries.size() && boundaries[i] <= prefix; ++i) {
        appendChunk(begin, boundaries[i]);
        begin = boundaries[i];
        firstSuffixChunk = i;
    }
    while (firstSuffixChunk < previous.size()
           && boundaries[firstSuffixChunk] < oldGlyphs.size() - suffix)
        ++firstSuffixChunk;
    const qsizetype delta = placed.size() - oldGlyphs.size();
    const qsizetype suffixStart = boundaries[firstSuffixChunk] + delta;
    while (begin < suffixStart) {
        const qsizetype end = qMin(begin + glyphsPerChunk, suffixStart);
        appendChunk(begin, end);
        begin = end;
    }
    for (qsizetype i = firstSuffixChunk; i < previous.size(); ++i)
        appendChunk(boundaries[i] + delta, boundaries[i + 1] + delta);

    d->stats.chunks = int(d->chunks.size());
    d->trimCache();
    startQualityRefinement(refinementScale);
    d->stats.polishNanoseconds = timer.nsecsElapsed();
    update();
}

QSGNode* TextOutlineItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    QElapsedTimer timer;
    timer.start();
    auto* root = static_cast<OutlineNode*>(oldNode);
    if (d->chunks.isEmpty()) {
        delete root;
        d->stats.atlasedGlyphs = 0;
        d->stats.textureBytes = 0;
        d->stats.uploadedGlyphs = 0;
        d->stats.syncNanoseconds = timer.nsecsElapsed();
        return nullptr;
    }
    if (!root)
        root = new OutlineNode;
    d->stats.uploadedGlyphs = 0;
    const bool colorChanged = root->color != d->color;
    root->color = d->color;
    QSet<const GlyphMesh*> usedTextures;
    const auto textureFor = [this, root](const Mesh& glyph) {
        auto it = root->textures.constFind(glyph.get());
        if (it != root->textures.cend())
            return it->texture;
        QImage colored = glyph->mask;
        if (d->color != glyph->maskColor) {
            QPainter painter(&colored);
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(colored.rect(), d->color);
        }
        // Qt packs these immutable glyphs in its shared atlas when possible.
        // Creation here (on the render thread) is required for atlas support.
        QSGTexture* texture = window()->createTextureFromImage(
            colored, QQuickWindow::TextureCanUseAtlas);
        root->textures.insert(glyph.get(), {glyph, texture});
        ++d->stats.uploadedGlyphs;
        return texture;
    };
    // Common alignment/padding motion belongs on one parent transform. In
    // particular, centering a long line while typing must not move each chunk
    // independently and make Qt rebatch the complete line.
    const QPointF origin = d->chunks.first().origin;
    QMatrix4x4 rootMatrix;
    rootMatrix.translate(origin.x(), origin.y());
    if (root->matrix() != rootMatrix)
        root->setMatrix(rootMatrix);
    d->stats.rebuiltChunks = 0;
    d->stats.movedChunks = 0;
    // Preserve occurrence order for repeated chunks. Prefer the same glyph
    // sequence, then recycle a node with the same quad count. A wrapped resize
    // can replace the visible sequence without requiring any QSG allocation.
    QList<ChunkNode*> available = root->chunks;
    root->chunks.clear();
    for (qsizetype i = 0; i < d->chunks.size(); ++i) {
        const Chunk& chunk = d->chunks[i];
        ChunkNode* node = nullptr;
        bool nodeCreated = false;
        qsizetype candidateIndex = -1;
        for (qsizetype j = 0; j < available.size(); ++j) {
            if (available[j]->key == chunk.key
                && sameGlyphSequence(available[j]->glyphs, chunk.glyphs)) {
                candidateIndex = j;
                break;
            }
        }
        if (candidateIndex < 0) {
            for (qsizetype j = 0; j < available.size(); ++j) {
                if (available[j]->glyphs.size() == chunk.glyphs.size()) {
                    candidateIndex = j;
                    break;
                }
            }
        }
        if (candidateIndex >= 0)
            node = available.takeAt(candidateIndex);
        if (!node) {
            node = new ChunkNode;
            root->appendChildNode(node);
            nodeCreated = true;
        }
        if (nodeCreated)
            ++d->stats.rebuiltChunks;
        root->chunks.append(node);
        for (const auto& glyph : chunk.glyphs)
            usedTextures.insert(glyph.mesh.get());
    }
    // Drop consumers that could not be recycled. Texture pruning happens once
    // retained nodes have received their new references, so no image node ever
    // observes a deleted borrowed texture.
    qDeleteAll(available);
    if (colorChanged) {
        for (auto* node : std::as_const(root->chunks)) {
            qDeleteAll(node->images);
            node->images.clear();
        }
        for (const auto& texture : std::as_const(root->textures))
            delete texture.texture;
        root->textures.clear();
    }
    for (qsizetype i = 0; i < d->chunks.size(); ++i) {
        const Chunk& chunk = d->chunks[i];
        ChunkNode* node = root->chunks[i];
        bool chunkMoved = false;
        QMatrix4x4 matrix;
        matrix.translate(chunk.origin.x() - origin.x(), chunk.origin.y() - origin.y());
        // Unlike QQuickItem setters, QSGTransformNode::setMatrix marks the
        // subtree dirty even when the matrix has not changed.
        if (node->matrix() != matrix) {
            node->setMatrix(matrix);
            chunkMoved = true;
        }
        // Reflow preserves the glyph sequence far more often than it preserves
        // positions. Retain those image nodes and update only the quads whose
        // relative positions changed.
        if (node->images.isEmpty()) {
            for (const auto& glyph : chunk.glyphs) {
                auto* image = window()->createImageNode();
                image->setOwnsTexture(false);
                image->setTexture(textureFor(glyph.mesh));
                image->setFiltering(QSGTexture::Linear);
                image->setRect(glyph.mesh->bounds.translated(glyph.position));
                node->appendChildNode(image);
                node->images.append(image);
            }
        } else if (node->glyphs != chunk.glyphs) {
            Q_ASSERT(node->images.size() == chunk.glyphs.size());
            for (qsizetype glyphIndex = 0; glyphIndex < chunk.glyphs.size(); ++glyphIndex) {
                const auto& glyph = chunk.glyphs[glyphIndex];
                if (node->glyphs[glyphIndex].mesh != glyph.mesh) {
                    node->images[glyphIndex]->setTexture(textureFor(glyph.mesh));
                    chunkMoved = true;
                }
                const QRectF rect = glyph.mesh->bounds.translated(glyph.position);
                if (node->images[glyphIndex]->rect() != rect) {
                    node->images[glyphIndex]->setRect(rect);
                    chunkMoved = true;
                }
            }
        }
        if (node->glyphs != chunk.glyphs) {
            node->glyphs = chunk.glyphs;
            node->key = chunk.key;
        }
        if (chunkMoved)
            ++d->stats.movedChunks;
    }
    // Active nodes no longer borrow unused textures. Release them after node
    // recycling; most resize reflows reuse the same glyph set and allocate
    // nothing, while genuinely new glyphs still use Qt's regular atlas path.
    for (auto it = root->textures.begin(); it != root->textures.end();) {
        if (!usedTextures.contains(it.key())) {
            delete it->texture;
            it = root->textures.erase(it);
        } else {
            ++it;
        }
    }
    d->stats.atlasedGlyphs = 0;
    d->stats.textureBytes = 0;
    for (const auto& entry : std::as_const(root->textures)) {
        d->stats.atlasedGlyphs += entry.texture->isAtlasTexture();
        d->stats.textureBytes += entry.glyph->mask.sizeInBytes();
    }
    d->stats.syncNanoseconds = timer.nsecsElapsed();
    return root;
}

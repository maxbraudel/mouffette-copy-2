#include "frontend/rendering/canvas/TextOutlineItem.h"

#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
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
#include <QtGui/private/qtextengine_p.h>
#include <QtQuick/private/qquicktextedit_p_p.h>
#include <cmath>
#include <algorithm>

namespace {
constexpr int glyphsPerChunk = 64;

struct GlyphMesh {
    QImage mask;
    QRectF bounds;
};
using Mesh = std::shared_ptr<const GlyphMesh>;
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
    qreal rasterScale = 1;
    // GUI-thread-only font objects. Only immutable, font-free masks cross
    // into updatePaintNode while the GUI thread is blocked by Qt's sync phase.
    QHash<QRawFont, QHash<quint32, CachedGlyph>> cache;
    qint64 cacheBytes = 0;
    quint64 cacheEpoch = 0;
    QList<Chunk> chunks;
    Statistics stats;

    void clearCache()
    {
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

    Mesh glyphMesh(const QRawFont& font, quint32 index)
    {
        auto& glyphs = cache[font];
        auto it = glyphs.find(index);
        if (it != glyphs.end()) {
            it->lastUse = cacheEpoch;
            return it->glyph;
        }
        auto mesh = std::make_shared<GlyphMesh>();
        const QPainterPath path = font.pathForGlyph(index);
        if (!path.isEmpty()) {
            const QRectF ink = path.boundingRect().adjusted(-width, -width, width, width);
            // Guard extreme font sizes without allocating unbounded images.
            const qreal scale = qMin(rasterScale,
                2044.0 / qMax(qreal(1), qMax(ink.width(), ink.height())));
            const QRect pixels = QRectF(ink.topLeft() * scale,
                ink.size() * scale).toAlignedRect().adjusted(-2, -2, 2, 2);
            mesh->bounds = QRectF(pixels.topLeft() / scale, QSizeF(pixels.size()) / scale);
            mesh->mask = QImage(pixels.size(), QImage::Format_ARGB32_Premultiplied);
            mesh->mask.fill(Qt::transparent);
            QPainter painter(&mesh->mask);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.scale(scale, scale);
            painter.translate(-mesh->bounds.topLeft());
            painter.strokePath(path, QPen(Qt::white, width * 2, Qt::SolidLine,
                                         Qt::RoundCap, Qt::RoundJoin));
        }
        ++stats.generatedGlyphs;
        cacheBytes += mesh->mask.sizeInBytes();
        glyphs.insert(index, {mesh, cacheEpoch});
        return mesh;
    }
};

TextOutlineItem::TextOutlineItem(QQuickItem* parent)
    : QQuickItem(parent), d(std::make_unique<Private>())
{
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

TextOutlineItem::~TextOutlineItem() = default;
QQuickItem* TextOutlineItem::source() const { return d->source; }
qreal TextOutlineItem::outlinePixels() const { return d->width; }
QColor TextOutlineItem::color() const { return d->color; }
QRectF TextOutlineItem::renderedRect() const { return d->renderedRect; }
QSize TextOutlineItem::renderedPixelSize() const { return d->renderedPixelSize; }
TextOutlineItem::Statistics TextOutlineItem::statistics() const { return d->stats; }

void TextOutlineItem::scheduleLayout()
{
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
    if (!d->source || d->width <= 0)
        return;
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
    d->color = color;
    update();
    emit colorChanged();
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
    if (wantedScale > d->rasterScale * (1 + 1e-5)
        || wantedScale < d->rasterScale * 0.5) {
        d->rasterScale = std::exp2(std::ceil(std::log2(wantedScale) * 2 - 1e-5) / 2);
        d->clearCache();
        contentChanged = true;
    }
    // Keep a small screen-space guard around the visible area. Panning or
    // dragging within it needs neither document access nor node rebuilding,
    // and the optional translucent mask retains an identical source rectangle.
    const QRectF itemBounds = boundingRect().toAlignedRect();
    const QRectF previousBounds = d->renderedRect;
    if (!visibleBounds.isEmpty()) {
        if (previousBounds.contains(visibleBounds) && itemBounds.contains(previousBounds)) {
            visibleBounds = previousBounds;
        } else {
            const qreal guard = 96 * dpr / qMax(qreal(0.0625), density);
            visibleBounds = visibleBounds.adjusted(-guard, -guard, guard, guard)
                .toAlignedRect().intersected(itemBounds.toRect());
        }
    }
    const auto textureExtent = [this](qreal extent) {
        return int(qBound(qreal(1), std::ceil(extent * d->rasterScale), qreal(4096)));
    };
    const QSize pixelSize(textureExtent(visibleBounds.width()), textureExtent(visibleBounds.height()));
    if (d->renderedRect != visibleBounds || d->renderedPixelSize != pixelSize) {
        d->renderedRect = visibleBounds;
        d->renderedPixelSize = pixelSize;
        emit viewportChanged();
    }
    if (!contentChanged && previousBounds == visibleBounds) {
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
                    const Mesh mesh = d->glyphMesh(font, indexes[i]);
                    if (mesh->mask.isNull())
                        continue;
                    if (!visibleBounds.intersects(mesh->bounds.translated(position)))
                        continue;
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
        if (d->color != QColor(Qt::white)) {
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

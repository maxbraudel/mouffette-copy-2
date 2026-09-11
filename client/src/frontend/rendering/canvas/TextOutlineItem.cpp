// Qt's private curve material headers use QStringBuilder concatenation, as
// does Qt Quick itself. Keep that build convention local to this adapter.
#define QT_USE_QSTRINGBUILDER
#include <QStringBuilder>
#include "frontend/rendering/canvas/TextOutlineItem.h"

#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
#include <QGlyphRun>
#include <QHash>
#include <QPointer>
#include <QRawFont>
#include <QSGTransformNode>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QtGui/private/qtextengine_p.h>
#include <QtQuick/private/qquicktextedit_p_p.h>
#include <QtQuick/private/qsgcurveprocessor_p.h>
#include <QtQuick/private/qsgcurvestrokenode_p.h>

#include <array>
#include <cmath>

namespace {
constexpr int glyphsPerChunk = 64;

struct Triangle {
    std::array<QVector2D, 3> vertices;
    std::array<QVector2D, 3> controls;
    std::array<QVector2D, 3> normals;
    std::array<float, 3> extrusions;
    bool line;
};
struct GlyphMesh {
    QList<Triangle> triangles;
    QRectF bounds;
};
using Mesh = std::shared_ptr<const GlyphMesh>;

struct PlacedGlyph {
    Mesh mesh;
    QPointF position;
    bool operator==(const PlacedGlyph& other) const
    { return mesh == other.mesh && position == other.position; }
};
struct Chunk {
    QList<PlacedGlyph> glyphs;
    QPointF origin;
    size_t key = 0;
};
struct ChunkNode : QSGTransformNode {
    QList<PlacedGlyph> glyphs;
    QSGCurveStrokeNode* stroke = nullptr;
    size_t key = 0;
};
struct OutlineNode : QSGTransformNode {
    QList<ChunkNode*> chunks;
};
}

struct TextOutlineItem::Private {
    QPointer<QQuickTextEdit> source;
    QList<QMetaObject::Connection> connections;
    qreal width = 0;
    QColor color = Qt::black;
    bool layoutDirty = true;
    // GUI-thread-only font objects. Only immutable, font-free meshes cross
    // into updatePaintNode while the GUI thread is blocked by Qt's sync phase.
    QHash<QRawFont, QHash<quint32, Mesh>> cache;
    QList<Chunk> chunks;
    Statistics stats;

    Mesh glyphMesh(const QRawFont& font, quint32 index)
    {
        auto& glyphs = cache[font];
        auto it = glyphs.constFind(index);
        if (it != glyphs.cend())
            return it.value();
        auto mesh = std::make_shared<GlyphMesh>();
        const QPainterPath path = font.pathForGlyph(index);
        mesh->bounds = path.boundingRect().adjusted(-width, -width, width, width);
        // Process once per unique glyph/width, using Qt's own analytic stroke
        // renderer. Unlike the stock Text.Outline style, width is unrestricted.
        QSGCurveProcessor::processStroke(QQuadPath::fromPainterPath(path), 2,
            float(width * 2), false, Qt::RoundJoin, Qt::RoundCap,
            [&mesh](const auto& v, const auto& c, const auto& n, const auto& e,
                    QSGCurveStrokeNode::TriangleFlags flags) {
                mesh->triangles.append({v, c, n, e,
                    flags.testFlag(QSGCurveStrokeNode::TriangleFlag::Line)});
            });
        ++stats.generatedGlyphs;
        glyphs.insert(index, mesh);
        return mesh;
    }
};

TextOutlineItem::TextOutlineItem(QQuickItem* parent)
    : QQuickItem(parent), d(std::make_unique<Private>())
{
    setFlag(ItemHasContents);
}

TextOutlineItem::~TextOutlineItem() = default;
QQuickItem* TextOutlineItem::source() const { return d->source; }
qreal TextOutlineItem::outlinePixels() const { return d->width; }
QColor TextOutlineItem::color() const { return d->color; }
TextOutlineItem::Statistics TextOutlineItem::statistics() const { return d->stats; }

void TextOutlineItem::scheduleLayout()
{
    d->layoutDirty = true;
    polish();
    update();
}

void TextOutlineItem::setSource(QQuickItem* item)
{
    auto* edit = qobject_cast<QQuickTextEdit*>(item);
    if (d->source == edit)
        return;
    for (const auto& connection : std::as_const(d->connections))
        disconnect(connection);
    d->connections.clear();
    d->cache.clear();
    d->source = edit;
    if (edit) {
        const auto watch = [this, edit](auto signal) {
            d->connections.append(connect(edit, signal, this, &TextOutlineItem::scheduleLayout));
        };
        watch(&QQuickTextEdit::textChanged);
        watch(&QQuickTextEdit::preeditTextChanged);
        watch(&QQuickTextEdit::contentSizeChanged);
        d->connections.append(connect(edit, &QQuickTextEdit::fontChanged, this, [this] {
            d->cache.clear();
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
        d->connections.append(connect(edit, &QObject::destroyed, this, [this] {
            d->cache.clear();
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
    d->cache.clear();
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
    scheduleLayout();
}

void TextOutlineItem::updatePolish()
{
    if (!d->layoutDirty)
        return;
    QElapsedTimer timer;
    timer.start();
    d->layoutDirty = false;
    const auto previous = std::move(d->chunks);
    d->chunks = {};
    d->stats = {};
    if (!d->source || d->width <= 0)
        return;

    auto* edit = d->source.data();
    edit->ensurePolished();
    auto* text = QQuickTextEditPrivate::get(edit);
    auto* doc = text->document;
    auto* layout = doc->documentLayout();
    layout->documentSize(); // Finish any pending QTextDocument layout.
    // Read Qt's actual offset, including padding and RTL alignment. Recreating
    // its alignment formulas was the cause of fill/border drift in the SVG path.
    const QPointF offset = edit->mapToItem(this, QPointF(text->xoff, text->yoff));
    const QRectF visibleBounds(0, 0, width(), height());
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
          const auto runs = line.glyphRuns();
          for (const auto& run : runs) {
            const QRawFont font = run.rawFont();
            const auto indexes = run.glyphIndexes();
            const auto positions = run.positions();
            for (qsizetype i = 0; i < indexes.size(); ++i) {
                const Mesh mesh = d->glyphMesh(font, indexes[i]);
                if (mesh->triangles.isEmpty())
                    continue;
                const QPointF position = positions[i] + blockOffset;
                if (!visibleBounds.intersects(mesh->bounds.translated(position)))
                    continue;
                placed.append({mesh, position});
                ++d->stats.glyphs;
                d->stats.triangles += mesh->triangles.size();
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
        Chunk chunk;
        chunk.origin = placed[begin].position;
        chunk.glyphs.reserve(end - begin);
        for (qsizetype i = begin; i < end; ++i) {
            PlacedGlyph glyph {placed[i].mesh, placed[i].position - chunk.origin};
            chunk.key = qHashMulti(chunk.key, quintptr(glyph.mesh.get()),
                                   glyph.position.x(), glyph.position.y());
            chunk.glyphs.append(std::move(glyph));
        }
        d->chunks.append(std::move(chunk));
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
        return nullptr;
    }
    if (!root)
        root = new OutlineNode;
    // Common alignment/padding motion belongs on one parent transform. In
    // particular, centering a long line while typing must not move each chunk
    // independently and make Qt rebatch the complete line.
    const QPointF origin = d->chunks.first().origin;
    QMatrix4x4 rootMatrix;
    rootMatrix.translate(origin.x(), origin.y());
    root->setMatrix(rootMatrix);
    d->stats.rebuiltChunks = 0;
    QMultiHash<size_t, ChunkNode*> available;
    for (auto* node : std::as_const(root->chunks))
        available.insert(node->key, node);
    root->chunks.clear();
    for (qsizetype i = 0; i < d->chunks.size(); ++i) {
        const Chunk& chunk = d->chunks[i];
        ChunkNode* node = nullptr;
        auto candidate = available.find(chunk.key);
        while (candidate != available.end() && candidate.key() == chunk.key) {
            if (candidate.value()->glyphs == chunk.glyphs) {
                node = candidate.value();
                available.erase(candidate);
                break;
            }
            ++candidate;
        }
        if (!node) {
            node = new ChunkNode;
            root->appendChildNode(node);
        }
        root->chunks.append(node);
        QMatrix4x4 matrix;
        matrix.translate(chunk.origin.x() - origin.x(), chunk.origin.y() - origin.y());
        node->setMatrix(matrix);
        if (node->glyphs != chunk.glyphs) {
            delete node->stroke;
            node->stroke = new QSGCurveStrokeNode;
            node->stroke->setStrokeWidth(float(d->width * 2));
            for (const auto& glyph : chunk.glyphs) {
                const QVector2D p(glyph.position);
                for (const auto& t : glyph.mesh->triangles) {
                    const std::array<QVector2D, 3> vertices {
                        t.vertices[0] + p, t.vertices[1] + p, t.vertices[2] + p};
                    if (t.line) {
                        node->stroke->appendTriangle(vertices,
                            std::array<QVector2D, 2>{t.controls[0] + p, t.controls[2] + p},
                            t.normals, t.extrusions);
                    } else {
                        node->stroke->appendTriangle(vertices,
                            {t.controls[0] + p, t.controls[1] + p, t.controls[2] + p},
                            t.normals, t.extrusions);
                    }
                }
            }
            node->stroke->cookGeometry();
            node->appendChildNode(node->stroke);
            node->glyphs = chunk.glyphs;
            node->key = chunk.key;
            ++d->stats.rebuiltChunks;
        }
        if (node->stroke->color() != d->color)
            node->stroke->setColor(d->color);
    }
    qDeleteAll(available);
    d->stats.syncNanoseconds = timer.nsecsElapsed();
    return root;
}

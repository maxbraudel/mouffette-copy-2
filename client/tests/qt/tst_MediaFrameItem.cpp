#include "frontend/rendering/remote/RemoteVideoFrameItem.h"
#include "frontend/rendering/canvas/TimelineThumbnailItem.h"
#include "backend/media/MediaResidencyManager.h"
#include "backend/media/DecodeScheduler.h"
#include "backend/media/MediaDecoder.h"
#include "shared/rendering/SharedVideoNode.h"
#include "../fixtures/ThumbnailVideoFixture.h"

#include <QGuiApplication>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QSignalSpy>
#include <QTest>
#include <QTemporaryDir>
#include <QtGui/private/qrhi_p.h>

#include <memory>

namespace {

class ObservedFrameItem final : public RemoteVideoFrameItem
{
public:
    using RemoteVideoFrameItem::RemoteVideoFrameItem;

    QSize textureSize;
    QRectF imageRect;
    quintptr textureIdentity = 0;
    int createdNodes = 0;
    bool hasImageNode = false;

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override
    {
        const bool creatingNode = !oldNode;
        QSGNode* node = RemoteVideoFrameItem::updatePaintNode(oldNode, data);
        createdNodes += creatingNode && node;
        auto* imageNode = node ? dynamic_cast<QSGImageNode*>(node->firstChild()) : nullptr;
        hasImageNode = imageNode && imageNode->texture();
        textureSize = hasImageNode ? imageNode->texture()->textureSize() : QSize();
        imageRect = imageNode ? imageNode->rect() : QRectF();
        textureIdentity = hasImageNode
            ? reinterpret_cast<quintptr>(imageNode->texture()) : 0;
        return node;
    }
};

// Drive the real scene graph deterministically, including GPU readback, without
// native-window focus or wall-clock performance assertions. The callback above
// runs during sync; no scene-graph object is read from outside that callback.
template<class Item = ObservedFrameItem>
struct Scene
{
    std::unique_ptr<QRhiTexture> texture;
    std::unique_ptr<QRhiRenderBuffer> depthStencil;
    std::unique_ptr<QRhiTextureRenderTarget> renderTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> renderPass;
    QQuickRenderControl control;
    QQuickWindow window{&control};
    QQuickItem* camera = new QQuickItem(window.contentItem());
    Item* item = new Item(camera);
    qreal devicePixelRatio;

    explicit Scene(qreal dpr = 1) : devicePixelRatio(dpr)
    {
        window.setColor(QColor(16, 32, 48));
        window.resize(256, 192);
        window.contentItem()->setSize(window.size());
        camera->setTransformOrigin(QQuickItem::TopLeft);
        item->setTransformOrigin(QQuickItem::TopLeft);
        item->setSize({128, 96});
    }

    ~Scene()
    {
        window.setRenderTarget({});
        control.invalidate();
        renderTarget.reset();
        renderPass.reset();
        depthStencil.reset();
        texture.reset();
    }

    bool initialize()
    {
        if (!control.initialize() || !control.rhi())
            return false;
        QRhi* rhi = control.rhi();
        const QSize pixelSize = window.size() * devicePixelRatio;
        texture.reset(rhi->newTexture(QRhiTexture::RGBA8, pixelSize, 1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!texture || !texture->create())
            return false;
        depthStencil.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil,
                                                pixelSize, 1));
        if (!depthStencil || !depthStencil->create())
            return false;
        QRhiTextureRenderTargetDescription description(QRhiColorAttachment(texture.get()));
        description.setDepthStencilBuffer(depthStencil.get());
        renderTarget.reset(rhi->newTextureRenderTarget(description));
        if (!renderTarget)
            return false;
        renderPass.reset(renderTarget->newCompatibleRenderPassDescriptor());
        if (!renderPass)
            return false;
        renderTarget->setRenderPassDescriptor(renderPass.get());
        if (!renderTarget->create())
            return false;
        auto quickTarget = QQuickRenderTarget::fromRhiRenderTarget(renderTarget.get());
        quickTarget.setDevicePixelRatio(devicePixelRatio);
        window.setRenderTarget(quickTarget);
        return true;
    }

    QImage render()
    {
        QRhiReadbackResult result;
        control.polishItems();
        control.beginFrame();
        control.sync();
        control.render();
        auto* updates = control.rhi()->nextResourceUpdateBatch();
        updates->readBackTexture(QRhiReadbackDescription(texture.get()), &result);
        control.commandBuffer()->resourceUpdate(updates);
        control.endFrame();
        control.rhi()->finish();
        if (result.data.isEmpty())
            return {};
        QImage image(reinterpret_cast<const uchar*>(result.data.constData()),
                     result.pixelSize.width(), result.pixelSize.height(),
                     QImage::Format_RGBA8888_Premultiplied);
        image = image.copy();
        if (control.rhi()->isYUpInFramebuffer())
            image.flip(Qt::Vertical);
        image.setDevicePixelRatio(devicePixelRatio);
        return image;
    }
};

QImage quadrantImage(QImage::Format format = QImage::Format_ARGB32_Premultiplied)
{
    QImage image(32, 24, format);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = y < image.height() / 2
                ? (x < image.width() / 2 ? QColor(Qt::red) : QColor(0, 255, 0, 128))
                : (x < image.width() / 2 ? QColor(Qt::blue) : QColor(Qt::transparent));
            image.setPixelColor(x, y, color);
        }
    }
    return image;
}

void verifyPixel(const QImage& image, QPoint position, QColor expected)
{
    QVERIFY(!image.isNull());
    position *= image.devicePixelRatio();
    QVERIFY(image.rect().contains(position));
    const QColor actual = image.pixelColor(position);
    QVERIFY2(qAbs(actual.red() - expected.red()) <= 1
                 && qAbs(actual.green() - expected.green()) <= 1
                 && qAbs(actual.blue() - expected.blue()) <= 1
                 && qAbs(actual.alpha() - expected.alpha()) <= 1,
             qPrintable(QStringLiteral("pixel %1,%2: got %3, expected %4")
                 .arg(position.x()).arg(position.y())
                 .arg(actual.name(QColor::HexArgb)).arg(expected.name(QColor::HexArgb))));
}

void verifyQuadrants(const QImage& image)
{
    verifyPixel(image, {32, 24}, Qt::red);
    verifyPixel(image, {96, 24}, QColor(8, 144, 24));
    verifyPixel(image, {32, 72}, Qt::blue);
    verifyPixel(image, {96, 72}, QColor(16, 32, 48));
    verifyPixel(image, {200, 140}, QColor(16, 32, 48));
}

class ObservedThumbnailItem final : public TimelineThumbnailItem {
public:
    using TimelineThumbnailItem::TimelineThumbnailItem;
    QList<QRectF> quads;
    QList<QRectF> sources;
    QList<QRectF> cellRects;
    QList<quintptr> cellTextures;
    QList<quintptr> quadTextures;
    QSet<quintptr> textures;
    QList<QSize> textureSizes;
protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override {
        auto* node = TimelineThumbnailItem::updatePaintNode(oldNode, data);
        quads.clear(); sources.clear(); cellRects.clear(); cellTextures.clear();
        quadTextures.clear(); textures.clear(); textureSizes.clear();
        for (auto* child = node ? node->firstChild() : nullptr; child; child = child->nextSibling()) {
            QRectF cell;
            quintptr identity = 0;
            const auto observe = [&](QSGImageNode* quad) {
                quads.append(quad->rect()); sources.append(quad->sourceRect());
                identity = reinterpret_cast<quintptr>(quad->texture());
                quadTextures.append(identity); textures.insert(identity);
                textureSizes.append(quad->texture()->textureSize());
                cell = cell.united(quad->rect());
            };
            // Observe logical cells separately from repeated quads. Accept the
            // previous flat graph too so the regression can run against it.
            if (auto* quad = dynamic_cast<QSGImageNode*>(child)) observe(quad);
            else for (auto* image = child->firstChild(); image; image = image->nextSibling())
                observe(static_cast<QSGImageNode*>(image));
            if (!cell.isEmpty()) { cellRects.append(cell); cellTextures.append(identity); }
        }
        return node;
    }
};

bool coversViewport(const ObservedThumbnailItem* item, qreal left, qreal right) {
    qreal edge = left;
    for (const auto& rect : item->quads) {
        if (qAbs(rect.left() - edge) > 0.001 || rect.width() <= 0) return false;
        edge = rect.right();
    }
    return qAbs(edge - right) < 0.001;
}

QHash<qint64, quintptr> sourceTextureIdentities(const ObservedThumbnailItem* item) {
    QHash<qint64, quintptr> result;
    const qreal left = std::max(qreal(0), item->visibleLeft());
    for (int i = 0; i < item->cellRects.size(); ++i) {
        // The first visible cell may be cropped; the other left edges are the
        // source grid anchors, independently of zoom, trim and viewport offset.
        if (item->cellRects[i].left() <= left + .001) continue;
        const auto time = qRound64((item->sourceInMs() + item->cellRects[i].left() / item->pixelsPerMs()) * 1000);
        result.insert(time, item->cellTextures[i]);
    }
    return result;
}

} // namespace

class MediaFrameItemTest final : public QObject
{
    Q_OBJECT

private slots:
    void nativeVideoPlanesPreserveAlphaAndOrientation() {
        RemoteVideoFrameSource source;
        QVideoFrame video(quadrantImage());
        source.setVideoFrame(video);
        Scene scene;
        scene.item->setFrameSource(&source);
        QVERIFY(scene.initialize());
        verifyQuadrants(scene.render());
        video.setRotation(QtVideo::Rotation::Clockwise90);
        source.setVideoFrame(video);
        auto rotated = scene.render();
        verifyPixel(rotated, {32,24}, Qt::blue);
        verifyPixel(rotated, {96,24}, Qt::red);
        source.clear();
        verifyPixel(scene.render(), {32,24}, QColor(16,32,48));
    }
    void nativeYuvFrameRendersWithoutRgbaSource() {
        QString error;
        auto asset = MediaDecoder::decode(QString::fromUtf8(TEST_VIDEO_FILE), {}, &error);
        QVERIFY2(asset, qPrintable(error));
        RemoteVideoFrameSource source;
        source.setVideoFrame(asset->firstFrame.frame);
        QVERIFY(source.frame().isNull());
        Scene scene;
        scene.item->setFrameSource(&source);
        QVERIFY(scene.initialize());
        const auto rendered = scene.render();
        QVERIFY(!rendered.isNull());
        const auto reference = ResidentVideoPlayer::presentationFrame(asset->firstFrame.frame).toImage();
        const QColor expected = reference.pixelColor(reference.width()/2, reference.height()/2);
        const QColor actual = rendered.pixelColor(64,48);
        QVERIFY(qAbs(actual.red()-expected.red()) <= 3);
        QVERIFY(qAbs(actual.green()-expected.green()) <= 3);
        QVERIFY(qAbs(actual.blue()-expected.blue()) <= 3);
    }

    void thumbnailRepetitionZoomViewportAndRelease_data() {
        QTest::addColumn<qreal>("dpr");
        QTest::newRow("normal") << qreal(1);
        QTest::newRow("retina") << qreal(2);
    }

    void thumbnailRepetitionZoomViewportAndRelease() {
        QFETCH(qreal, dpr);
        QTemporaryDir temporary;
        const QString path = temporary.filePath("thumbnail.png");
        QImage source(96, 48, QImage::Format_RGBA8888);
        source.fill(Qt::red);
        for (int y = 0; y < source.height(); ++y)
            for (int x = source.width() / 2; x < source.width(); ++x) source.setPixelColor(x, y, Qt::blue);
        QVERIFY(source.save(path));
        auto& manager = MediaResidencyManager::instance();
        const QString owner = "thumbnail-render-test";
        manager.acquire(owner, path);
        auto cleanup = qScopeGuard([&] { manager.release(owner); });
        QTRY_VERIFY(manager.ready(owner));
        Scene<ObservedThumbnailItem> scene(dpr);
        scene.item->setSize({230, 48});
        scene.item->setVisibleRight(230);
        scene.item->setOwnerId(owner);
        QVERIFY(scene.item->hasThumbnails());
        QVERIFY(scene.initialize());
        const QImage frame = scene.render();
        verifyPixel(frame, {12, 20}, Qt::red);
        verifyPixel(frame, {72, 20}, Qt::blue);
        verifyPixel(frame, {108, 20}, Qt::red);
        verifyPixel(frame, {210, 20}, Qt::red); // Partial last tile is cropped, never stretched.
        QCOMPARE(scene.item->quads.size(), 3);
        QCOMPARE(scene.item->textures.size(), 1); // One upload for a repeated image.
        const auto textures = scene.item->textures;
        scene.item->setWidth(1000000); // Zoom must not allocate a clip-sized surface or a million nodes.
        scene.item->setPixelsPerMs(100);
        QVERIFY(!scene.render().isNull());
        QCOMPARE(scene.item->quads.size(), 3);
        QCOMPARE(scene.item->textures, textures);
        scene.item->setVisibleLeft(101);
        scene.item->setVisibleRight(201);
        const auto scrolled = scene.render();
        verifyPixel(scrolled, {100, 20}, QColor(16, 32, 48));
        verifyPixel(scrolled, {108, 20}, Qt::red);
        verifyPixel(scrolled, {200, 20}, Qt::red);
        verifyPixel(scrolled, {202, 20}, QColor(16, 32, 48));
        QCOMPARE(scene.item->quads.size(), 2);
        QCOMPARE(scene.item->textures, textures);
        // Releasing one owner clears its strip even while another owns the same asset.
        manager.acquire("thumbnail-shared", path);
        QTRY_VERIFY(manager.ready("thumbnail-shared"));
        manager.release(owner);
        QVERIFY(!scene.item->hasThumbnails());
        QVERIFY(!scene.render().isNull());
        QVERIFY(scene.item->textures.isEmpty());
        QVERIFY(manager.ready("thumbnail-shared"));
        manager.release("thumbnail-shared");
    }

    void thumbnailVideoSourceTimeAndHolds() {
        auto& manager = MediaResidencyManager::instance();
        const QString owner = "thumbnail-video-test";
        manager.acquire(owner, QString::fromUtf8(TEST_VIDEO_FILE));
        auto cleanup = qScopeGuard([&] { manager.release(owner); });
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready(owner), 15000);
        const auto asset = manager.asset(owner);
        QVERIFY(asset->frameIndex.size() > 2);
        Scene<ObservedThumbnailItem> scene;
        scene.item->setSize({240, 44});
        scene.item->setVisibleRight(240);
        scene.item->setOwnerId(owner);
        scene.item->setPixelsPerMs(1);
        scene.item->setSourceInMs(-10000);
        QVERIFY(scene.initialize());
        QTRY_VERIFY((scene.render(), !scene.item->textures.isEmpty()));
        QCOMPARE(scene.item->textures.size(), 1); // First-frame hold.
        const auto firstTextures = scene.item->textures;
        scene.item->setSourceInMs(asset->durationUs / 1000.0 + 10000);
        QVERIFY(!scene.render().isNull());
        QTRY_COMPARE((scene.render(), scene.item->textures.size()), 1); // Final-frame hold.
        QTRY_VERIFY(!DecodeScheduler::instance().thumbnail(*asset, asset->frameIndex.last().timestampUs).isNull());
        QVERIFY(!scene.render().isNull());
        const auto heldTextures = scene.item->textures;
        scene.item->setSourceInMs(asset->durationUs / 1000.0 + 20000);
        QVERIFY(!scene.render().isNull());
        QCOMPARE(scene.item->textures, heldTextures);
        scene.item->setSourceInMs(0);
        scene.item->setPixelsPerMs(240.0 / (asset->durationUs / 1000.0));
        QVERIFY(!scene.render().isNull());
        QTRY_VERIFY((scene.render(), scene.item->textures.size() > 1));
        for (const auto& size : scene.item->textureSizes) {
            QVERIFY(size.width() <= MediaThumbnails::Width);
            QVERIFY(size.height() <= MediaThumbnails::Height);
        }
    }

    void thumbnailCacheEvictionRecoversWithoutLayoutChange() {
        auto& manager = MediaResidencyManager::instance();
        auto& scheduler = DecodeScheduler::instance();
        const QString owner = "thumbnail-eviction-test";
        manager.acquire(owner, QString::fromUtf8(TEST_VIDEO_FILE));
        auto cleanup = qScopeGuard([&] { manager.release(owner); scheduler.setOptionalCachingEnabled(true); });
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready(owner), 15000);
        Scene<ObservedThumbnailItem> scene;
        scene.item->setSize({240, 44});
        scene.item->setVisibleRight(240);
        scene.item->setOwnerId(owner);
        scene.item->setPixelsPerMs(1);
        QVERIFY(scene.initialize());
        const auto completeStrip = [&] {
            scene.render();
            qreal paintedWidth = 0;
            for (const auto& quad : scene.item->quads) paintedWidth += quad.width();
            return qFuzzyCompare(paintedWidth, scene.item->width());
        };
        QTRY_VERIFY(completeStrip());
        const auto displayedTextures = scene.item->textures;
        scheduler.evictOptionalCaches();
        scene.item->update(); // Selection/repaint, with no zoom or viewport change.
        // A normal cache purge cannot expose a blank intermediate frame.
        QVERIFY(completeStrip());
        QCOMPARE(scene.item->textures, displayedTextures);
        QTRY_VERIFY_WITH_TIMEOUT(completeStrip(), 3000);
        QVERIFY(scene.item->hasThumbnails());
        scheduler.setOptionalCachingEnabled(false);
        QVERIFY(!scene.item->hasThumbnails());
        QCOMPARE(scene.item->retainedThumbnailBytes(), quint64(0));
        QVERIFY(!scene.render().isNull());
        QVERIFY(scene.item->textures.isEmpty());
        scheduler.setOptionalCachingEnabled(true);
        QTRY_VERIFY(completeStrip());
        QVERIFY(scene.item->hasThumbnails());
    }

    void videoFilmstripKeepsSourceIdentitiesAcrossZoomAndTrim() {
        auto& manager = MediaResidencyManager::instance();
        const QString owner = "thumbnail-stable-source-grid";
        manager.acquire(owner, QString::fromUtf8(TEST_VIDEO_FILE));
        const auto cleanup = qScopeGuard([&] { manager.release(owner); });
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready(owner), 15000);
        Scene<ObservedThumbnailItem> scene;
        scene.item->setSize({240, 44});
        scene.item->setVisibleRight(240);
        scene.item->setPixelsPerMs(.2);
        scene.item->setOwnerId(owner);
        QVERIFY(scene.initialize());
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(DecodeScheduler::instance().pendingJobs(), 0, 10000);
        QVERIFY(!scene.render().isNull());
        QVERIFY(coversViewport(scene.item, 0, 240));
        const auto original = sourceTextureIdentities(scene.item);
        QVERIFY(original.size() >= 2);

        // Small zoom steps project the same media-time cells. They do not
        // request new exact timestamps for every visible screen coordinate.
        scene.item->setPixelsPerMs(.21);
        QVERIFY(!scene.render().isNull());
        const auto zoomed = sourceTextureIdentities(scene.item);
        QCOMPARE(zoomed, original);
        scene.item->setSourceInMs(250);
        QVERIFY(!scene.render().isNull());
        const auto trimmed = sourceTextureIdentities(scene.item);
        for (auto it = original.cbegin(); it != original.cend(); ++it)
            QCOMPARE(trimmed.value(it.key()), it.value());
        QVERIFY(coversViewport(scene.item, 0, 240));

        // Crossing a density threshold inserts nested source samples. A small
        // reversal around that threshold must retain the newly selected level.
        scene.item->setSourceInMs(0);
        scene.item->setPixelsPerMs(.252);
        QVERIFY(!scene.render().isNull());
        const qreal step = scene.item->quads.first().width() / scene.item->pixelsPerMs();
        for (qreal scale : {.249, .251, .248, .252}) {
            scene.item->setPixelsPerMs(scale);
            QVERIFY(!scene.render().isNull());
            QVERIFY(coversViewport(scene.item, 0, 240));
            QVERIFY(qAbs(scene.item->quads.first().width() / scale - step) < .001);
        }
    }

    void videoFilmstripKeepsVerticalFramingDuringZoom_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<qreal>("dpr");
        for (qreal dpr : {1., 2.}) {
            QTest::newRow(qPrintable(QStringLiteral("landscape-%1").arg(dpr))) << QSize(192, 108) << dpr;
            QTest::newRow(qPrintable(QStringLiteral("portrait-%1").arg(dpr))) << QSize(108, 192) << dpr;
            QTest::newRow(qPrintable(QStringLiteral("panorama-%1").arg(dpr))) << QSize(192, 48) << dpr;
            QTest::newRow(qPrintable(QStringLiteral("narrow-%1").arg(dpr))) << QSize(24, 192) << dpr;
        }
    }

    void videoFilmstripKeepsVerticalFramingDuringZoom() {
        QFETCH(QSize, size);
        QFETCH(qreal, dpr);
        QTemporaryDir temporary;
        const auto path = temporary.filePath("framing.mp4");
        QVERIFY(writeThumbnailVideoFixture(path, size));
        auto& manager = MediaResidencyManager::instance();
        const QString owner = "thumbnail-vertical-framing";
        manager.acquire(owner, path);
        const auto cleanup = qScopeGuard([&] { manager.release(owner); });
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready(owner), 15000);

        Scene<ObservedThumbnailItem> scene(dpr);
        scene.item->setSize({1200, 44});
        scene.item->setVisibleRight(240);
        scene.item->setSourceInMs(-10000); // First-frame hold uses the real video layout.
        scene.item->setOwnerId(owner);
        const qreal naturalWidth = 44. * size.width() / size.height();
        scene.item->setPixelsPerMs(naturalWidth / 1000.);
        QVERIFY(scene.initialize());
        const qreal factors[] = {.85, 1.05, 1.25, 1.55, 1.59, 1.61, 2., 1.6, 1.2, .64, .42, .65};
        bool sawRepeatedCell = false;
        for (int frame = 0; frame < 36; ++frame) {
            const qreal left = (frame * 37) % 900;
            scene.item->setPixelsPerMs(naturalWidth * factors[frame % 12] / 1000.);
            scene.item->setSourceInMs(-10000 + (frame % 3) * 137);
            scene.item->setVisibleLeft(left);
            scene.item->setVisibleRight(left + 240);
            scene.camera->setX(-left);
            const auto rendered = scene.render();
            QVERIFY(!rendered.isNull());
            QVERIFY(coversViewport(scene.item, left, left + 240));
            sawRepeatedCell |= scene.item->quads.size() > scene.item->cellRects.size();
            for (int i = 0; i < scene.item->quads.size(); ++i) {
                const auto source = scene.item->sources[i];
                const auto target = scene.item->quads[i];
                const auto texture = scene.item->textureSizes[i];
                // Every row remains visible; magnification never follows zoom.
                QCOMPARE(source.top(), qreal(0));
                QCOMPARE(source.height(), qreal(texture.height()));
                QVERIFY(source.left() >= -1e-6);
                QVERIFY(source.right() <= texture.width() + 1e-6);
                QVERIFY(qAbs(target.width() / source.width() - target.height() / source.height()) < 1e-6);
            }
            // Actual GPU pixels, including atlas boundaries and Retina: dark
            // top and bright bottom bands must survive every intermediate frame.
            for (int x : {2, 13, 47, 109, 191, 237}) {
                const QColor top = rendered.pixelColor(qRound(x * dpr), qRound(2 * dpr));
                const QColor bottom = rendered.pixelColor(qRound(x * dpr), qRound(41 * dpr));
                QVERIFY2(top.red() < 70 && top.green() < 70 && top.blue() < 70,
                         qPrintable(QStringLiteral("top band lost at frame %1, x %2").arg(frame).arg(x)));
                QVERIFY2(bottom.red() > 200 && bottom.green() > 200 && bottom.blue() > 200,
                         qPrintable(QStringLiteral("bottom band lost at frame %1, x %2").arg(frame).arg(x)));
            }
            QCoreApplication::processEvents();
        }
        if (size.width() <= size.height() * 2.4) QVERIFY(sawRepeatedCell);
    }

    void videoFilmstripCoversEveryFrameOfZoomScrollAndTrim() {
        auto& manager = MediaResidencyManager::instance();
        auto& scheduler = DecodeScheduler::instance();
        const QString owner = "thumbnail-continuous-gesture";
        manager.acquire(owner, QString::fromUtf8(TEST_VIDEO_FILE));
        const auto cleanup = qScopeGuard([&] { manager.release(owner); scheduler.setOptionalCachingEnabled(true); });
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready(owner), 15000);
        Scene<ObservedThumbnailItem> scene;
        scene.item->setSize({1200, 44});
        scene.item->setVisibleRight(240);
        scene.item->setPixelsPerMs(.02);
        scene.item->setOwnerId(owner);
        QVERIFY(scene.initialize());
        QTRY_VERIFY((scene.render(), coversViewport(scene.item, 0, 240)));
        const qreal scales[] = {.02, .021, .028, .05, .11, .24, .4, .39, .23, .09, .04, .012};
        for (int frame = 0; frame < 72; ++frame) {
            const qreal left = (frame * 37) % 900;
            scene.item->setPixelsPerMs(scales[frame % 12]);
            scene.item->setSourceInMs(frame % 3 == 0 ? -750 : (frame % 7) * 137);
            scene.item->setVisibleLeft(left);
            scene.item->setVisibleRight(left + 240);
            scene.camera->setX(-left);
            if (frame % 11 == 0) scheduler.evictOptionalCaches();
            // Render before servicing decode completions: an eventual QTRY
            // success would miss the transient blanking this guards against.
            QVERIFY(!scene.render().isNull());
            QVERIFY2(coversViewport(scene.item, left, left + 240), qPrintable(QString::number(frame)));
            QVERIFY(scene.item->retainedThumbnailBytes() <= TimelineThumbnailItem::VisibleImageLimit);
            QCoreApplication::processEvents();
        }
        manager.release(owner);
        QVERIFY(!scene.render().isNull());
        QVERIFY(scene.item->quads.isEmpty());
        QCoreApplication::processEvents();
        QVERIFY(!scene.render().isNull());
        QVERIFY(scene.item->quads.isEmpty()); // Late results cannot resurrect a released owner.
        QCOMPARE(scene.item->retainedThumbnailBytes(), quint64(0));
    }

    void colorsAlphaAndOrientation_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<qreal>("imageDpr");
        QTest::addColumn<qreal>("targetDpr");
        QTest::newRow("premultiplied")
            << int(QImage::Format_ARGB32_Premultiplied) << qreal(1) << qreal(1);
        QTest::newRow("straight-alpha")
            << int(QImage::Format_RGBA8888) << qreal(1) << qreal(1);
        QTest::newRow("image-dpr-two")
            << int(QImage::Format_ARGB32_Premultiplied) << qreal(2) << qreal(1);
        QTest::newRow("target-dpr-two")
            << int(QImage::Format_RGBA8888) << qreal(1) << qreal(2);
    }

    void colorsAlphaAndOrientation()
    {
        QFETCH(int, format);
        QFETCH(qreal, imageDpr);
        QFETCH(qreal, targetDpr);
        RemoteVideoFrameSource source;
        QImage frame = quadrantImage(QImage::Format(format));
        frame.setDevicePixelRatio(imageDpr);
        source.setFrame(frame);
        Scene scene(targetDpr);
        scene.item->setFrameSource(&source);
        QVERIFY(scene.initialize());
        verifyQuadrants(scene.render());
        QVERIFY(scene.item->hasImageNode);
        QCOMPARE(scene.item->textureSize, frame.size());
#ifdef Q_OS_MACOS
        QCOMPARE(scene.window.rendererInterface()->graphicsApi(), QSGRendererInterface::Metal);
#endif
    }

    void hugeGeometryAndCameraTransformsReuseSourceTexture_data()
    {
        QTest::addColumn<qreal>("targetDpr");
        QTest::newRow("target-dpr-one") << qreal(1);
        QTest::newRow("target-dpr-two") << qreal(2);
    }

    void hugeGeometryAndCameraTransformsReuseSourceTexture()
    {
        QFETCH(qreal, targetDpr);
        RemoteVideoFrameSource source;
        source.setFrame(quadrantImage());
        Scene scene(targetDpr);
        scene.item->setFrameSource(&source);
        QVERIFY(scene.initialize());
        verifyQuadrants(scene.render());
        // Check the bounded allocation before exercising enormous geometry, so
        // a painted-surface regression fails without attempting a huge image.
        QVERIFY(scene.item->hasImageNode);
        QCOMPARE(scene.item->textureSize, source.frame().size());
        const quintptr originalTexture = scene.item->textureIdentity;
        const int originalNodes = scene.item->createdNodes;

        for (qreal width : {4096., 65536., 1000000., 128.}) {
            const QSizeF size(width, width * 0.75);
            scene.item->setSize(size);
            scene.camera->setScale(1);
            scene.item->setScale(1);
            const QImage enlarged = scene.render();
            verifyPixel(enlarged, {16, 12}, Qt::red);
            QCOMPARE(scene.item->imageRect, QRectF(QPointF(), size));
            QCOMPARE(scene.item->textureSize, source.frame().size());
            QCOMPARE(scene.item->textureIdentity, originalTexture);

            // Bring the enormous Alt-resized item back into the viewport with
            // camera zoom, then exercise normal resize through the item's scale.
            scene.camera->setScale(128 / width);
            verifyQuadrants(scene.render());
            QCOMPARE(scene.item->textureIdentity, originalTexture);
            scene.item->setScale(8);
            scene.camera->setScale(16 / width);
            verifyQuadrants(scene.render());
            QCOMPARE(scene.item->textureIdentity, originalTexture);
            QCOMPARE(scene.item->createdNodes, originalNodes);
        }
    }

    void frameReplacementClearingAndSourceDestruction()
    {
        auto source = std::make_unique<RemoteVideoFrameSource>();
        Scene scene;
        QSignalSpy availability(scene.item, &RemoteVideoFrameItem::hasFrameChanged);
        QSignalSpy sourceChanges(scene.item, &RemoteVideoFrameItem::frameSourceChanged);
        scene.item->setFrameSource(source.get());
        QCOMPARE(sourceChanges.count(), 1);
        QCOMPARE(availability.count(), 0);
        QVERIFY(scene.initialize());
        verifyPixel(scene.render(), {32, 24}, QColor(16, 32, 48));

        source->setFrame(quadrantImage());
        QVERIFY(scene.item->hasFrame());
        QCOMPARE(availability.count(), 1);
        verifyQuadrants(scene.render());

        QImage replacement(48, 16, QImage::Format_RGB32);
        replacement.fill(Qt::magenta);
        source->setFrame(replacement);
        QCOMPARE(availability.count(), 1);
        verifyPixel(scene.render(), {32, 24}, Qt::magenta);
        QCOMPARE(scene.item->textureSize, replacement.size());

        source->clear();
        QVERIFY(!scene.item->hasFrame());
        QCOMPARE(availability.count(), 2);
        verifyPixel(scene.render(), {32, 24}, QColor(16, 32, 48));
        QVERIFY(!scene.item->hasImageNode);
        source->setFrame(replacement);
        QCOMPARE(availability.count(), 3);
        verifyPixel(scene.render(), {32, 24}, Qt::magenta);

        source.reset();
        QCOMPARE(scene.item->frameSource(), nullptr);
        QVERIFY(!scene.item->hasFrame());
        QCOMPARE(sourceChanges.count(), 2);
        QCOMPARE(availability.count(), 4);
        verifyPixel(scene.render(), {32, 24}, QColor(16, 32, 48));
        QVERIFY(!scene.item->hasImageNode);
    }

    void oversizedSourceRetainsAllContentAfterGpuDownscale()
    {
        RemoteVideoFrameSource source;
        Scene scene;
        scene.item->setFrameSource(&source);
        QVERIFY(scene.initialize());
        const int maximumSize = scene.control.rhi()->resourceLimit(QRhi::TextureSizeMax);
        QVERIFY(maximumSize > 0);
        // A thin image exceeds the hardware width limit with little memory.
        // Qt downsizes it during upload; the image node must continue to use
        // its entire texture instead of cropping with a stale source rectangle.
        QImage frame(maximumSize * 2 + 1, 8, QImage::Format_RGB32);
        const QRgb colors[] = {qRgb(255, 0, 0), qRgb(0, 255, 0),
                               qRgb(0, 0, 255), qRgb(255, 255, 0)};
        for (int y = 0; y < frame.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(frame.scanLine(y));
            for (int x = 0; x < frame.width(); ++x)
                row[x] = colors[qMin(3, x * 4 / frame.width())];
        }
        source.setFrame(frame);
        const auto verifyBands = [](const QImage& image) {
            verifyPixel(image, {16, 24}, Qt::red);
            verifyPixel(image, {48, 24}, Qt::green);
            verifyPixel(image, {80, 24}, Qt::blue);
            verifyPixel(image, {112, 24}, Qt::yellow);
        };
        verifyBands(scene.render());
        const quintptr originalTexture = scene.item->textureIdentity;
        scene.item->setSize({256, 192});
        scene.camera->setScale(0.5);
        verifyBands(scene.render());
        QVERIFY(scene.item->textureSize.width() <= maximumSize);
        QCOMPARE(scene.item->textureIdentity, originalTexture);
    }

    void unchangedFrameReturnsAfterNodeRecreation()
    {
        RemoteVideoFrameSource source;
        source.setFrame(quadrantImage());
        Scene scene;
        scene.item->setFrameSource(&source);
        QVERIFY(scene.initialize());
        verifyQuadrants(scene.render());
        const int originalNodes = scene.item->createdNodes;
        const qint64 frameKey = source.frame().cacheKey();

        scene.item->setSize({0, 0});
        verifyPixel(scene.render(), {32, 24}, QColor(16, 32, 48));
        QVERIFY(!scene.item->hasImageNode);
        scene.item->setSize({128, 96});
        verifyQuadrants(scene.render());
        QCOMPARE(source.frame().cacheKey(), frameKey);
        QCOMPARE(scene.item->textureSize, source.frame().size());
        QCOMPARE(scene.item->createdNodes, originalNodes + 1);
    }
};

int main(int argc, char** argv)
{
#ifdef Q_OS_MACOS
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#endif
    QGuiApplication application(argc, argv);
    MediaFrameItemTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_MediaFrameItem.moc"

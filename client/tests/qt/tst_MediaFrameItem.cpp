#include "frontend/rendering/remote/RemoteVideoFrameItem.h"
#include "frontend/rendering/canvas/TimelineThumbnailItem.h"
#include "backend/media/MediaResidencyManager.h"

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
    QSet<quintptr> textures;
    QList<QSize> textureSizes;
protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override {
        auto* node = TimelineThumbnailItem::updatePaintNode(oldNode, data);
        quads.clear(); textures.clear(); textureSizes.clear();
        for (auto* child = node ? node->firstChild() : nullptr; child; child = child->nextSibling()) {
            auto* quad = static_cast<QSGImageNode*>(child);
            quads.append(quad->rect());
            textures.insert(reinterpret_cast<quintptr>(quad->texture()));
            textureSizes.append(quad->texture()->textureSize());
        }
        return node;
    }
};

} // namespace

class MediaFrameItemTest final : public QObject
{
    Q_OBJECT

private slots:
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
        QVERIFY(asset->thumbnails.size() > 2);
        Scene<ObservedThumbnailItem> scene;
        scene.item->setSize({240, 44});
        scene.item->setVisibleRight(240);
        scene.item->setOwnerId(owner);
        scene.item->setPixelsPerMs(1);
        scene.item->setSourceInMs(-10000);
        QVERIFY(scene.initialize());
        QVERIFY(!scene.render().isNull());
        QCOMPARE(scene.item->textures.size(), 1); // First-frame hold.
        const auto firstTextures = scene.item->textures;
        scene.item->setSourceInMs(asset->durationUs / 1000.0 + 10000);
        QVERIFY(!scene.render().isNull());
        QCOMPARE(scene.item->textures.size(), 1); // Final-frame hold.
        QVERIFY(scene.item->textures != firstTextures);
        const auto heldTextures = scene.item->textures;
        scene.item->setSourceInMs(asset->durationUs / 1000.0 + 20000);
        QVERIFY(!scene.render().isNull());
        QCOMPARE(scene.item->textures, heldTextures);
        scene.item->setSourceInMs(0);
        scene.item->setPixelsPerMs(240.0 / (asset->durationUs / 1000.0));
        QVERIFY(!scene.render().isNull());
        QVERIFY(scene.item->textures.size() > 1);
        for (const auto& size : scene.item->textureSizes) {
            QVERIFY(size.width() <= MediaThumbnails::Width);
            QVERIFY(size.height() <= MediaThumbnails::Height);
        }
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

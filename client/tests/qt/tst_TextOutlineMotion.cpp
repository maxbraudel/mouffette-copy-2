#include "frontend/rendering/canvas/TextEditHelper.h"
#include "frontend/rendering/canvas/TextOutlineItem.h"

#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>
#include <QtGui/private/qrhi_p.h>
#include <QtQuick/private/qquicktextedit_p.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

class MotionOutline final : public TextOutlineItem
{
public:
    using TextOutlineItem::TextOutlineItem;
    int polishCalls = 0;
    int syncCalls = 0;

protected:
    void updatePolish() override
    {
        ++polishCalls;
        TextOutlineItem::updatePolish();
    }

    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override
    {
        ++syncCalls;
        return TextOutlineItem::updatePaintNode(oldNode, data);
    }
};

struct FrameTimings {
    qint64 totalUs = 0;
    qint64 quickPolishUs = 0;
    qint64 quickSyncUs = 0;
    qint64 renderAndGpuUs = 0;
    qint64 outlinePolishUs = 0;
    qint64 outlineSyncUs = 0;
    int generatedGlyphs = 0;
    int rebuiltChunks = 0;
    int movedChunks = 0;
    int layoutPasses = 0;
    int uploadedGlyphs = 0;
    int polishCalls = 0;
    int syncCalls = 0;
};

qint64 percentile(QList<qint64> samples, double fraction)
{
    std::sort(samples.begin(), samples.end());
    return samples.at(qBound<qsizetype>(0, qsizetype(std::ceil(samples.size() * fraction)) - 1,
                                      samples.size() - 1));
}

void reportFrames(const char* phase, const QList<FrameTimings>& frames)
{
    QList<qint64> total, polish, sync, gpu, outlinePolish, outlineSync;
    int generated = 0;
    int rebuilt = 0;
    int moved = 0;
    int layoutPasses = 0;
    int uploaded = 0;
    int polishCalls = 0;
    int syncCalls = 0;
    for (const FrameTimings& frame : frames) {
        total.append(frame.totalUs);
        polish.append(frame.quickPolishUs);
        sync.append(frame.quickSyncUs);
        gpu.append(frame.renderAndGpuUs);
        outlinePolish.append(frame.outlinePolishUs);
        outlineSync.append(frame.outlineSyncUs);
        generated += frame.generatedGlyphs;
        rebuilt += frame.rebuiltChunks;
        moved += frame.movedChunks;
        layoutPasses += frame.layoutPasses;
        uploaded += frame.uploadedGlyphs;
        polishCalls += frame.polishCalls;
        syncCalls += frame.syncCalls;
    }
    qInfo() << phase << "frame p50/p95/max (us)" << percentile(total, 0.5)
            << percentile(total, 0.95) << percentile(total, 1)
            << "; Quick polish/sync/render+GPU p95 (us)" << percentile(polish, 0.95)
            << percentile(sync, 0.95) << percentile(gpu, 0.95)
            << "; outline polish/sync p95 (us)" << percentile(outlinePolish, 0.95)
            << percentile(outlineSync, 0.95)
            << "; outline polish/sync calls" << polishCalls << syncCalls
            << "; generated/rebuilt/moved totals" << generated << rebuilt << moved
            << "; document layouts/texture uploads" << layoutPasses << uploaded;
}

} // namespace

class TextOutlineMotionTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        const QString fontPath = QStringLiteral(TEST_SOURCE_DIR "/resources/fonts/impact.ttf");
        QVERIFY2(QFontDatabase::addApplicationFont(fontPath) >= 0, qPrintable(fontPath));
    }

    void unchangedParagraphMotion_data()
    {
        QTest::addColumn<bool>("border");
        QTest::addColumn<bool>("cameraPan");
        QTest::addColumn<qreal>("cameraScale");
        QTest::addColumn<qreal>("devicePixelRatio");
        QTest::addColumn<bool>("translucent");
        QTest::addColumn<bool>("liveResize");
        for (const qreal dpr : {qreal(1), qreal(2)}) {
            const QByteArray suffix = dpr == 1 ? QByteArray() : QByteArray("-retina");
            const auto addRow = [&](const char* name, bool border, bool cameraPan,
                                    qreal scale, bool liveResize = false) {
                QTest::newRow((QByteArray(name) + suffix).constData())
                    << border << cameraPan << scale << dpr << false << liveResize;
            };
            addRow("native-camera-pan", false, true, 1);
            addRow("border-camera-pan", true, true, 1);
            addRow("native-element-drag", false, false, 1);
            addRow("border-element-drag", true, false, 1);
            addRow("zoomed-native-camera-pan", false, true, 0.35);
            addRow("zoomed-border-camera-pan", true, true, 0.35);
            addRow("zoomed-native-element-drag", false, false, 0.35);
            addRow("zoomed-border-element-drag", true, false, 0.35);
            addRow("native-alt-resize", false, false, 1, true);
            addRow("border-alt-resize", true, false, 1, true);
        }
        QTest::newRow("zoomed-translucent-camera-pan-retina")
            << true << true << qreal(0.35) << qreal(2) << true << false;
        QTest::newRow("zoomed-translucent-element-drag-retina")
            << true << false << qreal(0.35) << qreal(2) << true << false;
    }

    void unchangedParagraphMotion()
    {
        QFETCH(bool, border);
        QFETCH(bool, cameraPan);
        QFETCH(qreal, cameraScale);
        QFETCH(qreal, devicePixelRatio);
        QFETCH(bool, translucent);
        QFETCH(bool, liveResize);
        QString text;
        const QString phrase = QStringLiteral("MOUFFETTE OUTLINE PERFORMANCE 0123456789 ");
        while (text.size() < 12000)
            text += phrase;
        text.truncate(12000);

        // Resource lifetime mirrors QQuickWidget: the render target must outlive
        // the window/control scene graph, which is invalidated before teardown.
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiRenderBuffer> depthStencil;
        std::unique_ptr<QRhiTextureRenderTarget> renderTarget;
        std::unique_ptr<QRhiRenderPassDescriptor> renderPass;
        QQmlEngine qmlEngine;
        QQuickRenderControl control;
        QQuickWindow window(&control);
        const QSize targetSize(1280, 800);
        const QSize textureSize = targetSize * devicePixelRatio;
        window.setColor(Qt::black);
        window.resize(targetSize);
        window.contentItem()->setSize(targetSize);

        auto* viewport = new QQuickItem(window.contentItem());
        viewport->setSize(targetSize);
        viewport->setClip(true);
        auto* camera = new QQuickItem(viewport);
        camera->setTransformOrigin(QQuickItem::TopLeft);
        camera->setSize(targetSize);
        camera->setScale(cameraScale);
        auto* element = new QQuickItem(camera);
        element->setTransformOrigin(QQuickItem::TopLeft);
        element->setWidth(1200);

        auto* outline = new MotionOutline(element);
        outline->setColor(QColor(QStringLiteral("#cc4040")));
        outline->setOutlinePixels(border ? 48 : 0);
        outline->setZ(0);

        auto* edit = new QQuickTextEdit(element);
        edit->setPosition({100, 60});
        edit->setSize({1000, 100000});
        edit->setZ(1);
        edit->setTextFormat(QQuickTextEdit::PlainText);
        edit->setWrapMode(QQuickTextEdit::Wrap);
        edit->setHAlign(QQuickTextEdit::AlignLeft);
        edit->setVAlign(QQuickTextEdit::AlignTop);
        edit->setReadOnly(true);
        edit->setCursorVisible(false);
        edit->setColor(Qt::white);
        QFont font(QStringLiteral("Impact"));
        font.setPixelSize(48);
        font.setHintingPreference(QFont::PreferNoHinting);
        font.setKerning(true);
        edit->setFont(font);
        edit->setText(text);
        edit->componentComplete();
        TextEditHelper helper;
        helper.applyIncludeTrailingSpaces(edit);
        edit->ensurePolished();
        const qreal documentHeight = edit->contentHeight();
        QVERIFY(documentHeight > targetSize.height() * 3);
        edit->setHeight(documentHeight + 120);
        element->setHeight(documentHeight + 240);
        outline->setSize(element->size());
        outline->setSource(edit);
        camera->setY(-documentHeight * cameraScale / 2);

        // Match production TextItem.qml's alpha composition. The opaque glyph
        // union is rendered once to a cropped source before applying 50% alpha;
        // per-glyph opacity would darken overlapping strokes independently.
        QQmlComponent alphaComponent(&qmlEngine);
        std::unique_ptr<QObject> alphaObject;
        if (translucent) {
            alphaComponent.setData(R"QML(
                import QtQuick
                ShaderEffectSource {
                    required property Item outlineRenderer
                    sourceItem: outlineRenderer
                    hideSource: true
                    sourceRect: outlineRenderer.renderedRect
                    x: sourceRect.x
                    y: sourceRect.y
                    width: sourceRect.width
                    height: sourceRect.height
                    textureSize: outlineRenderer.renderedPixelSize
                    opacity: 0.5
                    smooth: true
                }
            )QML", QUrl::fromLocalFile(QStringLiteral(TEST_SOURCE_DIR
                "/tests/qt/inline-motion-alpha.qml")));
            QTRY_VERIFY_WITH_TIMEOUT(alphaComponent.status() != QQmlComponent::Loading, 5000);
            QVERIFY2(alphaComponent.isReady(), qPrintable(alphaComponent.errorString()));
            // Attach the effect to its window before assigning an already
            // attached source. Assigning the source first makes Qt 6.11 add
            // an extra source-window reference again when the effect is
            // subsequently parented, which leaks a reference at teardown.
            alphaObject.reset(alphaComponent.beginCreate(qmlEngine.rootContext()));
            QVERIFY2(alphaObject, qPrintable(alphaComponent.errorString()));
            auto* alphaItem = qobject_cast<QQuickItem*>(alphaObject.get());
            QVERIFY(alphaItem);
            alphaItem->setParentItem(element);
            alphaItem->setZ(0.5);
            alphaComponent.setInitialProperties(alphaObject.get(), {
                {QStringLiteral("outlineRenderer"),
                 QVariant::fromValue(static_cast<QQuickItem*>(outline))}
            });
            alphaComponent.completeCreate();
            QVERIFY2(!alphaComponent.isError(), qPrintable(alphaComponent.errorString()));
        }

        QVERIFY2(control.initialize(), "QQuickRenderControl initialization failed");
        QRhi* rhi = control.rhi();
        QVERIFY(rhi);
#ifdef Q_OS_MACOS
        QCOMPARE(window.rendererInterface()->graphicsApi(), QSGRendererInterface::Metal);
#endif
        texture.reset(rhi->newTexture(QRhiTexture::RGBA8, textureSize, 1,
                                      QRhiTexture::RenderTarget
                                          | QRhiTexture::UsedAsTransferSource));
        QVERIFY(texture && texture->create());
        depthStencil.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil,
                                                textureSize, 1));
        QVERIFY(depthStencil && depthStencil->create());
        QRhiTextureRenderTargetDescription targetDescription(
            QRhiColorAttachment(texture.get()));
        targetDescription.setDepthStencilBuffer(depthStencil.get());
        renderTarget.reset(rhi->newTextureRenderTarget(targetDescription));
        QVERIFY(renderTarget);
        renderPass.reset(renderTarget->newCompatibleRenderPassDescriptor());
        QVERIFY(renderPass);
        renderTarget->setRenderPassDescriptor(renderPass.get());
        QVERIFY(renderTarget->create());
        auto quickTarget = QQuickRenderTarget::fromRhiRenderTarget(renderTarget.get());
        quickTarget.setDevicePixelRatio(devicePixelRatio);
        window.setRenderTarget(quickTarget);
        QCOMPARE(window.effectiveDevicePixelRatio(), devicePixelRatio);

        const auto renderFrame = [&](QRhiReadbackResult* readback = nullptr) {
            FrameTimings frame;
            QElapsedTimer totalTimer;
            QElapsedTimer phaseTimer;
            const int polishBefore = outline->polishCalls;
            const int syncBefore = outline->syncCalls;
            totalTimer.start();
            phaseTimer.start();
            control.polishItems();
            frame.quickPolishUs = phaseTimer.nsecsElapsed() / 1000;
            control.beginFrame();
            phaseTimer.restart();
            control.sync();
            frame.quickSyncUs = phaseTimer.nsecsElapsed() / 1000;
            phaseTimer.restart();
            control.render();
            if (readback) {
                QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
                updates->readBackTexture(QRhiReadbackDescription(texture.get()), readback);
                control.commandBuffer()->resourceUpdate(updates);
            }
            control.endFrame();
            rhi->finish();
            frame.renderAndGpuUs = phaseTimer.nsecsElapsed() / 1000;
            frame.totalUs = totalTimer.nsecsElapsed() / 1000;
            frame.polishCalls = outline->polishCalls - polishBefore;
            frame.syncCalls = outline->syncCalls - syncBefore;
            const auto stats = outline->statistics();
            // Statistics describe the last executed callback, not necessarily
            // this frame. A transform-only frame must report zero actual work.
            if (frame.polishCalls) {
                frame.outlinePolishUs = stats.polishNanoseconds / 1000;
                frame.generatedGlyphs = stats.generatedGlyphs;
                frame.layoutPasses = stats.layoutPasses;
            }
            if (frame.syncCalls) {
                frame.outlineSyncUs = stats.syncNanoseconds / 1000;
                frame.rebuiltChunks = stats.rebuiltChunks;
                frame.movedChunks = stats.movedChunks;
                frame.uploadedGlyphs = stats.uploadedGlyphs;
            }
            return frame;
        };

        renderFrame();
        if (border) {
            QVERIFY(outline->statistics().glyphs > 300);
            QVERIFY(outline->statistics().glyphs < 1200 / cameraScale);
            QCOMPARE(outline->statistics().triangles,
                     qint64(outline->statistics().glyphs) * 2);
        }
        qInfo() << "fixture glyphs/chunks/triangles" << outline->statistics().glyphs
                << outline->statistics().chunks << outline->statistics().triangles;
        for (int i = 0; i < 4; ++i) {
            QCoreApplication::processEvents();
            renderFrame();
        }

        if (translucent) {
            // Validate that this really exercises an alpha-composited border,
            // then restore the native white fill before timing. Readback is
            // explicitly excluded from every performance sample below.
            edit->setColor(Qt::transparent);
            QRhiReadbackResult readback;
            renderFrame(&readback);
            QVERIFY(!readback.data.isEmpty());
            const QImage captured(reinterpret_cast<const uchar*>(readback.data.constData()),
                                  readback.pixelSize.width(), readback.pixelSize.height(),
                                  QImage::Format_RGBA8888_Premultiplied);
            const QImage image = captured.convertToFormat(QImage::Format_ARGB32);
            int maximumRed = 0;
            int redPixels = 0;
            for (int y = 0; y < image.height(); ++y) {
                const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
                for (int x = 0; x < image.width(); ++x) {
                    maximumRed = qMax(maximumRed, qRed(row[x]));
                    redPixels += qRed(row[x]) > 90;
                }
            }
            QVERIFY2(maximumRed >= 100 && maximumRed <= 104,
                     qPrintable(QStringLiteral("50% alpha mask peak red was %1, expected 102")
                         .arg(maximumRed)));
            QVERIFY(redPixels > 1000);
            edit->setColor(Qt::white);
            renderFrame();
        }

        QList<FrameTimings> stationaryFrames;
        for (int i = 0; i < 12; ++i) {
            QCoreApplication::processEvents();
            stationaryFrames.append(renderFrame());
        }
        reportFrames("stationary", stationaryFrames);

        QQuickItem* movingItem = cameraPan ? camera : element;
        const QPointF initialPosition = movingItem->position();
        const qreal initialElementWidth = element->width();
        const qreal motionScale = cameraPan ? 1 : cameraScale;
        QList<FrameTimings> movingFrames;
        int totalRebuiltChunks = 0;
        int totalMovedChunks = 0;
        for (int i = 0; i < 24; ++i) {
            QCoreApplication::processEvents();
            QElapsedTimer motionTimer;
            motionTimer.start();
            if (liveResize) {
                // Mirror horizontal Alt-resize: the container, editor wrap
                // width and outline viewport all change before the next frame.
                const qreal width = initialElementWidth - (i + 1) * 12;
                element->setWidth(width);
                edit->setWidth(width - 200);
                outline->setWidth(width);
            } else {
                // Fractional diagonal motion crosses both glyph and line culling
                // boundaries without changing text, size or local layout.
                movingItem->setPosition(initialPosition + QPointF((i + 1) * 1.25,
                                                                   -(i + 1) * 2.5) / motionScale);
            }
            FrameTimings frame = renderFrame();
            frame.totalUs = motionTimer.nsecsElapsed() / 1000;
            movingFrames.append(frame);
            totalRebuiltChunks += frame.rebuiltChunks;
            totalMovedChunks += frame.movedChunks;
            QCOMPARE(edit->text(), text);
            QCOMPARE(frame.generatedGlyphs, 0);
            if (!liveResize) {
                QCOMPARE(frame.layoutPasses, 0);
                QCOMPARE(frame.rebuiltChunks, 0);
                QCOMPARE(frame.movedChunks, 0);
            } else if (border) {
                // Prove this is exercising real live wrap/reflow rather than a
                // transform-only shortcut.
                QCOMPARE(frame.layoutPasses, 1);
            }
            QCOMPARE(frame.uploadedGlyphs, 0);
            if (!border) {
                // A disabled renderer must not observe inherited camera or
                // element transforms. One empty sync per TextItem is enough to
                // make a text-heavy canvas stutter.
                QCOMPARE(frame.polishCalls, 0);
                QCOMPARE(frame.syncCalls, 0);
            }
        }
        reportFrames(liveResize ? "alt-resize"
                                : (cameraPan ? "camera-pan" : "element-drag"), movingFrames);

        // A generous machine-independent safety gate; exact sub-frame timing
        // belongs in the reported benchmark, not in a load-sensitive CI test.
        QList<qint64> totalTimes;
        for (const FrameTimings& frame : movingFrames)
            totalTimes.append(frame.totalUs);
        QVERIFY2(percentile(totalTimes, 0.95) < 50000,
                 "Paragraph interaction regressed beyond a 50 ms frame");

        if (liveResize) {
            if (border) {
                // A resize legitimately moves many glyph quads as wrapping
                // changes, but it must recycle the established QSG subtree.
                // The former implementation rebuilt 17 chunks in this exact
                // sequence; at most four edge/visibility additions are allowed.
                QVERIFY2(totalMovedChunks > 0,
                         "Alt-resize did not update retained outline geometry");
                QVERIFY2(totalRebuiltChunks <= 4,
                         qPrintable(QStringLiteral("Alt-resize rebuilt %1 chunks instead of recycling them")
                             .arg(totalRebuiltChunks)));
            }
            window.setRenderTarget({});
            control.invalidate();
            return;
        }

        // Also cross the 96-screen-pixel cache guard several times. Measuring
        // only a short cached drag could conceal expensive viewport refreshes.
        const QPointF longMotionStart = movingItem->position();
        QList<FrameTimings> crossingFrames;
        int refreshes = 0;
        totalTimes.clear();
        for (int i = 0; i < 24; ++i) {
            QCoreApplication::processEvents();
            QElapsedTimer motionTimer;
            motionTimer.start();
            movingItem->setPosition(longMotionStart + QPointF((i + 1) * 7.5,
                                                                -(i + 1) * 20) / motionScale);
            FrameTimings frame = renderFrame();
            frame.totalUs = motionTimer.nsecsElapsed() / 1000;
            crossingFrames.append(frame);
            totalTimes.append(frame.totalUs);
            refreshes += frame.layoutPasses;
            QCOMPARE(edit->text(), text);
            QCOMPARE(frame.generatedGlyphs, 0);
            if (!border) {
                QCOMPARE(frame.polishCalls, 0);
                QCOMPARE(frame.syncCalls, 0);
            }
        }
        reportFrames(cameraPan ? "camera-pan-crossing-cache" : "element-drag-crossing-cache",
                     crossingFrames);
        if (border)
            QVERIFY2(refreshes >= 3, "Long motion did not exercise multiple viewport refreshes");
        QVERIFY2(percentile(totalTimes, 0.95) < 50000,
                 "Crossing the viewport guard regressed beyond a 50 ms frame");

        if (alphaObject) {
            // Release the source item's extra window reference while the
            // offscreen window and its scene graph are both still alive.
            alphaObject->setProperty("sourceItem", QVariant::fromValue<QQuickItem*>(nullptr));
            alphaObject.reset();
            renderFrame();
        }
        window.setRenderTarget({});
        control.invalidate();
    }
};

int main(int argc, char** argv)
{
#ifdef Q_OS_MACOS
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#endif
    QGuiApplication application(argc, argv);
    TextOutlineMotionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_TextOutlineMotion.moc"

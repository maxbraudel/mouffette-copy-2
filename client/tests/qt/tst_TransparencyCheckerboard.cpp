#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>
#include <QtGui/private/qrhi_p.h>

#include <cmath>
#include <memory>

namespace {
// Exercise the real shader and GPU readback without native window/focus timing.
struct Scene {
    std::unique_ptr<QRhiTexture> texture;
    std::unique_ptr<QRhiRenderBuffer> depthStencil;
    std::unique_ptr<QRhiTextureRenderTarget> target;
    std::unique_ptr<QRhiRenderPassDescriptor> pass;
    QQmlEngine engine;
    QQuickRenderControl control;
    QQuickWindow window{&control};
    QQuickItem* media = new QQuickItem(window.contentItem());
    std::unique_ptr<QObject> object;
    QQuickItem* checker = nullptr;
    const qreal dpr;
    QString error;

    explicit Scene(qreal ratio) : dpr(ratio)
    {
        window.setColor(QColor("#102030"));
        window.resize(256, 192);
        window.contentItem()->setSize(window.size());
        media->setTransformOrigin(QQuickItem::TopLeft);
    }

    ~Scene()
    {
        object.reset();
        window.setRenderTarget({});
        control.invalidate();
        target.reset();
        pass.reset();
        depthStencil.reset();
        texture.reset();
    }

    bool initialize(QColor a, QColor b)
    {
        QQmlComponent component(&engine, QUrl::fromLocalFile(
            TEST_SOURCE_DIR "/resources/qml/TransparencyCheckerboard.qml"));
        object.reset(component.createWithInitialProperties({
            {"viewportRect", QRectF(16, 24, 176, 128)}, {"colorA", a}, {"colorB", b}}));
        checker = qobject_cast<QQuickItem*>(object.get());
        if (!checker) { error = component.errorString(); return false; }
        checker->setParentItem(media);
        checker->setTransformOrigin(QQuickItem::TopLeft);
        if (!control.initialize() || !control.rhi()) return false;
        auto* rhi = control.rhi();
        const QSize size = window.size() * dpr;
        texture.reset(rhi->newTexture(QRhiTexture::RGBA8, size, 1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        depthStencil.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, 1));
        if (!texture->create() || !depthStencil->create()) return false;
        QRhiTextureRenderTargetDescription description{QRhiColorAttachment(texture.get())};
        description.setDepthStencilBuffer(depthStencil.get());
        target.reset(rhi->newTextureRenderTarget(description));
        pass.reset(target->newCompatibleRenderPassDescriptor());
        target->setRenderPassDescriptor(pass.get());
        if (!target->create()) return false;
        auto quickTarget = QQuickRenderTarget::fromRhiRenderTarget(target.get());
        quickTarget.setDevicePixelRatio(dpr);
        window.setRenderTarget(quickTarget);
        return true;
    }

    void place(QRectF visibleRect, qreal scale, QPointF mediaPosition)
    {
        media->setPosition(mediaPosition);
        media->setScale(scale);
        checker->setProperty("viewportRect", visibleRect);
        checker->setPosition((visibleRect.topLeft() - mediaPosition) / scale);
        checker->setScale(1 / scale);
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
        if (result.data.isEmpty()) return {};
        QImage image(reinterpret_cast<const uchar*>(result.data.constData()),
                     result.pixelSize.width(), result.pixelSize.height(),
                     QImage::Format_RGBA8888_Premultiplied);
        image = image.copy();
        if (control.rhi()->isYUpInFramebuffer()) image.flip(Qt::Vertical);
        image.setDevicePixelRatio(dpr);
        return image;
    }
};

void verifyGrid(const QImage& image, QRectF rect, QColor a, QColor b, qreal opacity = 1)
{
    QVERIFY(!image.isNull());
    const qreal dpr = image.devicePixelRatio();
    const QColor background("#102030");
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QPointF p((x + 0.5) / dpr, (y + 0.5) / dpr);
            // Fractional rectangle edges can land on a raster sample; only
            // test unambiguous coverage. Every internal checker edge is tested.
            if (qAbs(p.x() - rect.left()) < 0.1 || qAbs(p.x() - rect.right()) < 0.1
                || qAbs(p.y() - rect.top()) < 0.1 || qAbs(p.y() - rect.bottom()) < 0.1)
                continue;
            QColor expected = background;
            if (rect.contains(p) && !rect.isEmpty()) {
                const int parity = (int(std::floor(p.x() / 8)) + int(std::floor(p.y() / 8))) % 2;
                const QColor cell = parity ? b : a;
                expected = QColor(qRound(cell.red() * opacity + background.red() * (1 - opacity)),
                                  qRound(cell.green() * opacity + background.green() * (1 - opacity)),
                                  qRound(cell.blue() * opacity + background.blue() * (1 - opacity)));
            }
            const QColor actual = image.pixelColor(x, y);
            QVERIFY2(qAbs(actual.red() - expected.red()) <= 1
                         && qAbs(actual.green() - expected.green()) <= 1
                         && qAbs(actual.blue() - expected.blue()) <= 1
                         && actual.alpha() == 255,
                     qPrintable(QString("pixel %1,%2: %3 expected %4")
                         .arg(x).arg(y).arg(actual.name(), expected.name())));
        }
    }
}
}

class TransparencyCheckerboardTest : public QObject {
    Q_OBJECT
private slots:
    void viewportGrid_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::addColumn<QColor>("a");
        QTest::addColumn<QColor>("b");
        QTest::newRow("dark-1x") << qreal(1) << QColor("#383838") << QColor("#484848");
        QTest::newRow("dark-2x") << qreal(2) << QColor("#383838") << QColor("#484848");
        QTest::newRow("light-1x") << qreal(1) << QColor("#D8D8D8") << QColor("#ECECEC");
        QTest::newRow("light-2x") << qreal(2) << QColor("#D8D8D8") << QColor("#ECECEC");
    }

    void viewportGrid()
    {
        QFETCH(qreal, dpr);
        QFETCH(QColor, a);
        QFETCH(QColor, b);
        Scene scene(dpr);
        QVERIFY2(scene.initialize(a, b), qPrintable(scene.error));
        for (qreal scale : {0.125, 1.0, 1.75, 128.0}) {
            for (const QRectF rect : {QRectF(16, 24, 176, 128), QRectF(13.25, 19.5, 182.5, 129),
                                      QRectF(0, 0, 256, 192), QRectF(231, 167, 25, 25)}) {
                scene.place(rect, scale, {-312.5, -250});
                verifyGrid(scene.render(), rect, a, b);
            }
        }
        // Changes to the surrounding UI opacity must still blend normally.
        const QRectF rect(16, 24, 176, 128);
        scene.place(rect, 1, {});
        scene.media->setOpacity(0.5);
        verifyGrid(scene.render(), rect, a, b, 0.5);
    }

    void emptyBoundsAndThemeChange()
    {
        Scene scene(1);
        QVERIFY2(scene.initialize(QColor("#383838"), QColor("#484848")), qPrintable(scene.error));
        scene.place({0, 0, 0, 0}, 1, {});
        verifyGrid(scene.render(), {}, {}, {});
        const QRectF rect(8, 8, 192, 128);
        scene.checker->setProperty("colorA", QColor("#D8D8D8"));
        scene.checker->setProperty("colorB", QColor("#ECECEC"));
        scene.place(rect, 2, {});
        verifyGrid(scene.render(), rect, QColor("#D8D8D8"), QColor("#ECECEC"));
    }
};

int main(int argc, char** argv)
{
#ifdef Q_OS_MACOS
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#endif
    QGuiApplication application(argc, argv);
    TransparencyCheckerboardTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_TransparencyCheckerboard.moc"

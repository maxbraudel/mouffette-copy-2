#include <QApplication>
#include <QImage>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QtTest>

#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"

namespace {
struct Fixture {
    CanvasDocument document;
    QuickCanvasController controller{&document};
    QQuickView view;

    bool initialize()
    {
        QString error;
        if (!controller.initialize(&error)) return false;
        view.resize(1000, 700);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.setSource(QUrl(QStringLiteral(
            "qrc:/qt/qml/Mouffette/App/resources/qml/CanvasRoot.qml")));
        if (view.status() != QQuickView::Ready || !view.rootObject()) return false;
        view.rootObject()->setProperty("sessionViewModel", QVariantMap{
            {QStringLiteral("canvasController"),
             QVariant::fromValue<QObject*>(&controller)}});
        return true;
    }

    QVariantMap projected(const QString& id) const
    {
        MediaListModel* model = controller.mediaListModel();
        for (int row = 0; row < model->rowCount(); ++row) {
            const QVariantMap value = model->data(
                model->index(row), MediaListModel::ModelDataRole).toMap();
            if (value.value(QStringLiteral("mediaId")).toString() == id) return value;
        }
        return {};
    }
};

bool invokeSelect(QuickCanvasController& controller, const QString& id,
                  bool additive = false)
{
    return QMetaObject::invokeMethod(&controller, "handleMediaSelectRequested",
        Qt::DirectConnection, Q_ARG(QString, id), Q_ARG(bool, additive));
}
}

class CanvasSelectionBackendTest final : public QObject
{
    Q_OBJECT

private slots:
    void documentSelectionIsTheSingleAuthority()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        CanvasMedia* first = fixture.document.addText({100, 100}, QStringLiteral("First"));
        CanvasMedia* second = fixture.document.addText({100, 400}, QStringLiteral("Second"));
        QVERIFY(first && second);
        QCOMPARE(fixture.controller.mediaListModel()->rowCount(), 2);

        QVERIFY(invokeSelect(fixture.controller, first->mediaId()));
        QVERIFY(first->selected());
        QVERIFY(!second->selected());
        QVERIFY(invokeSelect(fixture.controller, second->mediaId(), true));
        QCOMPARE(fixture.document.selectedMediaIds().size(), 2);

        fixture.document.select(second->mediaId());
        QVERIFY(!first->selected());
        QVERIFY(second->selected());
        QCOMPARE(fixture.controller.selectedMediaItem(), second);
    }

    void textChangesAndLateDeletedIdsAreSafe()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        CanvasMedia* first = fixture.document.addText({10, 20});
        CanvasMedia* survivor = fixture.document.addText({30, 40});
        const QString firstId = first->mediaId();
        const QString survivorId = survivor->mediaId();
        fixture.document.select(survivorId);

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleTextCommitRequested", Qt::DirectConnection,
            Q_ARG(QString, firstId), Q_ARG(QString, QStringLiteral("Updated"))));
        QCOMPARE(first->text(), QStringLiteral("Updated"));
        QVERIFY(survivor->selected());

        QVERIFY(fixture.document.removeMedia(firstId));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(invokeSelect(fixture.controller, firstId));
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleTextCommitRequested", Qt::DirectConnection,
            Q_ARG(QString, firstId), Q_ARG(QString, QStringLiteral("stale"))));
        QCOMPARE(fixture.document.selectedMediaIds(), QStringList{survivorId});
    }

    void fitToTextIsDefaultTracksContentAndToggleRefits()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        const QPointF creationPoint(420, 310);
        CanvasMedia* media = fixture.document.addText(
            creationPoint, QStringLiteral("Text"));
        QVERIFY(media);
        QVERIFY(media->fitToTextEnabled());
        QVERIFY(media->baseSize().width() < 400);
        QVERIFY(media->baseSize().height() < 200);
        QVERIFY(qAbs(media->sceneRect().center().x() - creationPoint.x()) < 0.01);
        QVERIFY(qAbs(media->sceneRect().center().y() - creationPoint.y()) < 0.01);

        const QSize initialSize = media->baseSize();
        const QPointF anchoredCenter = media->sceneRect().center();
        media->setText(QStringLiteral("A much longer fitted text value"));
        QVERIFY(media->baseSize().width() > initialSize.width());
        QVERIFY(qAbs(media->sceneRect().center().x() - anchoredCenter.x()) < 0.01);
        QVERIFY(qAbs(media->sceneRect().center().y() - anchoredCenter.y()) < 0.01);

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleOverlayFitToTextToggle", Qt::DirectConnection,
            Q_ARG(QString, media->mediaId())));
        QVERIFY(!media->fitToTextEnabled());
        media->setBaseSize(QSize(310, 170));
        media->setText(QStringLiteral("X"));
        QCOMPARE(media->baseSize(), QSize(310, 170));

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleOverlayFitToTextToggle", Qt::DirectConnection,
            Q_ARG(QString, media->mediaId())));
        QVERIFY(media->fitToTextEnabled());
        QVERIFY(media->baseSize().width() < 310);
        QVERIFY(media->baseSize().height() < 170);
    }

    void uniformAndFreeResizeCommitToDocument()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        CanvasMedia* media = fixture.document.addText({100, 100});
        media->setFitToTextEnabled(false);
        media->setBaseSize({400, 200});
        media->setPosition({100, 100});
        const QString id = media->mediaId();

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaResizeRequested", Qt::DirectConnection,
            Q_ARG(QString, id), Q_ARG(QString, QStringLiteral("bottom-right")),
            Q_ARG(double, 900.0), Q_ARG(double, 500.0),
            Q_ARG(bool, false), Q_ARG(bool, false)));
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaResizeEnded", Qt::DirectConnection, Q_ARG(QString, id)));
        QCOMPARE(media->scale(), 2.0);
        QCOMPARE(media->sceneRect().size(), QSizeF(800, 400));

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaResizeRequested", Qt::DirectConnection,
            Q_ARG(QString, id), Q_ARG(QString, QStringLiteral("bottom-right")),
            Q_ARG(double, 1000.0), Q_ARG(double, 750.0),
            Q_ARG(bool, false), Q_ARG(bool, true)));
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaResizeEnded", Qt::DirectConnection, Q_ARG(QString, id)));
        QCOMPARE(media->scale(), 2.0);
        QCOMPARE(media->baseSize(), QSize(450, 325));
        QCOMPARE(media->sceneRect().size(), QSizeF(900, 650));
    }

    void altResizeDisablesFitAndPreservesExistingTextScale()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        CanvasMedia* media = fixture.document.addText(
            {300, 200}, QStringLiteral("Scaled text"));
        QVERIFY(media);
        QVERIFY(media->fitToTextEnabled());
        media->setScale(2.25);
        const qreal scaleBefore = media->scale();
        const QRectF rectBefore = media->sceneRect();
        const QString id = media->mediaId();

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaResizeRequested", Qt::DirectConnection,
            Q_ARG(QString, id), Q_ARG(QString, QStringLiteral("right-mid")),
            Q_ARG(double, rectBefore.right() + 180.0),
            Q_ARG(double, rectBefore.center().y()),
            Q_ARG(bool, false), Q_ARG(bool, true)));

        QVERIFY(!media->fitToTextEnabled());
        QCOMPARE(fixture.controller.liveAltResizeScale(), scaleBefore);
        QCOMPARE(fixture.controller.liveAltResizeWidth(),
                 (rectBefore.width() + 180.0) / scaleBefore);

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaResizeEnded", Qt::DirectConnection, Q_ARG(QString, id)));
        QCOMPARE(media->scale(), scaleBefore);
        QCOMPARE(media->sceneRect().height(), rectBefore.height());
        QVERIFY(qAbs(media->sceneRect().width()
                     - (rectBefore.width() + 180.0)) <= scaleBefore / 2.0);
    }

    void snapAndDropImportUseDocumentCoordinates()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        fixture.document.setScreens({ScreenInfo(0, 1920, 1080, 0, 0, true)});
        CanvasMedia* media = fixture.document.addText({100, 100});
        const QString id = media->mediaId();

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaMoveStarted", Qt::DirectConnection,
            Q_ARG(QString, id), Q_ARG(double, 100.0), Q_ARG(double, 100.0),
            Q_ARG(bool, true)));
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaMoveUpdated", Qt::DirectConnection,
            Q_ARG(QString, id), Q_ARG(double, 2.0), Q_ARG(double, 3.0),
            Q_ARG(bool, true)));
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaMoveEnded", Qt::DirectConnection,
            Q_ARG(QString, id), Q_ARG(double, 2.0), Q_ARG(double, 3.0),
            Q_ARG(bool, true)));
        QCOMPARE(media->position(), QPointF(0, 0));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString imagePath = directory.filePath(QStringLiteral("drop.png"));
        QImage image(80, 60, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::cyan);
        QVERIFY(image.save(imagePath));
        const int before = fixture.document.media().size();
        QVERIFY(fixture.controller.beginLocalFileDrag(
            {QUrl::fromLocalFile(imagePath)}, 500, 300));
        QVERIFY(fixture.controller.commitLocalFileDrop(520, 320));
        QCOMPARE(fixture.document.media().size(), before + 1);
        CanvasMedia* imported = fixture.document.selectedMedia();
        QVERIFY(imported && !imported->isText());
        QCOMPARE(imported->baseSize(), QSize(80, 60));
        QCOMPARE(imported->sceneRect().center(), QPointF(520, 320));
    }
};

QTEST_MAIN(CanvasSelectionBackendTest)
#include "tst_CanvasSelectionBackend.moc"

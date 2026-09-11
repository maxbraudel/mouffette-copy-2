#include <QApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QImage>
#include <QImageIOHandler>
#include <QImageReader>
#include <QImageWriter>
#include <QJSValue>
#include <QMimeData>
#include <QPixmap>
#include <QQuickItem>
#include <QQuickWidget>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QWidget>
#include <QtTest>
#include <cmath>
#include <limits>
#include <memory>

#include "backend/domain/media/MediaItems.h"
#include "backend/domain/media/TextMediaItem.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"

namespace {
QVariantList listProperty(QObject* object, const char* name)
{
    QVariant value = object->property(name);
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        value = value.value<QJSValue>().toVariant();
    return value.toList();
}

QVariantMap mapProperty(QObject* object, const char* name)
{
    QVariant value = object->property(name);
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        value = value.value<QJSValue>().toVariant();
    return value.toMap();
}

QQuickItem* mediaDelegate(QQuickItem* root, const QString& mediaId)
{
    QList<QQuickItem*> pending {root};
    while (!pending.isEmpty()) {
        QQuickItem* item = pending.takeLast();
        if (item->property("currentMediaId").toString() == mediaId)
            return item;
        pending.append(item->childItems());
    }
    return nullptr;
}

// Real controller/QML, without MainWindow, network or upload-cache initialization.
struct CanvasFixture {
    QGraphicsScene scene;
    QuickCanvasController controller;
    // Destroy QQuickWidget/QML before the controller's models and scene.
    QWidget host;
    QObject* root = nullptr;

    QSet<QString> selectedIds() const
    {
        QSet<QString> result;
        for (const auto& value : listProperty(root, "selectionChromeModel"))
            result.insert(value.toMap().value(QStringLiteral("mediaId")).toString());
        return result;
    }

    QVariantMap publishedMedia(const QString& id) const
    {
        for (const auto& value : listProperty(root, "mediaModel")) {
            const auto entry = value.toMap();
            if (entry.value(QStringLiteral("mediaId")).toString() == id)
                return entry;
        }
        return {};
    }

    bool select(const QString& id, bool additive = false)
    {
        return QMetaObject::invokeMethod(&controller, "handleMediaSelectRequested",
            Qt::DirectConnection, Q_ARG(QString, id), Q_ARG(bool, additive));
    }
    bool clear()
    {
        return QMetaObject::invokeMethod(&controller, "handleClearSelectionRequested",
            Qt::DirectConnection);
    }
    bool updateText(const char* command, const QString& id, const QString& text)
    {
        return QMetaObject::invokeMethod(&controller, command, Qt::DirectConnection,
            Q_ARG(QString, id), Q_ARG(QString, text));
    }
    bool resize(const QString& id, const QPointF& point, bool alt = false)
    {
        return QMetaObject::invokeMethod(&controller, "handleMediaResizeRequested",
            Qt::DirectConnection, Q_ARG(QString, id),
            Q_ARG(QString, QStringLiteral("bottom-right")),
            Q_ARG(double, point.x()), Q_ARG(double, point.y()),
            Q_ARG(bool, false), Q_ARG(bool, alt));
    }
    bool endResize(const QString& id)
    {
        return QMetaObject::invokeMethod(&controller, "handleMediaResizeEnded",
            Qt::DirectConnection, Q_ARG(QString, id));
    }
};
}

class CanvasSelectionBackendTest : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        QTest::failOnWarning(QRegularExpression(
            "TypeError|ReferenceError|Binding loop|Cannot assign|QQuickItem::stack"
            "|\\[QuickCanvas\\]\\[InputCoordinator\\]"));
        m_canvas = std::make_unique<CanvasFixture>();
        m_canvas->host.resize(1000, 700);
        QString error;
        QVERIFY2(m_canvas->controller.initialize(&m_canvas->host, &error), qPrintable(error));
        m_canvas->controller.widget()->resize(m_canvas->host.size());
        auto* quick = qobject_cast<QQuickWidget*>(m_canvas->controller.widget());
        QVERIFY(quick);
        QVERIFY(quick->rootObject());
        m_canvas->root = quick->rootObject();
        m_canvas->controller.setMediaScene(&m_canvas->scene);
        m_first = new TextMediaItem(QSize(600, 200), 8, 16, QStringLiteral("First"));
        m_second = new TextMediaItem(QSize(600, 200), 8, 16, QStringLiteral("Second"));
        m_first->setPos(100, 100);
        m_second->setPos(100, 400);
        m_canvas->scene.addItem(m_first);
        m_canvas->scene.addItem(m_second);
        m_firstId = m_first->mediaId();
        m_secondId = m_second->mediaId();
        QTRY_COMPARE(listProperty(m_canvas->root, "mediaModel").size(), 2);
    }

    void cleanup()
    {
        m_canvas.reset();
        m_first = nullptr;
        m_second = nullptr;
    }

    void sceneSelectionIsAuthoritative()
    {
        m_first->setSelected(true);
        QCOMPARE(m_canvas->selectedIds(), QSet<QString>{m_firstId});
        QVERIFY(m_canvas->select(m_secondId, true));
        QCOMPARE(m_canvas->selectedIds(), (QSet<QString>{m_firstId, m_secondId}));
        QVERIFY(m_first->isSelected());
        QVERIFY(m_second->isSelected());
        m_canvas->scene.clearSelection();
        QVERIFY(m_canvas->selectedIds().isEmpty());
        QVERIFY(m_canvas->select(m_firstId));
        QVERIFY(m_canvas->select(m_secondId));
        QCOMPARE(m_canvas->selectedIds(), QSet<QString>{m_secondId});
        QVERIFY(!m_first->isSelected());
        QVERIFY(m_second->isSelected());
    }

    void nativeLeftPressSelectsTextThroughController_data()
    {
        QTest::addColumn<qreal>("borderWidth");
        QTest::newRow("borderless") << qreal(0);
        QTest::newRow("thick-border") << qreal(100);
    }

    void nativeLeftPressSelectsTextThroughController()
    {
        QFETCH(qreal, borderWidth);
        m_first->setTextBorderWidthOverrideEnabled(true);
        m_first->setTextBorderWidth(borderWidth);
        QTRY_COMPARE(m_canvas->publishedMedia(m_firstId)
                         .value("textOutlineWidthPercent").toReal(), borderWidth);

        auto* quick = qobject_cast<QQuickWidget*>(m_canvas->controller.widget());
        QVERIFY(quick);
        m_canvas->host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&m_canvas->host));
        auto* rootItem = qobject_cast<QQuickItem*>(m_canvas->root);
        QVERIFY(rootItem);
        QQuickItem* delegate = nullptr;
        QTRY_VERIFY((delegate = mediaDelegate(rootItem, m_firstId)) != nullptr);
        const QPoint pressPoint = delegate->mapToScene(delegate->boundingRect().center()).toPoint();

        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, pressPoint);
        QCoreApplication::processEvents();
        QVERIFY2(m_first->isSelected(), "Native text press did not reach backend selection");
        QCOMPARE(m_canvas->selectedIds(), QSet<QString>{m_firstId});

        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, pressPoint);
        QCoreApplication::processEvents();
    }

    void lateTextCommitPreservesCurrentSelection()
    {
        QVERIFY(m_canvas->select(m_secondId));
        QVERIFY(m_canvas->updateText("handleTextCommitRequested", m_firstId,
            QStringLiteral("Late first commit")));
        QVERIFY(!m_first->isSelected());
        QVERIFY(m_second->isSelected());
        QCOMPARE(m_canvas->selectedIds(), QSet<QString>{m_secondId});
        QVERIFY(m_canvas->clear());
        QVERIFY(m_canvas->updateText("handleTextCommitRequested", m_firstId,
            QStringLiteral("Commit after deselection")));
        QVERIFY(m_canvas->scene.selectedItems().isEmpty());
        QVERIFY(m_canvas->selectedIds().isEmpty());
        QCOMPARE(m_first->text(), QStringLiteral("Commit after deselection"));
    }

    void selectionPreservesPendingContentPublication()
    {
        QVERIFY(m_canvas->select(m_firstId));
        // Observe the actual timer activated by the next live update, without
        // adding production accessors solely for the test.
        QTest::qWait(60);
        QList<QTimer*> previouslyIdle;
        for (auto* timer : m_canvas->controller.findChildren<QTimer*>()) {
            if (timer->isSingleShot() && timer->interval() == 16 && !timer->isActive())
                previouslyIdle.append(timer);
        }
        const QString text = QStringLiteral("Content and fit geometry survive deselection ").repeated(8);
        m_first->setHighlightEnabled(true);
        m_first->setHighlightColor(Qt::yellow);
        QVERIFY(m_canvas->updateText("handleTextLiveUpdateRequested", m_firstId, text));
        QTimer* publication = nullptr;
        for (auto* timer : previouslyIdle) {
            if (timer->isActive()) {
                QVERIFY2(!publication, "Only the media publication timer should be activated");
                publication = timer;
            }
        }
        QVERIFY(publication);
        // No event-loop turn: selection used to discard precisely this pending work.
        QVERIFY(m_canvas->clear());
        QVERIFY(publication->isActive());
        QVERIFY(m_canvas->select(m_secondId));
        QVERIFY(publication->isActive());
        QTRY_COMPARE(m_canvas->publishedMedia(m_firstId).value("textContent").toString(), text);
        const auto published = m_canvas->publishedMedia(m_firstId);
        QVERIFY(published.value("textHighlightEnabled").toBool());
        QCOMPARE(published.value("width").toReal(), qreal(m_first->baseSizePx().width()));
        QCOMPARE(published.value("height").toReal(), qreal(m_first->baseSizePx().height()));
        QCOMPARE(m_canvas->selectedIds(), QSet<QString>{m_secondId});
    }

    void staleDeletedIdsAreIgnored()
    {
        // Populate the index, then delete without processing publication timers.
        QVERIFY(m_canvas->select(m_firstId));
        QVERIFY(m_canvas->select(m_secondId));
        m_canvas->scene.removeItem(m_first);
        delete m_first;
        m_first = nullptr;
        QVERIFY(m_canvas->select(m_firstId));
        QVERIFY(m_canvas->updateText("handleTextCommitRequested", m_firstId,
            QStringLiteral("Stale deleted commit")));
        QCOMPARE(m_canvas->selectedIds(), QSet<QString>{m_secondId});
        for (int i = 0; i < 32; ++i) {
            auto* transient = new TextMediaItem(QSize(200, 80), 8, 16, QStringLiteral("Transient"));
            m_canvas->scene.addItem(transient);
            const QString id = transient->mediaId();
            QVERIFY(m_canvas->select(id));
            QVERIFY(m_canvas->select(m_secondId));
            m_canvas->scene.removeItem(transient);
            delete transient;
            QVERIFY(m_canvas->select(id));
        }
        QVERIFY(m_second->isSelected());
        QCOMPARE(m_canvas->selectedIds(), QSet<QString>{m_secondId});
    }

    void selectionTargetCanDisappearDuringReplacement()
    {
        QVERIFY(m_canvas->select(m_firstId));
        bool targetDeleted = false;
        QObject listener;
        connect(&m_canvas->scene, &QGraphicsScene::selectionChanged, &listener, [&] {
            if (targetDeleted || !m_canvas->scene.selectedItems().isEmpty())
                return;
            // A synchronous listener removes the target during clearSelection().
            targetDeleted = true;
            m_canvas->scene.removeItem(m_second);
            delete m_second;
            m_second = nullptr;
        });
        QVERIFY(m_canvas->select(m_secondId));
        QVERIFY(targetDeleted);
        QVERIFY(m_canvas->scene.selectedItems().isEmpty());
        QVERIFY(m_canvas->selectedIds().isEmpty());
    }

    void resizeEndAfterTargetRemoval_data()
    {
        QTest::addColumn<bool>("detachScene");
        QTest::addColumn<bool>("alt");
        QTest::newRow("deleted-uniform") << false << false;
        QTest::newRow("detached-uniform") << true << false;
        QTest::newRow("deleted-alt") << false << true;
        QTest::newRow("detached-alt") << true << true;
    }

    void altResizeCommitsLegacyGeometryOnlyOnRelease()
    {
        const QString wrappedText = QStringLiteral("Live bordered text reflow ").repeated(40);
        m_first->setText(wrappedText);
        m_first->setTextBorderWidthOverrideEnabled(true);
        m_first->setTextBorderWidth(100);
        QTRY_COMPARE(m_canvas->publishedMedia(m_firstId)
                         .value("textOutlineWidthPercent").toReal(), qreal(100));
        QTRY_COMPARE(m_canvas->publishedMedia(m_firstId)
                         .value("textContent").toString(), wrappedText);

        const QSize initialSize = m_first->baseSizePx();
        const QPointF initialPos = m_first->scenePos();
        const QPointF initialCorner = initialPos
            + QPointF(initialSize.width(), initialSize.height()) * m_first->scale();

        // First tick captures the pointer offset; the second produces a real
        // non-uniform size change and live text reflow in QML.
        QVERIFY(m_canvas->resize(m_firstId, initialCorner + QPointF(40, 30), true));
        QTRY_VERIFY(m_canvas->root->property("liveAltResizeActive").toBool());
        const qreal capturedWidth = m_canvas->root->property("liveAltResizeWidth").toReal();
        QVERIFY(m_canvas->resize(m_firstId, initialCorner + QPointF(120, 80), true));
        QTRY_VERIFY(m_canvas->root->property("liveAltResizeWidth").toReal()
                    > capturedWidth + 1);

        // The visible delegate owns transient geometry. The hidden QGraphics
        // mirror must not relayout or update its scene index on pointer ticks.
        QCOMPARE(m_first->baseSizePx(), initialSize);
        QCOMPARE(m_first->scenePos(), initialPos);

        QVERIFY(m_canvas->endResize(m_firstId));
        QVERIFY(!m_first->isActivelyResizing());
        QVERIFY(m_first->baseSizePx().width() > initialSize.width());
        QVERIFY(m_first->baseSizePx().height() > initialSize.height());
        const auto published = m_canvas->publishedMedia(m_firstId);
        QCOMPARE(published.value("width").toInt(), m_first->baseSizePx().width());
        QCOMPARE(published.value("height").toInt(), m_first->baseSizePx().height());
    }

    void releasingAltHandsLiveGeometryBackToUniformResize()
    {
        const QSize initialSize = m_first->baseSizePx();
        const QPointF initialCorner = m_first->scenePos()
            + QPointF(initialSize.width(), initialSize.height()) * m_first->scale();

        QVERIFY(m_canvas->resize(m_firstId, initialCorner + QPointF(30, 20), true));
        QTRY_VERIFY(m_canvas->root->property("liveAltResizeActive").toBool());
        QVERIFY(m_canvas->resize(m_firstId, initialCorner + QPointF(100, 70), true));
        QTRY_VERIFY(m_canvas->root->property("liveAltResizeWidth").toReal()
                    > initialSize.width() + 1);

        // Releasing Alt without releasing the mouse commits the staged base
        // size and removes the Alt visual override before uniform scaling takes
        // over. Otherwise the delegate appears frozen on its previous ratio.
        QVERIFY(m_canvas->resize(m_firstId, initialCorner + QPointF(120, 90), false));
        QTRY_VERIFY(m_canvas->root->property("liveResizeActive").toBool());
        QVERIFY(!m_canvas->root->property("liveAltResizeActive").toBool());
        QVERIFY(m_first->baseSizePx() != initialSize);

        QVERIFY(m_canvas->endResize(m_firstId));
        QVERIFY(!m_canvas->root->property("liveResizeActive").toBool());
        QVERIFY(!m_canvas->root->property("liveAltResizeActive").toBool());
    }

    void resizeEndAfterTargetRemoval()
    {
        QFETCH(bool, detachScene);
        QFETCH(bool, alt);
        QVERIFY(m_canvas->select(m_firstId));
        const QPointF initialCorner = m_first->scenePos()
            + QPointF(m_first->baseSizePx().width(), m_first->baseSizePx().height()) * m_first->scale();
        QVERIFY(m_canvas->resize(m_firstId, initialCorner + QPointF(40, 30), alt));
        const char* activeFlag = alt ? "liveAltResizeActive" : "liveResizeActive";
        const char* ownerProperty = alt ? "liveAltResizeMediaId" : "liveResizeMediaId";
        QTRY_VERIFY(m_canvas->root->property(activeFlag).toBool());
        QCOMPARE(m_canvas->root->property(ownerProperty).toString(), m_firstId);
        QVERIFY(m_first->isActivelyResizing());

        // A final pointer update is still queued when the target/scene goes
        // away. Ending must safely drain/discard it and release both kinds of
        // live resize state, even before the next model-publication timer fires.
        QVERIFY(m_canvas->resize(m_firstId, initialCorner + QPointF(80, 50), alt));
        if (detachScene) {
            m_canvas->controller.setMediaScene(nullptr);
        } else {
            m_canvas->scene.removeItem(m_first);
            delete m_first;
            m_first = nullptr;
        }
        QVERIFY(m_canvas->endResize(m_firstId));
        QVERIFY(m_canvas->endResize(m_firstId)); // Native cancel/release can both arrive.
        QVERIFY(!m_canvas->root->property("liveResizeActive").toBool());
        QVERIFY(!m_canvas->root->property("liveAltResizeActive").toBool());
        QVERIFY(m_canvas->root->property("liveResizeMediaId").toString().isEmpty());
        QVERIFY(m_canvas->root->property("liveAltResizeMediaId").toString().isEmpty());
        QVERIFY(listProperty(m_canvas->root, "snapGuidesModel").isEmpty());
        QCOMPARE(m_canvas->root->property("interactionMode").toString(), QStringLiteral("idle"));
        QVERIFY(m_canvas->root->property("interactionOwnerId").toString().isEmpty());
        QTRY_COMPARE(listProperty(m_canvas->root, "mediaModel").size(), detachScene ? 0 : 1);

        if (detachScene) {
            QVERIFY(!m_first->isActivelyResizing());
            m_canvas->controller.setMediaScene(&m_canvas->scene);
            QTRY_COMPARE(listProperty(m_canvas->root, "mediaModel").size(), 2);
        }
        // The public behavior also proves the private backend PointerSession
        // is no longer locked to the missing item: a different owner can resize.
        QVERIFY(m_canvas->select(m_secondId));
        const QPointF secondCorner = m_second->scenePos()
            + QPointF(m_second->baseSizePx().width(), m_second->baseSizePx().height()) * m_second->scale();
        QVERIFY(m_canvas->resize(m_secondId, secondCorner + QPointF(25, 20)));
        QTRY_VERIFY(m_canvas->root->property("liveResizeActive").toBool());
        QCOMPARE(m_canvas->root->property("liveResizeMediaId").toString(), m_secondId);
        QVERIFY(m_canvas->endResize(m_secondId));
        QVERIFY(!m_second->isActivelyResizing());
        QVERIFY(!m_canvas->root->property("liveResizeActive").toBool());
        QVERIFY(!m_canvas->root->property("liveAltResizeActive").toBool());
        QCOMPARE(m_canvas->selectedIds(), QSet<QString>{m_secondId});
    }

    void lateResizeEndPreservesNewOwner_data()
    {
        QTest::addColumn<bool>("secondAlreadyActive");
        QTest::newRow("new-owner-queued") << false;
        QTest::newRow("new-owner-active") << true;
    }

    void lateResizeEndPreservesNewOwner()
    {
        QFETCH(bool, secondAlreadyActive);
        QVERIFY(m_canvas->select(m_firstId));
        const QPointF firstCorner = m_first->sceneBoundingRect().bottomRight();
        QVERIFY(m_canvas->resize(m_firstId, firstCorner + QPointF(25, 20)));
        QTRY_VERIFY(m_canvas->root->property("liveResizeActive").toBool());
        QVERIFY(m_canvas->endResize(m_firstId));
        QVERIFY(m_canvas->select(m_secondId));

        if (secondAlreadyActive) {
            const QPointF secondCorner = m_second->sceneBoundingRect().bottomRight();
            QVERIFY(m_canvas->resize(m_secondId, secondCorner + QPointF(20, 15)));
            QTRY_VERIFY(m_canvas->root->property("liveResizeActive").toBool());
            QCOMPARE(m_canvas->root->property("liveResizeMediaId").toString(), m_secondId);
        }

        const QRectF before = m_second->sceneBoundingRect();
        const QPointF finalCorner = before.topLeft() + QPointF(before.width(), before.height()) * 2;
        QVERIFY(m_canvas->resize(m_secondId, finalCorner));
        QTimer* dispatch = nullptr;
        for (auto* timer : m_canvas->controller.findChildren<QTimer*>()) {
            if (timer->isSingleShot() && timer->interval() == 8 && timer->isActive()) {
                QVERIFY(!dispatch);
                dispatch = timer;
            }
        }
        QVERIFY(dispatch);

        // A delayed release from A must not cancel B's pending first update,
        // nor the next update of an already-active B session.
        QVERIFY(m_canvas->endResize(m_firstId));
        QVERIFY2(dispatch->isActive(), "A stale end must not stop another owner's queued resize");
        QTRY_VERIFY(m_second->sceneBoundingRect().width() > before.width() + 1);
        QVERIFY(m_canvas->root->property("liveResizeActive").toBool());
        QCOMPARE(m_canvas->root->property("liveResizeMediaId").toString(), m_secondId);
        QCOMPARE(m_canvas->selectedIds(), QSet<QString>{m_secondId});

        QVERIFY(m_canvas->endResize(m_secondId));
        QVERIFY(!m_canvas->root->property("liveResizeActive").toBool());
        const auto published = m_canvas->publishedMedia(m_secondId);
        const qreal publishedWidth = published.value("width").toReal() * published.value("scale").toReal();
        QVERIFY(qAbs(publishedWidth - m_second->sceneBoundingRect().width()) < 0.01);
        QCOMPARE(published.value("textContent").toString(), QStringLiteral("Second"));
    }

    void localImageDragUsesNativeGeometryAndRestoresCursor()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("image-320.png"));
        QImage source(320, 180, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(QStringLiteral("#ff5a7a")));
        QVERIFY(source.save(path));

        auto* quick = qobject_cast<QQuickWidget*>(m_canvas->controller.widget());
        QVERIFY(quick);
        m_canvas->host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&m_canvas->host));
        m_canvas->root->setProperty("viewScale", 0.5);
        m_canvas->root->setProperty("panX", 40.0);
        m_canvas->root->setProperty("panY", 25.0);

        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        QSignalSpy prepared(&m_canvas->controller,
                            &QuickCanvasController::preparedLocalFileDropRequested);
        connect(&m_canvas->controller,
                &QuickCanvasController::preparedLocalFileDropRequested,
                &m_canvas->controller,
                [this](const QString&, const QSize&, const QImage&, const QPointF&) {
                    m_canvas->controller.beginDropPreviewHandoff(
                        QStringLiteral("prepared-image"));
                });

        QDragEnterEvent enter(QPoint(300, 200), Qt::CopyAction, &mime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(quick, &enter);
        QVERIFY(enter.isAccepted());
        QCOMPARE(quick->cursor().shape(), Qt::BlankCursor);

        QTRY_VERIFY_WITH_TIMEOUT(
            mapProperty(m_canvas->root, "dropPreviewModel").value("visible").toBool(),
            3000);
        QTRY_VERIFY_WITH_TIMEOUT(
            mapProperty(m_canvas->root, "dropPreviewModel").value("frameReady").toBool(),
            3000);
        QVariantMap preview = mapProperty(m_canvas->root, "dropPreviewModel");
        QCOMPARE(preview.value("width").toInt(), 320);
        QCOMPARE(preview.value("height").toInt(), 180);
        QCOMPARE(preview.value("x").toDouble(), 360.0); // ((300-40)/.5) - 160
        QCOMPARE(preview.value("y").toDouble(), 260.0); // ((200-25)/.5) - 90

        auto* previewItem = m_canvas->root->findChild<QQuickItem*>(
            QStringLiteral("mediaDropPreview"));
        auto* previewSurface = m_canvas->root->findChild<QQuickItem*>(
            QStringLiteral("dropPreviewSurface"));
        auto* previewTitle = m_canvas->root->findChild<QQuickItem*>(
            QStringLiteral("dropPreviewTitle"));
        QVERIFY(previewItem);
        QVERIFY(previewSurface);
        QVERIFY(previewTitle);
        QCOMPARE(previewSurface->property("placeholderColor").value<QColor>(),
                 QColor(QStringLiteral("#F2323232")));
        QCOMPARE(previewSurface->property("fadeDuration").toInt(), 80);
        QCOMPARE(previewTitle->height(), 36.0);
        QCOMPARE(previewTitle->y(), 25.0 + 260.0 * 0.5 - 76.0 - 8.0);

        QDragMoveEvent move(QPoint(420, 260), Qt::CopyAction, &mime,
                            Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(quick, &move);
        QVERIFY(move.isAccepted());
        preview = mapProperty(m_canvas->root, "dropPreviewModel");
        QCOMPARE(preview.value("x").toDouble(), 600.0); // ((420-40)/.5) - 160
        QCOMPARE(preview.value("y").toDouble(), 380.0); // ((260-25)/.5) - 90

        QDropEvent drop(QPointF(420, 260), Qt::CopyAction, &mime,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(quick, &drop);
        QVERIFY(drop.isAccepted());
        QTRY_COMPARE(prepared.size(), 1);
        QCOMPARE(prepared.first().at(0).toString(), QFileInfo(path).canonicalFilePath());
        QCOMPARE(prepared.first().at(1).toSize(), QSize(320, 180));
        QCOMPARE(prepared.first().at(3).toPointF(), QPointF(760, 470));
        QCOMPARE(quick->cursor().shape(), Qt::ArrowCursor);
        QCOMPARE(mapProperty(m_canvas->root, "dropPreviewModel")
                     .value("handoffMediaId").toString(),
                 QStringLiteral("prepared-image"));

        QVERIFY(QMetaObject::invokeMethod(&m_canvas->controller,
                                          "handleDropPreviewContentReady",
                                          Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("prepared-image"))));
        // Content readiness only arms the render barrier. The preview and its
        // handoff identity must remain intact until Quick has presented the
        // final texture in completed render passes.
        QCOMPARE(mapProperty(m_canvas->root, "dropPreviewModel")
                     .value("handoffMediaId").toString(),
                 QStringLiteral("prepared-image"));
        QTRY_VERIFY_WITH_TIMEOUT(
            !mapProperty(m_canvas->root, "dropPreviewModel").value("visible").toBool(),
            500);
        // Cleanup is acknowledged by the real QML animation completion, not
        // by an unrelated timer that could race the render thread.
        QTRY_VERIFY_WITH_TIMEOUT(
            mapProperty(m_canvas->root, "dropPreviewModel")
                .value("handoffMediaId").toString().isEmpty(),
            500);

        // The identity-based cache makes a second enter ready synchronously.
        QDragEnterEvent cachedEnter(QPoint(200, 160), Qt::CopyAction, &mime,
                                    Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(quick, &cachedEnter);
        QVERIFY(cachedEnter.isAccepted());
        preview = mapProperty(m_canvas->root, "dropPreviewModel");
        QVERIFY(preview.value("visible").toBool());
        QVERIFY(preview.value("frameReady").toBool());
        QDragLeaveEvent leave;
        QApplication::sendEvent(quick, &leave);
        QVERIFY(leave.isAccepted());
        QCOMPARE(quick->cursor().shape(), Qt::ArrowCursor);

        // Replacing the same path changes its identity and cannot reuse stale
        // geometry or pixels from the previous cache entry.
        QTest::qWait(5);
        QImage replacement(401, 203, QImage::Format_ARGB32_Premultiplied);
        replacement.fill(QColor(QStringLiteral("#27364c")));
        QVERIFY(replacement.save(path));
        QDragEnterEvent invalidatedEnter(QPoint(200, 160), Qt::CopyAction, &mime,
                                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(quick, &invalidatedEnter);
        QVERIFY(invalidatedEnter.isAccepted());
        QTRY_COMPARE(mapProperty(m_canvas->root, "dropPreviewModel")
                         .value("width").toInt(), 401);
        QCOMPARE(mapProperty(m_canvas->root, "dropPreviewModel")
                     .value("height").toInt(), 203);
        QDragLeaveEvent invalidatedLeave;
        QApplication::sendEvent(quick, &invalidatedLeave);
        QVERIFY(invalidatedLeave.isAccepted());

        // Batches are intentionally refused and cannot hide the pointer.
        QMimeData batchMime;
        batchMime.setUrls({QUrl::fromLocalFile(path), QUrl::fromLocalFile(path)});
        QDragEnterEvent batchEnter(QPoint(100, 100), Qt::CopyAction, &batchMime,
                                   Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(quick, &batchEnter);
        QVERIFY(!batchEnter.isAccepted());
        QCOMPARE(quick->cursor().shape(), Qt::ArrowCursor);
    }

    void localVideoDragPublishesExactGeometryBeforeDrop()
    {
        const QString path = QString::fromUtf8(TEST_VIDEO_FILE);
        if (!QFileInfo::exists(path)) {
            QSKIP(qPrintable(QStringLiteral("Optional real-video fixture is missing: %1")
                                 .arg(path)));
        }

        auto* quick = qobject_cast<QQuickWidget*>(m_canvas->controller.widget());
        QVERIFY(quick);
        m_canvas->host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&m_canvas->host));

        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        QSignalSpy prepared(&m_canvas->controller,
                            &QuickCanvasController::preparedLocalFileDropRequested);
        connect(&m_canvas->controller,
                &QuickCanvasController::preparedLocalFileDropRequested,
                &m_canvas->controller,
                [this](const QString&, const QSize&, const QImage&, const QPointF&) {
                    m_canvas->controller.beginDropPreviewHandoff(
                        QStringLiteral("prepared-video"));
                });

        QDragEnterEvent enter(QPoint(500, 350), Qt::CopyAction, &mime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(quick, &enter);
        QVERIFY(enter.isAccepted());
        QCOMPARE(prepared.size(), 0);
        QCOMPARE(quick->cursor().shape(), Qt::BlankCursor);
        QTRY_VERIFY_WITH_TIMEOUT(
            mapProperty(m_canvas->root, "dropPreviewModel").value("visible").toBool(),
            5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            mapProperty(m_canvas->root, "dropPreviewModel").value("frameReady").toBool(),
            5000);
        const QVariantMap preview = mapProperty(m_canvas->root, "dropPreviewModel");
        QCOMPARE(preview.value("phase").toString(), QStringLiteral("ready"));
        QCOMPARE(preview.value("mediaType").toString(), QStringLiteral("video"));
        QCOMPARE(preview.value("width").toInt(), 1920);
        QCOMPARE(preview.value("height").toInt(), 1080);
        QCOMPARE(preview.value("x").toDouble(), -460.0);
        QCOMPARE(preview.value("y").toDouble(), -190.0);

        QDropEvent drop(QPointF(500, 350), Qt::CopyAction, &mime,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(quick, &drop);
        QVERIFY(drop.isAccepted());
        QTRY_COMPARE(prepared.size(), 1);
        QCOMPARE(prepared.first().at(1).toSize(), QSize(1920, 1080));
        QVERIFY(!prepared.first().at(2).value<QImage>().isNull());
        QCOMPARE(prepared.first().at(3).toPointF(), QPointF(500, 350));
        QCOMPARE(quick->cursor().shape(), Qt::ArrowCursor);
        const QVariantMap handoff = mapProperty(m_canvas->root, "dropPreviewModel");
        QCOMPARE(handoff.value("phase").toString(), QStringLiteral("handoff"));
        QCOMPARE(handoff.value("width").toInt(), 1920);
        QCOMPARE(handoff.value("height").toInt(), 1080);
    }

    void imageDropHandoffNeverRendersAnEmptyFrame()
    {
        // Exercise the complete visible path. State-only assertions cannot
        // detect a decoded image that has not reached the Quick render pass.
        m_canvas->scene.clear();
        m_first = nullptr;
        m_second = nullptr;
        QTRY_COMPARE(listProperty(m_canvas->root, "mediaModel").size(), 0);

        auto* quick = qobject_cast<QQuickWidget*>(m_canvas->controller.widget());
        QVERIFY(quick);
        m_canvas->host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&m_canvas->host));
        m_canvas->root->setProperty("viewScale", 1.0);
        m_canvas->root->setProperty("panX", 0.0);
        m_canvas->root->setProperty("panY", 0.0);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("handoff-red.png"));
        QImage source(160, 100, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(245, 36, 64));
        QVERIFY(source.save(path));

        QString insertedId;
        connect(&m_canvas->controller,
                &QuickCanvasController::preparedLocalFileDropRequested,
                &m_canvas->controller,
                [this, &insertedId](const QString& localPath,
                                    const QSize& nativeSize,
                                    const QImage& previewFrame,
                                    const QPointF& sceneCenter) {
                    auto* image = new ResizablePixmapItem(
                        QPixmap::fromImage(previewFrame), nativeSize,
                        12, 30, QFileInfo(localPath).fileName());
                    image->setSourcePath(localPath);
                    image->setPos(sceneCenter - QPointF(nativeSize.width() * 0.5,
                                                        nativeSize.height() * 0.5));
                    image->setZValue(10.0);
                    m_canvas->scene.addItem(image);
                    image->setSelected(true);
                    insertedId = image->mediaId();
                    m_canvas->controller.beginDropPreviewHandoff(insertedId);
                });

        const QPoint dropPoint(420, 320);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        QDragEnterEvent enter(dropPoint, Qt::CopyAction, &mime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(quick, &enter);
        QVERIFY(enter.isAccepted());
        QTRY_VERIFY_WITH_TIMEOUT(
            mapProperty(m_canvas->root, "dropPreviewModel").value("frameReady").toBool(),
            3000);

        const auto centerPixel = [quick, dropPoint]() {
            const QImage frame = quick->grab().toImage();
            const qreal dpr = frame.devicePixelRatio();
            const int x = std::clamp(qRound(dropPoint.x() * dpr), 0, frame.width() - 1);
            const int y = std::clamp(qRound(dropPoint.y() * dpr), 0, frame.height() - 1);
            return frame.pixelColor(x, y);
        };
        QColor initialPixel;
        for (int attempt = 0; attempt < 100; ++attempt) {
            initialPixel = centerPixel();
            if (initialPixel.red() > 220)
                break;
            QTest::qWait(10);
        }
        const QVariantMap initialPreview = mapProperty(m_canvas->root, "dropPreviewModel");
        QVERIFY2(initialPixel.red() > 220,
                 qPrintable(QStringLiteral(
                     "preview center was %1 at drop=(%2,%3), preview=(%4,%5 %6x%7), viewScale=%8 pan=(%9,%10)")
                     .arg(initialPixel.name(QColor::HexArgb))
                     .arg(dropPoint.x()).arg(dropPoint.y())
                     .arg(initialPreview.value("x").toDouble())
                     .arg(initialPreview.value("y").toDouble())
                     .arg(initialPreview.value("width").toDouble())
                     .arg(initialPreview.value("height").toDouble())
                     .arg(m_canvas->root->property("viewScale").toDouble())
                     .arg(m_canvas->root->property("panX").toDouble())
                     .arg(m_canvas->root->property("panY").toDouble())));

        QDropEvent drop(QPointF(dropPoint), Qt::CopyAction, &mime,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(quick, &drop);
        QVERIFY(drop.isAccepted());
        QVERIFY(!insertedId.isEmpty());

        auto* previewVisual = m_canvas->root->findChild<QQuickItem*>(
            QStringLiteral("mediaDropPreview"));
        QVERIFY(previewVisual);
        QQuickItem* selectedChrome = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT([&]() {
            QList<QQuickItem*> pending {
                qobject_cast<QQuickItem*>(m_canvas->root)
            };
            while (!pending.isEmpty()) {
                QQuickItem* candidate = pending.takeLast();
                if (candidate->objectName() == QStringLiteral("selectionChromeVisual")
                    && mapProperty(candidate, "entry").value("mediaId").toString()
                        == insertedId) {
                    selectedChrome = candidate;
                    return true;
                }
                pending.append(candidate->childItems());
            }
            return false;
        }(), 1000);
        QCOMPARE(selectedChrome->parentItem(), previewVisual->parentItem());
        QVERIFY2(selectedChrome->z() > previewVisual->z(),
                 "selection border/handles must render above the drop preview");

        int darkestRed = 255;
        int brightestOtherChannel = 0;
        for (int frameIndex = 0; frameIndex < 45; ++frameIndex) {
            QTest::qWait(4);
            const QColor pixel = centerPixel();
            darkestRed = std::min(darkestRed, pixel.red());
            brightestOtherChannel = std::max(
                brightestOtherChannel, std::max(pixel.green(), pixel.blue()));
        }
        QVERIFY2(darkestRed > 220,
                 qPrintable(QStringLiteral("drop rendered a non-media frame; minimum red=%1")
                                .arg(darkestRed)));
        QVERIFY2(brightestOtherChannel < 100,
                 qPrintable(QStringLiteral("drop exposed placeholder/background; max other=%1")
                                .arg(brightestOtherChannel)));
        QTRY_VERIFY_WITH_TIMEOUT(
            mapProperty(m_canvas->root, "dropPreviewModel")
                .value("handoffMediaId").toString().isEmpty(),
            1000);
        QVERIFY(mediaDelegate(qobject_cast<QQuickItem*>(m_canvas->root), insertedId));
    }

    void videoDropHandoffWaitsForItsVisibleVideoOutput()
    {
        const QString path = QString::fromUtf8(TEST_VIDEO_FILE);
        if (!QFileInfo::exists(path)) {
            QSKIP(qPrintable(QStringLiteral("Optional real-video fixture is missing: %1")
                                 .arg(path)));
        }

        m_canvas->scene.clear();
        m_first = nullptr;
        m_second = nullptr;
        QTRY_COMPARE(listProperty(m_canvas->root, "mediaModel").size(), 0);

        auto* quick = qobject_cast<QQuickWidget*>(m_canvas->controller.widget());
        QVERIFY(quick);
        m_canvas->host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&m_canvas->host));
        m_canvas->root->setProperty("viewScale", 1.0);
        m_canvas->root->setProperty("panX", 0.0);
        m_canvas->root->setProperty("panY", 0.0);

        QString insertedId;
        connect(&m_canvas->controller,
                &QuickCanvasController::preparedLocalFileDropRequested,
                &m_canvas->controller,
                [this, &insertedId](const QString& localPath,
                                    const QSize& nativeSize,
                                    const QImage& previewFrame,
                                    const QPointF& sceneCenter) {
                    auto* video = new ResizableVideoItem(
                        localPath, nativeSize, 12, 30,
                        QFileInfo(localPath).fileName());
                    video->setSourcePath(localPath);
                    video->setExternalPosterImage(previewFrame, nativeSize);
                    video->setPos(sceneCenter - QPointF(nativeSize.width() * 0.5,
                                                        nativeSize.height() * 0.5));
                    video->setZValue(10.0);
                    m_canvas->scene.addItem(video);
                    video->setSelected(true);
                    insertedId = video->mediaId();
                    m_canvas->controller.beginDropPreviewHandoff(insertedId);
                });

        const QPoint dropPoint(500, 350);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        QDragEnterEvent enter(dropPoint, Qt::CopyAction, &mime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(quick, &enter);
        QVERIFY(enter.isAccepted());
        QTRY_VERIFY_WITH_TIMEOUT(
            mapProperty(m_canvas->root, "dropPreviewModel").value("frameReady").toBool(),
            5000);

        const auto widgetFrame = [quick]() { return quick->grab().toImage(); };
        QImage previewImage;
        const QColor canvasColor(QStringLiteral("#10131a"));
        const QColor placeholderColor(QStringLiteral("#323232"));
        const auto colorDistance = [](const QColor& lhs, const QColor& rhs) {
            const int dr = lhs.red() - rhs.red();
            const int dg = lhs.green() - rhs.green();
            const int db = lhs.blue() - rhs.blue();
            return std::sqrt(double(dr * dr + dg * dg + db * db));
        };

        QPoint probe = dropPoint;
        double probeDistance = -1.0;
        for (int attempt = 0; attempt < 200 && probeDistance <= 35.0; ++attempt) {
            previewImage = widgetFrame();
            QVERIFY(!previewImage.isNull());
            const qreal dpr = previewImage.devicePixelRatio();
            for (int y = 100; y <= 600; y += 50) {
                for (int x = 100; x <= 900; x += 50) {
                    const int px = std::clamp(qRound(x * dpr), 0, previewImage.width() - 1);
                    const int py = std::clamp(qRound(y * dpr), 0, previewImage.height() - 1);
                    const QColor candidate = previewImage.pixelColor(px, py);
                    const double distance = std::min(colorDistance(candidate, canvasColor),
                                                     colorDistance(candidate, placeholderColor));
                    if (distance > probeDistance) {
                        probeDistance = distance;
                        probe = QPoint(x, y);
                    }
                }
            }
            if (probeDistance <= 35.0)
                QTest::qWait(10);
        }
        QVERIFY2(probeDistance > 35.0,
                 "The video fixture has no usable probe distinct from canvas/placeholder");

        QDropEvent drop(QPointF(dropPoint), Qt::CopyAction, &mime,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(quick, &drop);
        QVERIFY(drop.isAccepted());
        QVERIFY(!insertedId.isEmpty());

        double closestToEmpty = std::numeric_limits<double>::max();
        for (int frameIndex = 0; frameIndex < 80; ++frameIndex) {
            QTest::qWait(5);
            const QImage frame = widgetFrame();
            const qreal frameDpr = frame.devicePixelRatio();
            const int px = std::clamp(qRound(probe.x() * frameDpr), 0, frame.width() - 1);
            const int py = std::clamp(qRound(probe.y() * frameDpr), 0, frame.height() - 1);
            const QColor pixel = frame.pixelColor(px, py);
            closestToEmpty = std::min(
                closestToEmpty,
                std::min(colorDistance(pixel, canvasColor),
                         colorDistance(pixel, placeholderColor)));
        }
        QVERIFY2(closestToEmpty > 20.0,
                 qPrintable(QStringLiteral(
                     "video handoff exposed canvas/placeholder; closest color distance=%1")
                     .arg(closestToEmpty)));
        QTRY_VERIFY_WITH_TIMEOUT(
            mapProperty(m_canvas->root, "dropPreviewModel")
                .value("handoffMediaId").toString().isEmpty(),
            8000);
        QVERIFY(mediaDelegate(qobject_cast<QQuickItem*>(m_canvas->root), insertedId));
    }

    void localImageDragRespectsStoredOrientation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("oriented.jpg"));
        QImage source(120, 60, QImage::Format_RGB32);
        source.fill(QColor(QStringLiteral("#d59344")));
        QImageWriter writer(path, "jpeg");
        if (!writer.supportsOption(QImageIOHandler::ImageTransformation)) {
            QSKIP("The active JPEG plugin cannot write orientation metadata");
        }
        writer.setTransformation(QImageIOHandler::TransformationRotate90);
        QVERIFY2(writer.write(source), qPrintable(writer.errorString()));

        QImageReader verification(path);
        verification.setAutoTransform(true);
        const QImage decoded = verification.read();
        QVERIFY(!decoded.isNull());
        QCOMPARE(decoded.size(), QSize(60, 120));

        auto* quick = qobject_cast<QQuickWidget*>(m_canvas->controller.widget());
        QVERIFY(quick);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        QDragEnterEvent enter(QPoint(300, 220), Qt::CopyAction, &mime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(quick, &enter);
        QVERIFY(enter.isAccepted());
        QTRY_VERIFY_WITH_TIMEOUT(
            mapProperty(m_canvas->root, "dropPreviewModel").value("frameReady").toBool(),
            3000);
        const QVariantMap preview = mapProperty(m_canvas->root, "dropPreviewModel");
        QCOMPARE(preview.value("width").toInt(), 60);
        QCOMPARE(preview.value("height").toInt(), 120);
        QDragLeaveEvent leave;
        QApplication::sendEvent(quick, &leave);
        QCOMPARE(quick->cursor().shape(), Qt::ArrowCursor);
    }

private:
    std::unique_ptr<CanvasFixture> m_canvas;
    TextMediaItem* m_first = nullptr;
    TextMediaItem* m_second = nullptr;
    QString m_firstId;
    QString m_secondId;
};

QTEST_MAIN(CanvasSelectionBackendTest)
#include "tst_CanvasSelectionBackend.moc"

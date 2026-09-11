#include <QApplication>
#include <QGraphicsScene>
#include <QJSValue>
#include <QQuickItem>
#include <QQuickWidget>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QWidget>
#include <QtTest>
#include <memory>

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

private:
    std::unique_ptr<CanvasFixture> m_canvas;
    TextMediaItem* m_first = nullptr;
    TextMediaItem* m_second = nullptr;
    QString m_firstId;
    QString m_secondId;
};

QTEST_MAIN(CanvasSelectionBackendTest)
#include "tst_CanvasSelectionBackend.moc"

#include "frontend/rendering/canvas/TextOutlineItem.h"
#include "frontend/rendering/canvas/TextEditHelper.h"

#include <QFontDatabase>
#include <QDir>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QtQuick/private/qquicktextedit_p.h>

class TextItemQmlTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(QFontDatabase::addApplicationFont(TEST_SOURCE_DIR "/resources/fonts/impact.ttf") >= 0);
        qmlRegisterType<TextOutlineItem>("Mouffette.Canvas", 1, 0, "TextOutlineItem");
        qmlRegisterSingletonType<TextEditHelper>("Mouffette.Canvas", 1, 0, "TextEditHelper",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new TextEditHelper; });
    }

    void borderFitsAtEveryAlignmentAndAlphaIsUniform()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(TEST_SOURCE_DIR "/resources/qml/TextItem.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QQuickWindow window;
        std::unique_ptr<QObject> object(component.createWithInitialProperties({
            {"mediaWidth", 600}, {"mediaHeight", 360},
            {"textContent", "Éi:B"}, {"fontPixelSize", 64},
            {"outlineWidthPercent", 100}, {"textColor", QColor(Qt::white)},
            {"outlineColor", QColor(Qt::red)}
        }));
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* item = qobject_cast<QQuickItem*>(object.get());
        QVERIFY(item);
        window.setColor(Qt::black);
        window.resize(600, 360);
        item->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        for (const QString& horizontal : {QStringLiteral("left"), QStringLiteral("center"), QStringLiteral("right")}) {
            for (const QString& vertical : {QStringLiteral("top"), QStringLiteral("center"), QStringLiteral("bottom")}) {
                item->setProperty("horizontalAlignment", horizontal);
                item->setProperty("verticalAlignment", vertical);
                QCoreApplication::processEvents();
                const QImage frame = window.grabWindow().convertToFormat(QImage::Format_ARGB32);
                QVERIFY(!frame.isNull());
                int redPixels = 0;
                int whitePixels = 0;
                const int margin = qRound(2 * frame.width() / qreal(window.width()));
                for (int y = 0; y < frame.height(); ++y) {
                    const auto* row = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
                    for (int x = 0; x < frame.width(); ++x) {
                        if (qRed(row[x]) > 150 && qGreen(row[x]) < 30) {
                            ++redPixels;
                            QVERIFY2(x >= margin && y >= margin && x < frame.width() - margin
                                     && y < frame.height() - margin,
                                     qPrintable("Clipped border at " + horizontal + "/" + vertical));
                        }
                        if (qRed(row[x]) > 220 && qGreen(row[x]) > 220)
                            ++whitePixels;
                    }
                }
                QVERIFY(redPixels > 100);
                QVERIFY(whitePixels > 100);
                if (horizontal == "center" && vertical == "center")
                    frame.save(QDir::tempPath() + "/mouffette-text-outline-composite.png");
            }
        }

        item->setProperty("textColor", QColor(Qt::transparent));
        item->setProperty("outlineColor", QColor(255, 0, 0, 128));
        QCoreApplication::processEvents();
        const QImage translucent = window.grabWindow().convertToFormat(QImage::Format_ARGB32);
        QVERIFY(!translucent.isNull());
        int maximumRed = 0;
        for (int y = 0; y < translucent.height(); ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(translucent.constScanLine(y));
            for (int x = 0; x < translucent.width(); ++x)
                maximumRed = qMax(maximumRed, qRed(row[x]));
        }
        QVERIFY2(maximumRed >= 125 && maximumRed <= 130,
                 qPrintable(QStringLiteral("Overlapping border alpha accumulated: %1").arg(maximumRed)));

        // The same text at the right edge of a huge document must use only a
        // viewport-sized alpha texture, without stretching or shifting it.
        auto* outline = item->findChild<TextOutlineItem*>();
        QVERIFY(outline);
        for (qreal panFraction : {qreal(0), qreal(0.25)}) {
            item->setProperty("mediaWidth", 600);
            item->setX(panFraction);
            QCoreApplication::processEvents();
            const QImage baseline = window.grabWindow().convertToFormat(QImage::Format_ARGB32);
            item->setProperty("mediaWidth", 100000);
            item->setX(600 - 100000 + panFraction);
            QCoreApplication::processEvents();
            const QImage huge = window.grabWindow().convertToFormat(QImage::Format_ARGB32);
            QVERIFY(!huge.isNull());
            QVERIFY(outline->renderedRect().left() > 99000);
            // The stable viewport cache includes a 96-DIP guard on each side.
            // It must remain viewport-sized, never the 100,000-pixel document.
            QVERIFY(outline->renderedRect().width() <= window.width() + 2 * 96 + 2);
            QCOMPARE(outline->renderedRect(), QRectF(outline->renderedRect().toAlignedRect()));
            QVERIFY(outline->renderedPixelSize().width() <= 4096);
            QVERIFY(outline->renderedPixelSize().height() <= 4096);
            qsizetype baselinePixels = 0;
            qsizetype hugePixels = 0;
            qsizetype differingPixels = 0;
            maximumRed = 0;
            QCOMPARE(huge.size(), baseline.size());
            for (int y = 0; y < huge.height(); ++y) {
                const auto* a = reinterpret_cast<const QRgb*>(baseline.constScanLine(y));
                const auto* b = reinterpret_cast<const QRgb*>(huge.constScanLine(y));
                for (int x = 0; x < huge.width(); ++x) {
                    const bool wasRed = qRed(a[x]) > 60;
                    const bool isRed = qRed(b[x]) > 60;
                    baselinePixels += wasRed;
                    hugePixels += isRed;
                    differingPixels += wasRed != isRed;
                    maximumRed = qMax(maximumRed, qRed(b[x]));
                }
            }
            QVERIFY(hugePixels > 100);
            QVERIFY(maximumRed >= 125 && maximumRed <= 130);
            QVERIFY2(differingPixels < baselinePixels / 20,
                     qPrintable(QStringLiteral("Viewport alpha mask shifted/stretched: %1 / %2 pixels")
                         .arg(differingPixels).arg(baselinePixels)));
        }
        item->setParentItem(nullptr);
    }

    void highlightFollowsLiveTextThroughoutEditing()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(TEST_SOURCE_DIR "/resources/qml/TextItem.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QQuickWindow window;
        std::unique_ptr<QObject> object(component.createWithInitialProperties({
            {"mediaWidth", 600}, {"mediaHeight", 320}, {"mediaId", "highlight-test"},
            {"textContent", "A"}, {"fontPixelSize", 40}, {"textEditable", true},
            {"horizontalAlignment", "left"}, {"verticalAlignment", "top"},
            {"textColor", QColor(Qt::white)}, {"highlightEnabled", true},
            {"highlightColor", QColor(Qt::green)}
        }));
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* item = qobject_cast<QQuickItem*>(object.get());
        QVERIFY(item);
        auto* outline = item->findChild<TextOutlineItem*>();
        QVERIFY(outline);
        auto* edit = qobject_cast<QQuickTextEdit*>(outline->source());
        QVERIFY(edit);
        QSignalSpy liveUpdates(item, SIGNAL(textLiveUpdateRequested(QString,QString)));
        QSignalSpy commits(item, SIGNAL(textCommitRequested(QString,QString)));
        QVERIFY(liveUpdates.isValid());
        QVERIFY(commits.isValid());
        window.setColor(Qt::black);
        window.resize(600, 320);
        item->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        const auto highlightBounds = [&]() {
            QCoreApplication::processEvents();
            const QImage frame = window.grabWindow().convertToFormat(QImage::Format_ARGB32);
            int left = frame.width(), top = frame.height(), right = -1, bottom = -1;
            for (int y = 0; y < frame.height(); ++y) {
                const auto* row = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
                for (int x = 0; x < frame.width(); ++x) {
                    if (qGreen(row[x]) > 200 && qRed(row[x]) < 40 && qBlue(row[x]) < 40) {
                        left = qMin(left, x);
                        right = qMax(right, x);
                        top = qMin(top, y);
                        bottom = qMax(bottom, y);
                    }
                }
            }
            return right >= left ? QRect(QPoint(left, top), QPoint(right, bottom)) : QRect();
        };
        const QRect initial = highlightBounds();
        QVERIFY(!initial.isEmpty());

        // The parent canvas forwards actual double-clicks through this method.
        QVERIFY(QMetaObject::invokeMethod(item, "fireDoubleClick",
                                          Q_ARG(QVariant, false),
                                          Q_ARG(QVariant, QVariant()),
                                          Q_ARG(QVariant, QVariant())));
        QVERIFY(item->property("editing").toBool());
        QVERIFY(edit->isEnabled());
        QVERIFY(!edit->isReadOnly());
        QVERIFY(edit->hasActiveFocus());
        QCOMPARE(highlightBounds(), initial);

        QTest::keyClick(&window, Qt::Key_End);
        QTest::keyClick(&window, Qt::Key_W);
        QCOMPARE(edit->text(), QStringLiteral("Aw"));
        edit->insert(edit->length(), QStringLiteral(" LONG TEXT\nSECOND LINE"));
        QCOMPARE(item->property("textContent").toString(), QStringLiteral("A"));
        const QRect expanded = highlightBounds();
        QVERIFY(expanded.width() > initial.width() * 3);
        QVERIFY(expanded.height() > initial.height() + 10);
        QVERIFY(liveUpdates.count() >= 2);

        // A stale/empty committed model must not govern the live background.
        item->setProperty("textContent", QString());
        QVERIFY(!edit->text().isEmpty());
        edit->selectAll();
        QTest::keyClick(&window, Qt::Key_Backspace);
        QCOMPARE(edit->length(), 0);
        QVERIFY(highlightBounds().isEmpty());

        QInputMethodEvent preedit(QStringLiteral("ime"), {});
        QCoreApplication::sendEvent(edit, &preedit);
        QCOMPARE(edit->length(), 0);
        QCOMPARE(edit->preeditText(), QStringLiteral("ime"));
        QVERIFY(!highlightBounds().isEmpty());
        QInputMethodEvent clearPreedit;
        QCoreApplication::sendEvent(edit, &clearPreedit);
        QVERIFY(highlightBounds().isEmpty());

        QTest::keyClick(&window, Qt::Key_W);
        QCOMPARE(edit->text(), QStringLiteral("w"));
        QCOMPARE(item->property("textContent").toString(), QString());
        QVERIFY(!highlightBounds().isEmpty());
        item->setProperty("highlightEnabled", false);
        QVERIFY(highlightBounds().isEmpty());
        item->setProperty("highlightEnabled", true);
        const QRect restored = highlightBounds();
        QVERIFY(!restored.isEmpty());
        edit->select(0, 1);
        QCOMPARE(edit->selectedText(), QStringLiteral("w"));

        // Supply the host's authoritative value only at commit, after all
        // preceding edits were intentionally tested without model round-trips.
        item->setProperty("textContent", edit->text());
        QVERIFY(QMetaObject::invokeMethod(item, "commitAndStopEditing"));
        QCOMPARE(commits.count(), 1);
        QCOMPARE(commits.first().at(1).toString(), QStringLiteral("w"));
        QVERIFY(!item->property("editing").toBool());
        QVERIFY(edit->selectedText().isEmpty());
        QCOMPARE(highlightBounds(), restored);
        item->setParentItem(nullptr);
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    TextItemQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_TextItemQml.moc"

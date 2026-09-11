#include "frontend/rendering/canvas/TextOutlineItem.h"
#include "frontend/rendering/canvas/TextEditHelper.h"

#include <QFontDatabase>
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTest>

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

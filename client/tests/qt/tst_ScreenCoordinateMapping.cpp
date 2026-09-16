#include <QtTest>

#include "backend/managers/system/ScreenCoordinateMapping.h"

class ScreenCoordinateMappingTest final : public QObject
{
    Q_OBJECT

private slots:
    void screenGeometryPreservesIndependentOriginAndSize_data();
    void screenGeometryPreservesIndependentOriginAndSize();
    void cursorUsesScreenLocalScaleOnMixedDpiDesktop();
};

void ScreenCoordinateMappingTest::screenGeometryPreservesIndependentOriginAndSize_data()
{
    QTest::addColumn<QRect>("logicalGeometry");
    QTest::addColumn<qreal>("scale");
    QTest::addColumn<QRect>("expectedGeometry");

    QTest::newRow("primary-retina")
        << QRect(0, 0, 1512, 982) << qreal(2) << QRect(0, 0, 3024, 1964);
    QTest::newRow("retina-left-and-above")
        << QRect(-1920, -1080, 1920, 1080) << qreal(2)
        << QRect(-3840, -2160, 3840, 2160);
    QTest::newRow("retina-right-and-below")
        << QRect(1920, 1080, 1920, 1080) << qreal(2)
        << QRect(3840, 2160, 3840, 2160);
    QTest::newRow("unscaled-secondary")
        << QRect(-1920, 240, 1920, 1080) << qreal(1)
        << QRect(-1920, 240, 1920, 1080);
    QTest::newRow("fractional-scale-rounding")
        << QRect(-101, 103, 1919, 1079) << qreal(1.5)
        << QRect(-152, 155, 2879, 1619);
}

void ScreenCoordinateMappingTest::screenGeometryPreservesIndependentOriginAndSize()
{
    QFETCH(QRect, logicalGeometry);
    QFETCH(qreal, scale);
    QFETCH(QRect, expectedGeometry);
    const QRect originalGeometry = logicalGeometry;

    QCOMPARE(ScreenCoordinateMapping::scaledScreenGeometry(logicalGeometry, scale),
             expectedGeometry);
    QCOMPARE(logicalGeometry, originalGeometry);
}

void ScreenCoordinateMappingTest::cursorUsesScreenLocalScaleOnMixedDpiDesktop()
{
    const QRect retinaScreen(-1512, -240, 1512, 982);
    const QRect standardScreen(0, 0, 1920, 1080);

    QCOMPARE(ScreenCoordinateMapping::screenLocalPosition(
                 QPoint(-1412, -190), retinaScreen, 2),
             QPointF(200, 100));
    QCOMPARE(ScreenCoordinateMapping::screenLocalPosition(
                 QPoint(100, 50), standardScreen, 1),
             QPointF(100, 50));
    QCOMPARE(ScreenCoordinateMapping::screenLocalPosition(
                 retinaScreen.topLeft(), retinaScreen, 2),
             QPointF(0, 0));
    QCOMPARE(ScreenCoordinateMapping::screenLocalPosition(
                 retinaScreen.bottomRight(), retinaScreen, 2),
             QPointF(3022, 1962));
    QCOMPARE(ScreenCoordinateMapping::screenLocalPosition(
                 QPoint(101, 51), standardScreen, 1.5),
             QPointF(151.5, 76.5));
}

QTEST_APPLESS_MAIN(ScreenCoordinateMappingTest)
#include "tst_ScreenCoordinateMapping.moc"

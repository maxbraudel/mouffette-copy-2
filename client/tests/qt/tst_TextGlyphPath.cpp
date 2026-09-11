#include "frontend/rendering/canvas/TextGlyphPath.h"

#include <QGuiApplication>
#include <QTest>

class TestableTextGlyphPath final : public TextGlyphPath {
public:
    using TextGlyphPath::TextGlyphPath;
    void rebuildNow() { updatePolish(); }
};

class TextGlyphPathTest final : public QObject {
    Q_OBJECT

private:
    static void configure(TestableTextGlyphPath& path) {
        path.setFontFamily(QStringLiteral("Impact"));
        path.setFontPixelSize(48);
        path.setFontWeight(400);
        path.setOutlinePixels(6.0);
        path.setItemWidth(420.0);
        path.setItemHeight(180.0);
        path.setHorizontalAlignment(QStringLiteral("left"));
        path.setVerticalAlignment(QStringLiteral("top"));
    }

private slots:
    void outlineFollowsCharacterByCharacterInput() {
        TestableTextGlyphPath path;
        configure(path);
        const QString text = QStringLiteral("Contour élan 🚀 漢字");
        for (int i = 1; i <= text.size(); ++i) {
            path.setTextContent(text.left(i));
            path.rebuildNow();
            QVERIFY2(!path.strokePath().isEmpty(), "outline disappeared during live input");
        }
    }

    void alignmentChangeRebuildsAbsoluteCoordinates() {
        TestableTextGlyphPath path;
        configure(path);
        path.setTextContent(QStringLiteral("ALIGN   "));
        path.rebuildNow();
        const QString leftPath = path.strokePath();

        path.setHorizontalAlignment(QStringLiteral("center"));
        path.rebuildNow();
        const QString centerPath = path.strokePath();
        QVERIFY(leftPath != centerPath);
        QCOMPARE(path.strokeXOffset(), 0.0);

        path.setHorizontalAlignment(QStringLiteral("right"));
        path.rebuildNow();
        QVERIFY(centerPath != path.strokePath());
        QCOMPARE(path.strokeXOffset(), 0.0);
    }

    void widthOnlyResizeUsesTranslationWithoutReflow() {
        TestableTextGlyphPath path;
        configure(path);
        path.setHorizontalAlignment(QStringLiteral("center"));
        path.setTextContent(QStringLiteral("SHORT"));
        path.rebuildNow();
        const QString originalPath = path.strokePath();

        path.setItemWidth(480.0);
        path.rebuildNow();
        QCOMPARE(path.strokePath(), originalPath);
        QVERIFY(qAbs(path.strokeXOffset() - 30.0) <= 0.5);
    }

    void verticalAlignmentAndMultilineChangeGeometry() {
        TestableTextGlyphPath path;
        configure(path);
        path.setTextContent(QStringLiteral("first line  \nsecond line"));
        path.rebuildNow();
        const QString topPath = path.strokePath();

        path.setVerticalAlignment(QStringLiteral("center"));
        path.rebuildNow();
        const QString centerPath = path.strokePath();
        QVERIFY(topPath != centerPath);

        path.setVerticalAlignment(QStringLiteral("bottom"));
        path.rebuildNow();
        QVERIFY(centerPath != path.strokePath());
    }

    void stylesFallbackFontsAndExactThicknessesRemainRenderable() {
        TestableTextGlyphPath path;
        configure(path);
        path.setTextContent(QStringLiteral("Crème 🙂 漢字"));
        const QList<int> weights { 400, 700 };
        const QList<qreal> widths { 1.0, 2.375, 3.0, 7.0 };
        for (int weight : weights) {
            path.setFontWeight(weight);
            for (bool italic : { false, true }) {
                path.setFontItalic(italic);
                for (qreal width : widths) {
                    path.setOutlinePixels(width);
                    path.rebuildNow();
                    QVERIFY(!path.strokePath().isEmpty());
                }
            }
        }
    }
};

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication application(argc, argv);
    TextGlyphPathTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_TextGlyphPath.moc"

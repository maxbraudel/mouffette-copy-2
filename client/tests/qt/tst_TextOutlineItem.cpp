#include "frontend/rendering/canvas/TextEditHelper.h"
#include "frontend/rendering/canvas/TextOutlineItem.h"

#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPathStroker>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QtGui/private/qrhi_p.h>
#include <QtQuick/private/qquicktextedit_p.h>
#include <QtQuick/private/qquicktextedit_p_p.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr int renderTimeoutMs = 5000;

class TestableTextOutlineItem final : public TextOutlineItem {
public:
    using TextOutlineItem::TextOutlineItem;
    void rebuildNow() { updatePolish(); }
};

struct PixelBounds {
    QRect rect;
    qsizetype pixels = 0;
};

PixelBounds nonBlackBounds(const QImage& source, int threshold = 32)
{
    const QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int left = image.width();
    int top = image.height();
    int right = -1;
    int bottom = -1;
    qsizetype pixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            if (qMax(qRed(pixel), qMax(qGreen(pixel), qBlue(pixel))) <= threshold)
                continue;
            left = qMin(left, x);
            top = qMin(top, y);
            right = qMax(right, x);
            bottom = qMax(bottom, y);
            ++pixels;
        }
    }
    return {{left, top, right >= left ? right - left + 1 : 0,
             bottom >= top ? bottom - top + 1 : 0}, pixels};
}

QImage grabAfterSync(QQuickWindow& window, TestableTextOutlineItem* outline)
{
    if (outline)
        outline->rebuildNow();
    window.update();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    for (int attempt = 0; attempt < 5; ++attempt) {
        QImage image = window.grabWindow();
        if (!image.isNull())
            return image;
        QTest::qWait(16);
    }
    return {};
}

QPainterPath documentGlyphPath(QQuickTextEdit* edit, QQuickItem* target,
                               qreal outlinePixels = 0)
{
    auto* state = QQuickTextEditPrivate::get(edit);
    QTextDocument* document = state->document;
    QAbstractTextDocumentLayout* documentLayout = document->documentLayout();
    documentLayout->documentSize();
    const QPointF documentOrigin =
        edit->mapToItem(target, QPointF(state->xoff, state->yoff));

    QPainterPath result;
    // Individual glyph strokes overlap heavily at large border widths. Odd-even
    // would XOR those overlaps and create holes that the scene graph does not.
    result.setFillRule(Qt::WindingFill);
    QPainterPathStroker stroker;
    if (outlinePixels > 0) {
        stroker.setWidth(outlinePixels * 2);
        stroker.setJoinStyle(Qt::RoundJoin);
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setMiterLimit(1.5);
    }

    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        if (!block.isVisible() || !block.layout())
            continue;
        const QPointF blockOrigin = documentOrigin
            + documentLayout->blockBoundingRect(block).topLeft();
        QTextLayout* blockLayout = block.layout();
        for (int lineIndex = 0; lineIndex < blockLayout->lineCount(); ++lineIndex) {
            const QTextLine line = blockLayout->lineAt(lineIndex);
            for (const QGlyphRun& run : line.glyphRuns()) {
                const QList<quint32> indexes = run.glyphIndexes();
                const QList<QPointF> positions = run.positions();
                const qsizetype count = qMin(indexes.size(), positions.size());
                for (qsizetype i = 0; i < count; ++i) {
                    QPainterPath glyph = run.rawFont().pathForGlyph(indexes[i]);
                    glyph.translate(blockOrigin + positions[i]);
                    result.addPath(outlinePixels > 0 ? stroker.createStroke(glyph) : glyph);
                }
            }
        }
    }
    return result;
}

QImage rasterizeReference(const QPainterPath& path, const QSize& pixelSize,
                          const QSizeF& logicalSize)
{
    QImage image(pixelSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(pixelSize.width() / logicalSize.width(),
                  pixelSize.height() / logicalSize.height());
    painter.fillPath(path, Qt::white);
    return image;
}

double binaryMaskIoU(const QImage& firstSource, const QImage& secondSource,
                     int threshold = 32)
{
    const QImage first = firstSource.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage second = secondSource.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (first.size() != second.size())
        return 0;
    qsizetype intersection = 0;
    qsizetype setUnion = 0;
    for (int y = 0; y < first.height(); ++y) {
        const auto* a = reinterpret_cast<const QRgb*>(first.constScanLine(y));
        const auto* b = reinterpret_cast<const QRgb*>(second.constScanLine(y));
        for (int x = 0; x < first.width(); ++x) {
            const bool inA = qMax(qRed(a[x]), qMax(qGreen(a[x]), qBlue(a[x]))) > threshold;
            const bool inB = qMax(qRed(b[x]), qMax(qGreen(b[x]), qBlue(b[x]))) > threshold;
            intersection += inA && inB;
            setUnion += inA || inB;
        }
    }
    return setUnion ? double(intersection) / double(setUnion) : 0;
}

struct Scene {
    QQuickWindow window;
    QQuickTextEdit* edit = nullptr;
    TestableTextOutlineItem* outline = nullptr;
    TextEditHelper helper;

    Scene(const QString& text, QQuickTextEdit::HAlignment alignment,
          int fontPixels, qreal outlinePixels, QSize windowSize = {900, 520})
    {
        window.setColor(Qt::black);
        window.resize(windowSize);

        outline = new TestableTextOutlineItem(window.contentItem());
        outline->setPosition({0, 0});
        outline->setSize(windowSize);
        outline->setZ(0);
        outline->setColor(Qt::white);
        outline->setOutlinePixels(outlinePixels);

        edit = new QQuickTextEdit(window.contentItem());
        edit->setPosition({150, 85});
        edit->setSize({600, 350});
        edit->setZ(1);
        edit->setTextFormat(QQuickTextEdit::PlainText);
        edit->setWrapMode(QQuickTextEdit::Wrap);
        edit->setHAlign(alignment);
        edit->setVAlign(QQuickTextEdit::AlignTop);
        edit->setTopPadding(11);
        edit->setLeftPadding(0);
        edit->setRightPadding(0);
        edit->setBottomPadding(0);
        edit->setReadOnly(true);
        edit->setCursorVisible(false);
        edit->setColor(Qt::white);
        QFont font(QStringLiteral("Impact"));
        font.setPixelSize(fontPixels);
        font.setHintingPreference(QFont::PreferNoHinting);
        font.setKerning(true);
        edit->setFont(font);
        edit->setText(text);
        edit->componentComplete();
        helper.applyIncludeTrailingSpaces(edit);
        outline->setSource(edit);
    }

    bool expose()
    {
        window.show();
        return QTest::qWaitForWindowExposed(&window, renderTimeoutMs);
    }
};

struct OutlineCapture {
    QImage rendered;
    QImage reference;
    PixelBounds renderedBounds;
    PixelBounds referenceBounds;
    double iou = 0;
};

OutlineCapture captureOutline(Scene& scene)
{
    scene.outline->setVisible(true);
    scene.edit->setColor(Qt::transparent);
    OutlineCapture capture;
    capture.rendered = grabAfterSync(scene.window, scene.outline);
    if (capture.rendered.isNull())
        return capture;
    const QPainterPath localReferencePath = documentGlyphPath(
        scene.edit, scene.outline, scene.outline->outlinePixels());
    const QPointF origin = scene.outline->mapToItem(scene.window.contentItem(), QPointF());
    const QPointF xBasis = scene.outline->mapToItem(
        scene.window.contentItem(), QPointF(1, 0)) - origin;
    const QPointF yBasis = scene.outline->mapToItem(
        scene.window.contentItem(), QPointF(0, 1)) - origin;
    const QTransform itemToWindow(xBasis.x(), xBasis.y(), yBasis.x(), yBasis.y(),
                                  origin.x(), origin.y());
    const QPainterPath referencePath = itemToWindow.map(localReferencePath);
    capture.reference = rasterizeReference(referencePath, capture.rendered.size(),
                                            scene.window.size());
    capture.renderedBounds = nonBlackBounds(capture.rendered);
    capture.referenceBounds = nonBlackBounds(capture.reference);
    capture.iou = binaryMaskIoU(capture.rendered, capture.reference);
    return capture;
}

int documentLineCount(QQuickTextEdit* edit)
{
    auto* state = QQuickTextEditPrivate::get(edit);
    state->document->documentLayout()->documentSize();
    int result = 0;
    for (QTextBlock block = state->document->begin(); block.isValid(); block = block.next()) {
        if (block.isVisible() && block.layout())
            result += block.layout()->lineCount();
    }
    return result;
}

} // namespace

class TextOutlineItemTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        const QString fontPath = QStringLiteral(TEST_SOURCE_DIR "/resources/fonts/impact.ttf");
        const int fontId = QFontDatabase::addApplicationFont(fontPath);
        QVERIFY2(fontId >= 0, qPrintable(QStringLiteral("Cannot load test font: %1").arg(fontPath)));
        QVERIFY(QFontDatabase::applicationFontFamilies(fontId).contains(QStringLiteral("Impact")));
    }

    void actualDocumentGeometryMatchesPixels_data()
    {
        QTest::addColumn<int>("alignment");
        QTest::newRow("left") << int(QQuickTextEdit::AlignLeft);
        QTest::newRow("center") << int(QQuickTextEdit::AlignHCenter);
        QTest::newRow("right") << int(QQuickTextEdit::AlignRight);
    }

    void actualDocumentGeometryMatchesPixels()
    {
        QFETCH(int, alignment);
        const QString text = QStringLiteral(
            "ÉTÉ i:j · ŒUVRE AVEC DES MOTS ET DES ESPACES   POUR WRAPPER   \n"
            "DEUXIÈME LIGNE ÀÇÉÏ   ");
        Scene scene(text, QQuickTextEdit::HAlignment(alignment), 48, 48);
        QVERIFY2(scene.expose(), "The Qt Quick window could not be exposed");

        // Render the outline first. An invisible QQuickItem is intentionally
        // not polished by Qt, so hiding it before its first frame would turn
        // this into a test of polish scheduling rather than text geometry.
        scene.edit->setColor(Qt::transparent);
        const QImage outlineImage = grabAfterSync(scene.window, scene.outline);
        QVERIFY(!outlineImage.isNull());
        const PixelBounds outline = nonBlackBounds(outlineImage);
        QVERIFY2(outline.pixels > 0, "TextOutlineItem produced no pixels");

        scene.outline->setVisible(false);
        scene.edit->setColor(Qt::white);
        const QImage fillImage = grabAfterSync(scene.window, nullptr);
        QVERIFY(!fillImage.isNull());
        const PixelBounds fill = nonBlackBounds(fillImage);
        QVERIFY2(fill.pixels > 0, "Native TextEdit produced no pixels");

        const QPainterPath referencePath =
            documentGlyphPath(scene.edit, scene.outline, scene.outline->outlinePixels());
        const QImage reference = rasterizeReference(referencePath, outlineImage.size(),
                                                     scene.window.size());
        const PixelBounds expected = nonBlackBounds(reference);
        QVERIFY(expected.pixels > 0);

        const qreal sx = outlineImage.width() / qreal(scene.window.width());
        const qreal tolerance = qMax<qreal>(3, std::ceil(2 * sx));
        QVERIFY2(std::abs(outline.rect.left() - expected.rect.left()) <= tolerance,
                 qPrintable(QStringLiteral("left edge differs: rendered=%1 expected=%2")
                     .arg(outline.rect.left()).arg(expected.rect.left())));
        QVERIFY2(std::abs(outline.rect.right() - expected.rect.right()) <= tolerance,
                 qPrintable(QStringLiteral("right edge differs: rendered=%1 expected=%2")
                     .arg(outline.rect.right()).arg(expected.rect.right())));
        QVERIFY2(std::abs(outline.rect.top() - expected.rect.top()) <= tolerance,
                 qPrintable(QStringLiteral("top edge differs: rendered=%1 expected=%2")
                     .arg(outline.rect.top()).arg(expected.rect.top())));
        QVERIFY2(std::abs(outline.rect.bottom() - expected.rect.bottom()) <= tolerance,
                 qPrintable(QStringLiteral("bottom edge differs: rendered=%1 expected=%2")
                     .arg(outline.rect.bottom()).arg(expected.rect.bottom())));

        const qreal centerDelta = std::abs(fill.rect.center().x() - outline.rect.center().x());
        QVERIFY2(centerDelta <= tolerance,
                 qPrintable(QStringLiteral("fill/outline horizontal center drift: %1 px")
                     .arg(centerDelta)));

        const double iou = binaryMaskIoU(outlineImage, reference);
        QVERIFY2(iou >= 0.88,
                 qPrintable(QStringLiteral(
                     "outline/reference mask IoU too low: %1 (rendered=%2 px, expected=%3 px)")
                     .arg(iou).arg(outline.pixels).arg(expected.pixels)));

        if (qEnvironmentVariableIsSet("MOUFFETTE_SAVE_TEXT_TEST_IMAGES")) {
            scene.outline->setVisible(true);
            scene.outline->setColor(QColor(QStringLiteral("#e04040")));
            scene.edit->setColor(Qt::white);
            const QImage composite = grabAfterSync(scene.window, scene.outline);
            composite.save(QStringLiteral("/tmp/mouffette_text_composite_%1.png")
                               .arg(alignment));
        }

#ifdef Q_OS_MACOS
        QCOMPARE(scene.window.rendererInterface()->graphicsApi(), QSGRendererInterface::Metal);
#endif
    }

    void disconnectedGlyphContoursAreRendered_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("lowercase-i") << QStringLiteral("i");
        QTest::newRow("lowercase-j") << QStringLiteral("j");
        QTest::newRow("acute-accent") << QStringLiteral("é");
        QTest::newRow("colon") << QStringLiteral(":");
        QTest::newRow("counter-O") << QStringLiteral("O");
        QTest::newRow("counters-B") << QStringLiteral("B");
    }

    void disconnectedGlyphContoursAreRendered()
    {
        QFETCH(QString, text);
        Scene scene(text, QQuickTextEdit::AlignHCenter, 80, 24, {420, 320});
        scene.edit->setPosition({110, 70});
        scene.edit->setSize({200, 180});
        scene.outline->setSize(scene.window.size());
        QVERIFY2(scene.expose(), "The Qt Quick window could not be exposed");

        scene.edit->setColor(Qt::transparent);
        const QImage rendered = grabAfterSync(scene.window, scene.outline);
        QVERIFY(!rendered.isNull());
        const QPainterPath referencePath =
            documentGlyphPath(scene.edit, scene.outline, scene.outline->outlinePixels());
        const QImage reference = rasterizeReference(referencePath, rendered.size(),
                                                     scene.window.size());
        if (qEnvironmentVariableIsSet("MOUFFETTE_SAVE_TEXT_TEST_IMAGES")) {
            rendered.save(QStringLiteral("/tmp/mouffette_outline_rendered.png"));
            reference.save(QStringLiteral("/tmp/mouffette_outline_reference.png"));
        }
        const PixelBounds renderedBounds = nonBlackBounds(rendered);
        const PixelBounds referenceBounds = nonBlackBounds(reference);
        const double iou = binaryMaskIoU(rendered, reference);
        QVERIFY2(iou >= 0.90,
                 qPrintable(QStringLiteral(
                     "glyph '%1' differs (IoU %2, rendered %3/%4 px @ %5,%6 %7x%8, "
                     "expected %9 px @ %10,%11 %12x%13)")
                     .arg(text).arg(iou).arg(renderedBounds.pixels).arg(rendered.size().width()
                         * rendered.size().height())
                     .arg(renderedBounds.rect.x()).arg(renderedBounds.rect.y())
                     .arg(renderedBounds.rect.width()).arg(renderedBounds.rect.height())
                     .arg(referenceBounds.pixels).arg(referenceBounds.rect.x())
                     .arg(referenceBounds.rect.y()).arg(referenceBounds.rect.width())
                     .arg(referenceBounds.rect.height())));
    }

    void wrapResizeTracksTheActualDocument()
    {
        const QString text = QStringLiteral(
            "ÉTÉ EN ITALIQUE AVEC DES MOTS LONGS, DES ACCENTS ET DES ESPACES   "
            "POUR FORCER PLUSIEURS RETOURS À LA LIGNE SANS RECRÉER LE LAYOUT   ");
        Scene scene(text, QQuickTextEdit::AlignHCenter, 44, 30, {900, 700});
        QFont italic = scene.edit->font();
        italic.setItalic(true);
        scene.edit->setFont(italic);
        scene.edit->setPosition({150, 55});
        scene.edit->setSize({270, 580});
        scene.outline->setSize(scene.window.size());
        QVERIFY2(scene.expose(), "The Qt Quick window could not be exposed");

        const OutlineCapture narrow = captureOutline(scene);
        QVERIFY(!narrow.rendered.isNull());
        QVERIFY2(narrow.iou >= 0.86,
                 qPrintable(QStringLiteral("narrow wrap IoU too low: %1").arg(narrow.iou)));
        const int narrowLines = documentLineCount(scene.edit);

        scene.edit->setWidth(600);
        const OutlineCapture wide = captureOutline(scene);
        QVERIFY(!wide.rendered.isNull());
        QVERIFY2(wide.iou >= 0.86,
                 qPrintable(QStringLiteral("wide wrap IoU too low: %1").arg(wide.iou)));
        const int wideLines = documentLineCount(scene.edit);
        QVERIFY2(wideLines < narrowLines,
                 qPrintable(QStringLiteral("resize did not rewrap: narrow=%1 wide=%2")
                     .arg(narrowLines).arg(wideLines)));

        scene.edit->setWidth(270);
        const OutlineCapture narrowAgain = captureOutline(scene);
        QVERIFY(!narrowAgain.rendered.isNull());
        QVERIFY2(narrowAgain.iou >= 0.86,
                 qPrintable(QStringLiteral("second narrow wrap IoU too low: %1")
                     .arg(narrowAgain.iou)));
        QCOMPARE(documentLineCount(scene.edit), narrowLines);
        const qreal dprTolerance = qMax<qreal>(3, scene.window.devicePixelRatio() * 2);
        QVERIFY(std::abs(narrowAgain.renderedBounds.rect.left()
                         - narrow.renderedBounds.rect.left()) <= dprTolerance);
        QVERIFY(std::abs(narrowAgain.renderedBounds.rect.right()
                         - narrow.renderedBounds.rect.right()) <= dprTolerance);
        QVERIFY(std::abs(narrowAgain.renderedBounds.rect.bottom()
                         - narrow.renderedBounds.rect.bottom()) <= dprTolerance);
    }

    void italicFallbackRtlAndWidthTransitions()
    {
        const QString text = QStringLiteral(
            "שָׁלוֹם — العربية — Élan italic — 漢字");
        Scene scene(text, QQuickTextEdit::AlignRight, 52, 0, {1000, 430});
        QFont italic = scene.edit->font();
        italic.setItalic(true);
        scene.edit->setFont(italic);
        scene.edit->setPosition({100, 85});
        scene.edit->setSize({800, 230});
        scene.outline->setSize(scene.window.size());
        QVERIFY2(scene.expose(), "The Qt Quick window could not be exposed");

        // The fixture font intentionally lacks these scripts. Verify that this
        // really exercises multiple QRawFonts instead of merely containing
        // nominal fallback characters.
        auto* state = QQuickTextEditPrivate::get(scene.edit);
        state->document->documentLayout()->documentSize();
        QSet<QString> rawFontFamilies;
        for (QTextBlock block = state->document->begin(); block.isValid(); block = block.next()) {
            if (!block.layout())
                continue;
            for (int lineIndex = 0; lineIndex < block.layout()->lineCount(); ++lineIndex) {
                for (const QGlyphRun& run : block.layout()->lineAt(lineIndex).glyphRuns())
                    rawFontFamilies.insert(run.rawFont().familyName());
            }
        }
        QVERIFY2(rawFontFamilies.size() >= 2,
                 qPrintable(QStringLiteral("fallback was not exercised: %1")
                     .arg(QStringList(rawFontFamilies.cbegin(), rawFontFamilies.cend()).join(','))));

        const OutlineCapture zero = captureOutline(scene);
        QVERIFY(!zero.rendered.isNull());
        QCOMPARE(zero.renderedBounds.pixels, 0);
        QCOMPARE(scene.outline->statistics().glyphs, 0);

        scene.outline->setOutlinePixels(40);
        const OutlineCapture large = captureOutline(scene);
        QVERIFY(!large.rendered.isNull());
        QVERIFY2(large.renderedBounds.pixels > 0, "large RTL/fallback outline is empty");
        QVERIFY2(large.iou >= 0.84,
                 qPrintable(QStringLiteral("large RTL/fallback IoU too low: %1")
                     .arg(large.iou)));
        QVERIFY(scene.outline->statistics().generatedGlyphs > 0);

        scene.outline->setOutlinePixels(8);
        const OutlineCapture small = captureOutline(scene);
        QVERIFY(!small.rendered.isNull());
        QVERIFY2(small.renderedBounds.pixels > 0, "small RTL/fallback outline is empty");
        QVERIFY2(small.iou >= 0.84,
                 qPrintable(QStringLiteral("small RTL/fallback IoU too low: %1")
                     .arg(small.iou)));
        QVERIFY(scene.outline->statistics().generatedGlyphs > 0);
        QVERIFY(large.renderedBounds.rect.contains(small.renderedBounds.rect));
    }

    void viewportCullingFollowsAncestorPanAndZoom()
    {
        const QString aSection(220, QLatin1Char('A'));
        const QString bSection(220, QLatin1Char('B'));
        const QString qSection(220, QLatin1Char('Q'));
        const QString text = aSection + bSection + qSection;
        Scene scene(text, QQuickTextEdit::AlignLeft, 56, 22, {720, 330});
        scene.edit->setWrapMode(QQuickTextEdit::NoWrap);
        const QFontMetricsF metrics(scene.edit->font());
        const qreal aAdvance = metrics.horizontalAdvance(aSection);
        const qreal bAdvance = metrics.horizontalAdvance(bSection);
        const qreal fullAdvance = metrics.horizontalAdvance(text);

        auto* canvas = new QQuickItem(scene.window.contentItem());
        canvas->setTransformOrigin(QQuickItem::TopLeft);
        canvas->setSize({fullAdvance + 200, 330});
        scene.outline->setParentItem(canvas);
        scene.edit->setParentItem(canvas);
        scene.outline->setPosition({0, 0});
        scene.outline->setSize({fullAdvance + 160, 300});
        scene.edit->setPosition({30, 95});
        scene.edit->setSize({fullAdvance + 100, 130});
        QVERIFY2(scene.expose(), "The Qt Quick window could not be exposed");

        const OutlineCapture prefix = captureOutline(scene);
        QVERIFY(!prefix.rendered.isNull());
        QVERIFY2(prefix.iou >= 0.88,
                 qPrintable(QStringLiteral("prefix viewport IoU too low: %1")
                     .arg(prefix.iou)));
        const auto prefixStats = scene.outline->statistics();
        QVERIFY(prefixStats.glyphs > 0 && prefixStats.glyphs < 80);

        canvas->setX(-aAdvance + 40);
        const OutlineCapture middle = captureOutline(scene);
        QVERIFY(!middle.rendered.isNull());
        QVERIFY2(middle.iou >= 0.88,
                 qPrintable(QStringLiteral("panned viewport IoU too low: %1")
                     .arg(middle.iou)));
        const auto middleStats = scene.outline->statistics();
        QVERIFY(middleStats.glyphs > 0 && middleStats.glyphs < 80);
        QVERIFY2(middleStats.rebuiltChunks > 0,
                 "ancestor pan did not replace the visible glyph geometry");

        canvas->setScale(0.5);
        canvas->setX(-(aAdvance + bAdvance) * canvas->scale() + 40);
        const OutlineCapture zoomedTail = captureOutline(scene);
        QVERIFY(!zoomedTail.rendered.isNull());
        QVERIFY2(zoomedTail.iou >= 0.86,
                 qPrintable(QStringLiteral("zoomed tail viewport IoU too low: %1")
                     .arg(zoomedTail.iou)));
        const auto zoomedStats = scene.outline->statistics();
        QVERIFY(zoomedStats.glyphs > middleStats.glyphs);
        QVERIFY(zoomedStats.glyphs < 160);
    }

    void repeatedPrefixEditsDoNotFragmentChunks()
    {
        QString text;
        const QString phrase = QStringLiteral("ABRACADABRA 0123456789 ");
        while (text.size() < 4096)
            text += phrase;
        text.truncate(4096);

        Scene scene(text, QQuickTextEdit::AlignLeft, 32, 24, {900, 260});
        // This test validates the complete chunk list, independently of the
        // production viewport optimization.
        scene.outline->setFlag(QQuickItem::ItemObservesViewport, false);
        scene.edit->setReadOnly(false);
        scene.edit->setWrapMode(QQuickTextEdit::NoWrap);
        const qreal width = QFontMetricsF(scene.edit->font()).horizontalAdvance(text)
            + 256 * QFontMetricsF(scene.edit->font()).horizontalAdvance(QLatin1Char('W'))
            + 300;
        scene.edit->setSize({width, 100});
        scene.outline->setSize({width + 120, 220});

        const auto verifyBound = [&]() {
            scene.outline->rebuildNow();
            const auto stats = scene.outline->statistics();
            QVERIFY2(stats.glyphs > 3000,
                     qPrintable(QStringLiteral("only %1 glyphs reached chunking")
                         .arg(stats.glyphs)));
            const int idealChunks = (stats.glyphs + 63) / 64;
            QVERIFY2(stats.chunks <= idealChunks * 2 + 1,
                     qPrintable(QStringLiteral("chunk fragmentation: %1 chunks for %2 glyphs")
                         .arg(stats.chunks).arg(stats.glyphs)));
        };

        verifyBound();
        for (int iteration = 0; iteration < 256; ++iteration) {
            scene.edit->insert(0, QStringLiteral("A"));
            verifyBound();
        }

        const int stableLength = scene.edit->length();
        for (int iteration = 0; iteration < 256; ++iteration) {
            scene.edit->remove(0, 1);
            scene.edit->insert(0, iteration % 2 ? QStringLiteral("A")
                                                : QStringLiteral("B"));
            QCOMPARE(scene.edit->length(), stableLength);
            verifyBound();
        }
    }

    void twelveThousandCharacterAppendStaysIncremental_data()
    {
        QTest::addColumn<bool>("viewportCulling");
        QTest::addColumn<bool>("wrapped");
        QTest::newRow("production-visible-glyphs") << true << false;
        QTest::newRow("production-wrapped-tail") << true << true;
        QTest::newRow("all-glyphs-stress") << false << false;
    }

    void twelveThousandCharacterAppendStaysIncremental()
    {
        QFETCH(bool, viewportCulling);
        QFETCH(bool, wrapped);
        QString text;
        const QString phrase = QStringLiteral("MOUFFETTE OUTLINE PERFORMANCE 0123456789 ");
        while (text.size() < 12000)
            text += phrase;
        text.truncate(12000);

        // Keep the render target resources alive until after the Quick window
        // and render control have released their scene-graph references.
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiRenderBuffer> depthStencil;
        std::unique_ptr<QRhiTextureRenderTarget> renderTarget;
        std::unique_ptr<QRhiRenderPassDescriptor> renderPass;
        QQuickRenderControl control;
        QQuickWindow window(&control);
        const QSize targetSize = wrapped ? QSize(1280, 600) : QSize(1280, 300);
        window.setColor(Qt::black);
        window.resize(targetSize);
        window.contentItem()->setSize(targetSize);

        auto* outline = new TestableTextOutlineItem(window.contentItem());
        outline->setFlag(QQuickItem::ItemObservesViewport, viewportCulling);
        outline->setPosition({0, 0});
        outline->setZ(0);
        outline->setColor(Qt::white);
        outline->setOutlinePixels(48);

        auto* edit = new QQuickTextEdit(window.contentItem());
        edit->setPosition({60, 100});
        edit->setZ(1);
        edit->setTextFormat(QQuickTextEdit::PlainText);
        edit->setWrapMode(wrapped ? QQuickTextEdit::Wrap : QQuickTextEdit::NoWrap);
        edit->setHAlign(QQuickTextEdit::AlignLeft);
        edit->setVAlign(QQuickTextEdit::AlignTop);
        edit->setTopPadding(11);
        edit->setLeftPadding(0);
        edit->setRightPadding(0);
        edit->setBottomPadding(0);
        edit->setReadOnly(false);
        edit->setCursorVisible(false);
        edit->setColor(Qt::transparent);
        QFont font(QStringLiteral("Impact"));
        font.setPixelSize(48);
        font.setHintingPreference(QFont::PreferNoHinting);
        font.setKerning(true);
        edit->setFont(font);
        edit->setText(text);
        edit->componentComplete();
        TextEditHelper helper;
        helper.applyIncludeTrailingSpaces(edit);
        outline->setSource(edit);

        if (wrapped) {
            // A realistic long paragraph: fixed editor width, tall document,
            // viewport positioned at the editing tail.
            edit->setSize({1000, 100000});
            edit->ensurePolished();
            const qreal documentHeight = edit->contentHeight();
            const qreal viewportShift = qMax<qreal>(0, documentHeight
                - targetSize.height() + 160);
            edit->setPosition({100, 50 - viewportShift});
            edit->setHeight(documentHeight + 240);
            outline->setPosition({0, -viewportShift});
            outline->setSize({1200, documentHeight + 340});
        } else {
            // Deliberately retain the whole unwrapped line in logical bounds.
            const qreal textAdvance = QFontMetricsF(edit->font()).horizontalAdvance(text);
            const qreal longLineWidth = textAdvance + 300;
            edit->setSize({longLineWidth, 120});
            outline->setSize({longLineWidth + 120, 280});
            const qreal viewportShift = qMax<qreal>(
                0, textAdvance - targetSize.width() + 180);
            edit->setX(60 - viewportShift);
            outline->setX(-viewportShift);
        }
        // Production paints the native TextEdit fill and the outline together.
        // The all-glyph row keeps it transparent to isolate the outline stress.
        edit->setColor(viewportCulling ? Qt::white : Qt::transparent);

        QVERIFY2(control.initialize(), "QQuickRenderControl initialization failed");
        QRhi* rhi = control.rhi();
        QVERIFY2(rhi, "QQuickRenderControl did not provide a QRhi");
        texture.reset(rhi->newTexture(QRhiTexture::RGBA8, targetSize, 1,
                                      QRhiTexture::RenderTarget));
        QVERIFY2(texture && texture->create(), "could not create QRhi color texture");
        depthStencil.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil,
                                                targetSize, 1));
        QVERIFY2(depthStencil && depthStencil->create(),
                 "could not create QRhi depth/stencil buffer");
        QRhiTextureRenderTargetDescription targetDescription(
            QRhiColorAttachment(texture.get()));
        targetDescription.setDepthStencilBuffer(depthStencil.get());
        renderTarget.reset(rhi->newTextureRenderTarget(targetDescription));
        QVERIFY(renderTarget);
        renderPass.reset(renderTarget->newCompatibleRenderPassDescriptor());
        QVERIFY(renderPass);
        renderTarget->setRenderPassDescriptor(renderPass.get());
        QVERIFY2(renderTarget->create(), "could not create QRhi render target");
        window.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(renderTarget.get()));

        struct FrameTimings {
            qint64 totalUs = 0;
            qint64 quickPolishUs = 0;
            qint64 quickSyncUs = 0;
            qint64 renderAndGpuUs = 0;
            bool sceneChanged = false;
        };
        const auto renderFrame = [&]() {
            FrameTimings timings;
            QElapsedTimer totalTimer;
            QElapsedTimer phaseTimer;
            totalTimer.start();
            phaseTimer.start();
            control.polishItems();
            timings.quickPolishUs = phaseTimer.nsecsElapsed() / 1000;
            control.beginFrame();
            phaseTimer.restart();
            timings.sceneChanged = control.sync();
            timings.quickSyncUs = phaseTimer.nsecsElapsed() / 1000;
            phaseTimer.restart();
            control.render();
            control.endFrame();
            rhi->finish();
            timings.renderAndGpuUs = phaseTimer.nsecsElapsed() / 1000;
            timings.totalUs = totalTimer.nsecsElapsed() / 1000;
            return timings;
        };

        // Create all cached glyph meshes and the initial scene graph before
        // measuring edits. QQuickRenderControl's offscreen frame is deterministic
        // and complete at endFrame(), without a window presentation or readback.
        const FrameTimings initialFrame = renderFrame();
        QVERIFY(initialFrame.sceneChanged);
        const auto initial = outline->statistics();
        if (viewportCulling) {
            const int maximumVisibleGlyphs = wrapped ? 600 : 200;
            QVERIFY2(initial.glyphs > 0 && initial.glyphs < maximumVisibleGlyphs,
                     qPrintable(QStringLiteral(
                         "viewport culling processed an unexpected %1 glyphs")
                         .arg(initial.glyphs)));
            QVERIFY(initial.chunks <= (wrapped ? 10 : 4));
        } else {
            QVERIFY2(initial.glyphs > 9000,
                     qPrintable(QStringLiteral(
                         "all-glyph stress did not process the long text: %1 glyphs")
                         .arg(initial.glyphs)));
            QVERIFY(initial.chunks > 100);
        }
        qInfo() << "initial 12k outline: glyphs/chunks/triangles/generated/rebuilt/moved"
                << initial.glyphs << initial.chunks << initial.triangles
                << initial.generatedGlyphs << initial.rebuiltChunks << initial.movedChunks
                << "; Quick polish/sync/render+GPU" << initialFrame.quickPolishUs
                << initialFrame.quickSyncUs << initialFrame.renderAndGpuUs << "us";

        // Record the first edit separately. The outer timer deliberately starts
        // before insert(): this is the observable typing-to-completed-frame cost.
        QCoreApplication::processEvents();
        QElapsedTimer coldTimer;
        coldTimer.start();
        const int beforeColdLength = edit->length();
        edit->insert(edit->length(), QStringLiteral("A"));
        QCOMPARE(edit->length(), beforeColdLength + 1);
        const FrameTimings coldFrame = renderFrame();
        const qint64 coldUs = coldTimer.nsecsElapsed() / 1000;
        const auto coldStats = outline->statistics();
        QCOMPARE(coldStats.generatedGlyphs, 0);
        QVERIFY(coldStats.rebuiltChunks <= 2);
        qInfo() << "cold 12k append before threshold:" << coldUs
                << "us; rebuilt/moved" << coldStats.rebuiltChunks << coldStats.movedChunks
                << "; triangles" << coldStats.triangles
                << "; outline polish/sync" << coldStats.polishNanoseconds / 1000
                << coldStats.syncNanoseconds / 1000
                << "; Quick polish/sync/render+GPU" << coldFrame.quickPolishUs
                << coldFrame.quickSyncUs << coldFrame.renderAndGpuUs << "us";
        QVERIFY2(coldUs < 750000,
                 qPrintable(QStringLiteral("cold 12k-character append took %1 us").arg(coldUs)));

        QList<qint64> elapsedUs;
        QList<qint64> polishUs;
        QList<qint64> syncUs;
        QList<qint64> quickPolishUs;
        QList<qint64> quickSyncUs;
        QList<qint64> renderAndGpuUs;
        for (int iteration = 0; iteration < 9; ++iteration) {
            // Real keystrokes arrive on separate event-loop iterations. Let
            // Qt/platform deferred releases run between explicit offscreen
            // frames (notably Metal autoreleased resources), outside timing.
            QCoreApplication::processEvents();
            QElapsedTimer timer;
            timer.start();
            const int beforeLength = edit->length();
            edit->insert(edit->length(), QStringLiteral("A"));
            QCOMPARE(edit->length(), beforeLength + 1);
            const FrameTimings frame = renderFrame();
            elapsedUs.append(timer.nsecsElapsed() / 1000);

            const auto stats = outline->statistics();
            polishUs.append(stats.polishNanoseconds / 1000);
            syncUs.append(stats.syncNanoseconds / 1000);
            quickPolishUs.append(frame.quickPolishUs);
            quickSyncUs.append(frame.quickSyncUs);
            renderAndGpuUs.append(frame.renderAndGpuUs);
            QCOMPARE(stats.generatedGlyphs, 0);
            QVERIFY2(stats.rebuiltChunks <= 2,
                     qPrintable(QStringLiteral("append rebuilt %1 chunks out of %2")
                         .arg(stats.rebuiltChunks).arg(stats.chunks)));
            QVERIFY2(stats.movedChunks <= 2,
                     qPrintable(QStringLiteral("append moved %1 chunks out of %2")
                         .arg(stats.movedChunks).arg(stats.chunks)));
        }
        std::sort(elapsedUs.begin(), elapsedUs.end());
        std::sort(polishUs.begin(), polishUs.end());
        std::sort(syncUs.begin(), syncUs.end());
        std::sort(quickPolishUs.begin(), quickPolishUs.end());
        std::sort(quickSyncUs.begin(), quickSyncUs.end());
        std::sort(renderAndGpuUs.begin(), renderAndGpuUs.end());
        const qint64 p95Us = elapsedUs.constLast();
        qInfo() << (wrapped ? "production wrapped viewport"
                            : viewportCulling ? "production viewport-culling"
                                              : "all-glyph stress")
                << "12k-character 100%-outline cold edit-to-GPU:" << coldUs << "us;"
                << "steady p95:" << p95Us << "us; samples:" << elapsedUs
                << "outline polish p95:" << polishUs.constLast() << "us;"
                << "outline sync p95:" << syncUs.constLast() << "us;"
                << "Quick polish/sync/render+GPU p95:"
                << quickPolishUs.constLast() << quickSyncUs.constLast()
                << renderAndGpuUs.constLast() << "us;"
                << "cold Quick polish/sync/render+GPU:"
                << coldFrame.quickPolishUs << coldFrame.quickSyncUs
                << coldFrame.renderAndGpuUs << "us;"
                << "cold outline polish/sync:" << coldStats.polishNanoseconds / 1000
                << coldStats.syncNanoseconds / 1000 << "us";
        const qint64 steadyLimitUs = viewportCulling ? 50000 : 250000;
        QVERIFY2(p95Us < steadyLimitUs,
                 qPrintable(QStringLiteral("12k-character append took %1 us").arg(p95Us)));

        const auto verifyInsertion = [&](int position, const char* description) {
            QCoreApplication::processEvents();
            QElapsedTimer timer;
            timer.start();
            const int beforeLength = edit->length();
            edit->insert(position, QStringLiteral("A"));
            QCOMPARE(edit->length(), beforeLength + 1);
            const FrameTimings frame = renderFrame();
            const qint64 elapsed = timer.nsecsElapsed() / 1000;
            const auto stats = outline->statistics();
            QCOMPARE(stats.generatedGlyphs, 0);
            const int maximumRebuiltChunks = wrapped ? 10 : 3;
            QVERIFY2(stats.rebuiltChunks <= maximumRebuiltChunks,
                     qPrintable(QStringLiteral("%1 rebuilt %2 chunks out of %3")
                         .arg(QString::fromLatin1(description))
                         .arg(stats.rebuiltChunks).arg(stats.chunks)));
            QVERIFY2(elapsed < 500000,
                     qPrintable(QStringLiteral("%1 took %2 us")
                         .arg(QString::fromLatin1(description)).arg(elapsed)));
            qInfo() << description << elapsed << "us; rebuilt chunks"
                    << stats.rebuiltChunks << '/' << stats.chunks
                    << "; outline polish/sync" << stats.polishNanoseconds / 1000
                    << stats.syncNanoseconds / 1000
                    << "; Quick polish/sync/render+GPU" << frame.quickPolishUs
                    << frame.quickSyncUs << frame.renderAndGpuUs << "us";
        };
        verifyInsertion(0, "12k insertion at start");
        verifyInsertion(edit->length() / 2, "12k insertion in middle");

        // A color change must be material-only: no glyph or chunk geometry rebuild.
        outline->setColor(Qt::red);
        renderFrame();
        const auto recolored = outline->statistics();
        QCOMPARE(recolored.generatedGlyphs, 0);
        QCOMPARE(recolored.rebuiltChunks, 0);

        window.setRenderTarget({});
        control.invalidate();
    }
};

int main(int argc, char** argv)
{
#ifdef Q_OS_MACOS
    // This is the production backend on macOS. Set it before the first window,
    // so a passing pixel test cannot silently exercise the software renderer.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#endif
    QGuiApplication application(argc, argv);
    TextOutlineItemTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_TextOutlineItem.moc"

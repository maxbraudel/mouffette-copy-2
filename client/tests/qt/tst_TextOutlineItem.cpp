#include "frontend/rendering/canvas/TextEditHelper.h"
#include "frontend/rendering/canvas/TextOutlineItem.h"

#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPathStroker>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
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

bool waitForFrame(QSignalSpy& frames, int timeoutMs = renderTimeoutMs)
{
    return !frames.isEmpty() || frames.wait(timeoutMs);
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

    void twelveThousandCharacterAppendStaysIncremental()
    {
        QString text;
        const QString phrase = QStringLiteral("MOUFFETTE OUTLINE PERFORMANCE 0123456789 ");
        while (text.size() < 12000)
            text += phrase;
        text.truncate(12000);

        Scene scene(text, QQuickTextEdit::AlignLeft, 48, 48, {1280, 300});
        // Deliberately keep the whole long line inside the outline item's logical
        // bounds. This exercises all 12k glyph placements, not just viewport culling.
        scene.edit->setPosition({60, 100});
        scene.edit->setWrapMode(QQuickTextEdit::NoWrap);
        scene.edit->setReadOnly(false);
        const qreal textAdvance = QFontMetricsF(scene.edit->font()).horizontalAdvance(text);
        const qreal longLineWidth = textAdvance + 300;
        scene.edit->setSize({longLineWidth, 120});
        scene.outline->setSize({longLineWidth + 120, 280});
        // Keep the edited tail inside the actual window while retaining every
        // glyph in the outline item's logical bounds. Otherwise Qt correctly
        // suppresses frames for changes wholly outside the exposed viewport.
        const qreal viewportShift = qMax<qreal>(0, textAdvance - scene.window.width() + 180);
        scene.edit->setX(60 - viewportShift);
        scene.outline->setX(-viewportShift);
        QVERIFY2(scene.expose(), "The Qt Quick window could not be exposed");

        QSignalSpy frames(&scene.window, &QQuickWindow::frameSwapped);
        QVERIFY(frames.isValid());
        scene.outline->rebuildNow();
        scene.outline->update();
        scene.window.update();
        QVERIFY2(waitForFrame(frames), "no initial scene-graph frame");
        // Drain queued notifications from initial scene-graph/material creation,
        // so the cold measurement is tied to the first edit, not a prior frame.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        frames.clear();

        const auto initial = scene.outline->statistics();
        QVERIFY2(initial.glyphs > 9000,
                 qPrintable(QStringLiteral("benchmark did not process the long text: %1 glyphs")
                     .arg(initial.glyphs)));
        QVERIFY(initial.chunks > 100);

        // Record cold first-edit latency separately: the first changed geometry
        // may cause Metal to compile/upload resources that steady typing reuses.
        frames.clear();
        QElapsedTimer coldTimer;
        coldTimer.start();
        const int beforeColdLength = scene.edit->length();
        scene.edit->insert(scene.edit->length(), QStringLiteral("A"));
        QCOMPARE(scene.edit->length(), beforeColdLength + 1);
        scene.outline->rebuildNow();
        scene.outline->update();
        scene.window.update();
        QVERIFY2(waitForFrame(frames), "no frame after cold append");
        const qint64 coldMs = coldTimer.elapsed();
        const auto coldStats = scene.outline->statistics();
        QCOMPARE(coldStats.generatedGlyphs, 0);
        QVERIFY(coldStats.rebuiltChunks <= 2);
        QVERIFY2(coldMs < 750,
                 qPrintable(QStringLiteral("cold 12k-character append took %1 ms").arg(coldMs)));

        QList<qint64> elapsedMs;
        QList<qint64> polishUs;
        QList<qint64> syncUs;
        for (int iteration = 0; iteration < 9; ++iteration) {
            frames.clear();
            QElapsedTimer timer;
            timer.start();
            const int beforeLength = scene.edit->length();
            scene.edit->insert(scene.edit->length(), QStringLiteral("A"));
            QCOMPARE(scene.edit->length(), beforeLength + 1);
            scene.outline->rebuildNow();
            scene.outline->update();
            scene.window.update();
            QVERIFY2(waitForFrame(frames), "no frame after steady append");
            elapsedMs.append(timer.elapsed());

            const auto stats = scene.outline->statistics();
            polishUs.append(stats.polishNanoseconds / 1000);
            syncUs.append(stats.syncNanoseconds / 1000);
            QCOMPARE(stats.generatedGlyphs, 0);
            QVERIFY2(stats.rebuiltChunks <= 2,
                     qPrintable(QStringLiteral("append rebuilt %1 chunks out of %2")
                         .arg(stats.rebuiltChunks).arg(stats.chunks)));
        }
        std::sort(elapsedMs.begin(), elapsedMs.end());
        std::sort(polishUs.begin(), polishUs.end());
        std::sort(syncUs.begin(), syncUs.end());
        const qint64 p95 = elapsedMs[elapsedMs.size() - 1];
        qInfo() << "12k-character 100%-outline cold append:" << coldMs << "ms;"
                << "steady p95:" << p95 << "ms; samples:" << elapsedMs
                << "outline polish p95:" << polishUs.constLast() << "us;"
                << "outline sync p95:" << syncUs.constLast() << "us;"
                << "cold outline polish/sync:" << coldStats.polishNanoseconds / 1000
                << coldStats.syncNanoseconds / 1000 << "us";
        QVERIFY2(p95 < 250,
                 qPrintable(QStringLiteral("12k-character append took %1 ms").arg(p95)));

        const auto verifyInsertion = [&](int position, const char* description) {
            frames.clear();
            QElapsedTimer timer;
            timer.start();
            const int beforeLength = scene.edit->length();
            scene.edit->insert(position, QStringLiteral("A"));
            QCOMPARE(scene.edit->length(), beforeLength + 1);
            scene.outline->rebuildNow();
            scene.outline->update();
            scene.window.update();
            const bool frameRendered = waitForFrame(frames);
            const qint64 elapsed = timer.elapsed();
            QVERIFY2(frameRendered, description);
            const auto stats = scene.outline->statistics();
            QCOMPARE(stats.generatedGlyphs, 0);
            QVERIFY2(stats.rebuiltChunks <= 3,
                     qPrintable(QStringLiteral("%1 rebuilt %2 chunks out of %3")
                         .arg(QString::fromLatin1(description))
                         .arg(stats.rebuiltChunks).arg(stats.chunks)));
            QVERIFY2(elapsed < 500,
                     qPrintable(QStringLiteral("%1 took %2 ms")
                         .arg(QString::fromLatin1(description)).arg(elapsed)));
            qInfo() << description << elapsed << "ms; rebuilt chunks"
                    << stats.rebuiltChunks << '/' << stats.chunks;
        };
        verifyInsertion(0, "12k insertion at start");
        verifyInsertion(scene.edit->length() / 2, "12k insertion in middle");

        // A color change must be material-only: no glyph or chunk geometry rebuild.
        scene.edit->setColor(Qt::transparent);
        scene.outline->setColor(Qt::red);
        const QImage recoloredImage = grabAfterSync(scene.window, scene.outline);
        QVERIFY(!recoloredImage.isNull());
        const auto recolored = scene.outline->statistics();
        QCOMPARE(recolored.generatedGlyphs, 0);
        QCOMPARE(recolored.rebuiltChunks, 0);

        const QImage rgba = recoloredImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        int peakRed = 0;
        int peakGreen = 0;
        for (int y = 0; y < rgba.height(); ++y) {
            const auto* line = reinterpret_cast<const QRgb*>(rgba.constScanLine(y));
            for (int x = 0; x < rgba.width(); ++x) {
                peakRed = qMax(peakRed, qRed(line[x]));
                peakGreen = qMax(peakGreen, qGreen(line[x]));
            }
        }
        QCOMPARE(peakRed, 255);
        QVERIFY2(peakGreen <= 5,
                 qPrintable(QStringLiteral("transparent source leaked into outline: %1")
                     .arg(peakGreen)));
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

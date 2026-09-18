#include "backend/domain/media/TextRenderState.h"

#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QScreen>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>

namespace TextRenderMetrics {

int effectiveFontPixelSize(const QFont& font, qreal uniformScale) {
    qreal pixelSize = 0.0;
    if (font.pixelSize() > 0) {
        pixelSize = font.pixelSize();
    } else if (font.pointSizeF() > 0.0) {
        qreal logicalDpiY = 96.0;
        if (QScreen* screen = QGuiApplication::primaryScreen()) {
            logicalDpiY = std::max<qreal>(screen->logicalDotsPerInchY(), 1.0);
        }
        pixelSize = font.pointSizeF() * logicalDpiY / 72.0;
    } else {
        const QFontInfo info(font);
        if (info.pixelSize() > 0) {
            pixelSize = info.pixelSize();
        } else if (info.pointSizeF() > 0.0) {
            qreal logicalDpiY = 96.0;
            if (QScreen* screen = QGuiApplication::primaryScreen()) {
                logicalDpiY = std::max<qreal>(screen->logicalDotsPerInchY(), 1.0);
            }
            pixelSize = info.pointSizeF() * logicalDpiY / 72.0;
        }
    }

    if (!std::isfinite(pixelSize) || pixelSize <= 0.0) {
        pixelSize = 12.0;
    }
    if (!std::isfinite(uniformScale) || std::abs(uniformScale) < 1e-4) {
        uniformScale = 1.0;
    }
    return std::max(1, qRound(pixelSize * std::abs(uniformScale)));
}

qreal outlinePixels(const QFont& font, qreal widthPercent, qreal uniformScale) {
    return outlinePixels(widthPercent, effectiveFontPixelSize(font, uniformScale));
}

qreal outlineSafetyPadding(qreal outlineWidthPixels) {
    if (!std::isfinite(outlineWidthPixels) || outlineWidthPixels <= 0.0) {
        return 0.0;
    }
    // One device-independent pixel covers antialiasing at the outer edge.
    return std::ceil(outlineWidthPixels) + 1.0;
}

QSize fittedTextSize(const TextRenderState& state) {
    QFont font;
    font.setFamily(state.fontFamily.isEmpty()
                       ? QStringLiteral("Impact") : state.fontFamily);
    font.setPixelSize(std::max(1, state.fontPixelSize));
    font.setWeight(static_cast<QFont::Weight>(
        std::clamp(state.fontWeight, 1, 1000)));
    font.setItalic(state.italic);
    font.setUnderline(state.underline);
    font.setCapitalization(state.uppercase
                               ? QFont::AllUppercase : QFont::MixedCase);
    font.setKerning(true);
    font.setHintingPreference(QFont::PreferNoHinting);

    QString normalized = state.text;
    normalized.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setUseDesignMetrics(true);

    const QFontMetricsF metrics(font);
    const qreal emptyLineHeight = std::ceil(
        metrics.ascent() + metrics.descent() + metrics.leading());
    qreal contentWidth = 0.0;
    qreal contentHeight = 0.0;
    const QStringList paragraphs = normalized.split(QLatin1Char('\n'));
    for (const QString& paragraph : paragraphs) {
        if (paragraph.isEmpty()) {
            contentHeight += emptyLineHeight;
            continue;
        }
        QTextLayout layout(paragraph, font);
        layout.setTextOption(option);
        layout.beginLayout();
        qreal paragraphHeight = 0.0;
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(1000000.0);
            line.setPosition(QPointF(0.0, paragraphHeight));
            contentWidth = std::max(contentWidth, line.naturalTextWidth());
            paragraphHeight += std::ceil(
                line.ascent() + line.descent() + line.leading());
        }
        layout.endLayout();
        contentHeight += paragraphHeight > 0.0
            ? paragraphHeight : emptyLineHeight;
    }

    contentWidth = std::max<qreal>(1.0, contentWidth);
    contentHeight = std::max<qreal>(1.0, contentHeight);
    const qreal outline = state.outlineWidthPixels > 0.0
        ? state.outlineWidthPixels
        : outlinePixels(state.outlineWidthPercent, state.fontPixelSize);
    const qreal inset = ContentMarginPx + outlineSafetyPadding(outline);
    constexpr qreal minimumContentWidth = 24.0;
    return QSize(
        std::max(1, static_cast<int>(std::ceil(
            std::max(minimumContentWidth, contentWidth) + inset * 2.0))),
        std::max(1, static_cast<int>(std::ceil(contentHeight + inset * 2.0))));
}

} // namespace TextRenderMetrics

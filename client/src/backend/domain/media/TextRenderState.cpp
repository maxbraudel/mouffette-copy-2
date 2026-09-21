#include "backend/domain/media/TextRenderState.h"

#include <QFontInfo>
#include <QGuiApplication>
#include <QScreen>

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

    // QTextDocument is also the layout authority inside the live TextEdit.
    // Reimplementing paragraphs with QTextLayout misses document semantics
    // such as Unicode paragraph separators and fallback-font line metrics.
    QTextDocument document;
    configureTextDocumentLayout(document);
    document.setDefaultFont(font);
    QTextOption option = document.defaultTextOption();
    option.setWrapMode(QTextOption::NoWrap);
    document.setDefaultTextOption(option);
    document.setPlainText(state.text);

    const QSizeF documentSize = document.size();
    const qreal contentWidth = std::max<qreal>(1.0, document.idealWidth());
    const qreal contentHeight = std::max<qreal>(1.0, documentSize.height());
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

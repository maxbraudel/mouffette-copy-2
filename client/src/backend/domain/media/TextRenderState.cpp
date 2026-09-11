#include "backend/domain/media/TextRenderState.h"

#include "backend/domain/media/TextMediaItem.h"

#include <QFontInfo>
#include <QGuiApplication>
#include <QScreen>

#include <algorithm>
#include <cmath>

namespace {

QString horizontalAlignmentToString(TextMediaItem::HorizontalAlignment alignment) {
    switch (alignment) {
    case TextMediaItem::HorizontalAlignment::Left:
        return QStringLiteral("left");
    case TextMediaItem::HorizontalAlignment::Right:
        return QStringLiteral("right");
    case TextMediaItem::HorizontalAlignment::Center:
        return QStringLiteral("center");
    }
    return QStringLiteral("center");
}

QString verticalAlignmentToString(TextMediaItem::VerticalAlignment alignment) {
    switch (alignment) {
    case TextMediaItem::VerticalAlignment::Top:
        return QStringLiteral("top");
    case TextMediaItem::VerticalAlignment::Bottom:
        return QStringLiteral("bottom");
    case TextMediaItem::VerticalAlignment::Center:
        return QStringLiteral("center");
    }
    return QStringLiteral("center");
}

} // namespace

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

qreal outlinePixels(qreal widthPercent, int fontPixelSize) {
    if (!std::isfinite(widthPercent) || widthPercent <= 0.0) {
        return 0.0;
    }
    const qreal pixels = widthPercent * std::max(1, fontPixelSize) / 100.0;
    if (!std::isfinite(pixels)) {
        return 0.0;
    }
    return std::max<qreal>(1.0, std::round(pixels));
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

TextRenderState fromMediaItem(const TextMediaItem& item) {
    TextRenderState state;
    const QFont font = item.font();
    const QFontInfo resolvedFont(font);
    state.text = item.text();
    // QFont::family() is the requested family. QFontInfo reports the face Qt
    // actually selected, which is the only family that can keep TextEdit and
    // QRawFont glyph metrics aligned when the requested face is unavailable.
    state.fontFamily = resolvedFont.family().isEmpty()
        ? font.family() : resolvedFont.family();
    if (state.fontFamily.isEmpty()) {
        state.fontFamily = QStringLiteral("Impact");
    }
    state.fontPixelSize = effectiveFontPixelSize(font, item.uniformScaleFactor());
    state.fontWeight = std::clamp(item.textFontWeightValue(), 100, 900);
    state.italic = item.italicEnabled();
    state.underline = item.underlineEnabled();
    state.uppercase = item.uppercaseEnabled();
    state.horizontalAlignment = horizontalAlignmentToString(item.horizontalAlignment());
    state.verticalAlignment = verticalAlignmentToString(item.verticalAlignment());
    state.fitToTextEnabled = item.fitToTextEnabled();
    state.textColor = item.textColor();
    state.outlineWidthPercent = item.textBorderWidth();
    state.outlineWidthPixels = outlinePixels(state.outlineWidthPercent, state.fontPixelSize);
    state.outlineColor = item.textBorderColor();
    state.highlightEnabled = item.highlightEnabled();
    state.highlightColor = item.highlightColor();
    return state;
}

} // namespace TextRenderMetrics

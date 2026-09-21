#pragma once

#include <QColor>
#include <QFont>
#include <QSize>
#include <QString>
#include <QTextDocument>
#include <QTextOption>
#include <algorithm>
#include <cmath>

// Canonical, renderer-agnostic text state. Values are expressed in the same
// logical pixels consumed by the Qt Quick TextItem before the outer media
// transform is applied.
struct TextRenderState {
    QString text;
    QString fontFamily;
    int fontPixelSize = 1;
    int fontWeight = 400;
    bool italic = false;
    bool underline = false;
    bool uppercase = false;
    QString horizontalAlignment = QStringLiteral("center");
    QString verticalAlignment = QStringLiteral("center");
    bool fitToTextEnabled = false;
    QColor textColor;
    qreal outlineWidthPercent = 0.0;
    qreal outlineWidthPixels = 0.0;
    QColor outlineColor;
    bool highlightEnabled = false;
    QColor highlightColor;
};

namespace TextRenderMetrics {

constexpr qreal ContentMarginPx = 4.0;
constexpr qreal HighlightPaddingPx = 2.0;

// Fit measurement and the live TextEdit must use the same document rules.
// Wrap mode and alignment belong to the caller and are preserved here.
inline void configureTextDocumentLayout(QTextDocument& document) {
    document.setDocumentMargin(0.0);
    document.setUseDesignMetrics(true);
    QTextOption option = document.defaultTextOption();
    option.setUseDesignMetrics(true);
    option.setFlags(option.flags() | QTextOption::IncludeTrailingSpaces);
    document.setDefaultTextOption(option);
}

int effectiveFontPixelSize(const QFont& font, qreal uniformScale = 1.0);
inline qreal outlinePixels(qreal widthPercent, int fontPixelSize) {
    if (!std::isfinite(widthPercent) || widthPercent <= 0.0) return 0.0;
    const qreal pixels = widthPercent * std::max(1, fontPixelSize) / 100.0;
    if (!std::isfinite(pixels)) return 0.0;
    return std::max<qreal>(1.0, std::round(pixels));
}
qreal outlinePixels(const QFont& font, qreal widthPercent, qreal uniformScale = 1.0);
qreal outlineSafetyPadding(qreal outlineWidthPixels);
QSize fittedTextSize(const TextRenderState& state);

} // namespace TextRenderMetrics

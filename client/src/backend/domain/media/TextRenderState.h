#pragma once

#include <QColor>
#include <QFont>
#include <QSize>
#include <QString>

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

int effectiveFontPixelSize(const QFont& font, qreal uniformScale = 1.0);
qreal outlinePixels(qreal widthPercent, int effectiveFontPixelSize);
qreal outlinePixels(const QFont& font, qreal widthPercent, qreal uniformScale = 1.0);
qreal outlineSafetyPadding(qreal outlineWidthPixels);
QSize fittedTextSize(const TextRenderState& state);

} // namespace TextRenderMetrics

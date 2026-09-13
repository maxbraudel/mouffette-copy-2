#pragma once

#include <QJsonObject>
#include <QString>

// Raw editor values are kept as text on purpose: project restoration must
// preserve exactly what the user typed, while the runtime consumes normalized
// numeric values through the helpers below.
struct MediaSettingsState {
    bool displayAutomatically = true;
    bool displayDelayEnabled = false;
    QString displayDelayText = QStringLiteral("1");
    bool unmuteAutomatically = true;
    bool unmuteDelayEnabled = false;
    QString unmuteDelayText = QStringLiteral("0");
    bool playAutomatically = true;
    bool playDelayEnabled = false;
    QString playDelayText = QStringLiteral("1");
    bool pauseDelayEnabled = false;
    QString pauseDelayText = QStringLiteral("1");
    bool repeatEnabled = false;
    QString repeatCountText = QStringLiteral("1");
    bool fadeInEnabled = false;
    QString fadeInText = QStringLiteral("1");
    bool fadeOutEnabled = false;
    QString fadeOutText = QStringLiteral("1");
    bool audioFadeInEnabled = false;
    QString audioFadeInText = QStringLiteral("1");
    bool audioFadeOutEnabled = false;
    QString audioFadeOutText = QStringLiteral("1");
    bool opacityOverrideEnabled = false;
    QString opacityText = QStringLiteral("100");
    bool volumeOverrideEnabled = false;
    QString volumeText = QStringLiteral("100");
    bool hideDelayEnabled = false;
    QString hideDelayText = QStringLiteral("1");
    bool hideWhenVideoEnds = false;
    bool muteDelayEnabled = false;
    QString muteDelayText = QStringLiteral("1");
    bool muteWhenVideoEnds = false;
};

namespace MediaSettingsSerialization {
QJsonObject toProjectJson(const MediaSettingsState& settings);
bool fromProjectJson(const QJsonObject& object, MediaSettingsState* settings);
int delayMilliseconds(bool enabled, const QString& secondsText);
double durationSeconds(bool enabled, const QString& secondsText);
}

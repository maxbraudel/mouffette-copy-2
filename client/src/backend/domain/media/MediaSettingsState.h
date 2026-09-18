#pragma once
#include <QJsonObject>
#include <QString>

// Intrinsic element controls. Scene behavior belongs exclusively to SceneTimeline.
struct MediaSettingsState {
    bool opacityOverrideEnabled = false;
    QString opacityText = QStringLiteral("100");
    bool volumeOverrideEnabled = false;
    QString volumeText = QStringLiteral("100");
};
namespace MediaSettingsSerialization {
QJsonObject toProjectJson(const MediaSettingsState& settings);
bool fromProjectJson(const QJsonObject& object, MediaSettingsState* settings);
}

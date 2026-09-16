#include "backend/domain/media/MediaSettingsState.h"

#include <QtGlobal>
#include <cmath>

namespace {
constexpr int kSchemaVersion = 1;
}

QJsonObject MediaSettingsSerialization::toProjectJson(
    const MediaSettingsState& s)
{
    return {
        {QStringLiteral("schemaVersion"), kSchemaVersion},
        {QStringLiteral("displayAutomatically"), s.displayAutomatically},
        {QStringLiteral("displayDelayEnabled"), s.displayDelayEnabled},
        {QStringLiteral("displayDelayText"), s.displayDelayText},
        {QStringLiteral("unmuteAutomatically"), s.unmuteAutomatically},
        {QStringLiteral("unmuteDelayEnabled"), s.unmuteDelayEnabled},
        {QStringLiteral("unmuteDelayText"), s.unmuteDelayText},
        {QStringLiteral("playAutomatically"), s.playAutomatically},
        {QStringLiteral("playDelayEnabled"), s.playDelayEnabled},
        {QStringLiteral("playDelayText"), s.playDelayText},
        {QStringLiteral("pauseDelayEnabled"), s.pauseDelayEnabled},
        {QStringLiteral("pauseDelayText"), s.pauseDelayText},
        {QStringLiteral("repeatEnabled"), s.repeatEnabled},
        {QStringLiteral("repeatCountText"), s.repeatCountText},
        {QStringLiteral("fadeInEnabled"), s.fadeInEnabled},
        {QStringLiteral("fadeInText"), s.fadeInText},
        {QStringLiteral("fadeOutEnabled"), s.fadeOutEnabled},
        {QStringLiteral("fadeOutText"), s.fadeOutText},
        {QStringLiteral("audioFadeInEnabled"), s.audioFadeInEnabled},
        {QStringLiteral("audioFadeInText"), s.audioFadeInText},
        {QStringLiteral("audioFadeOutEnabled"), s.audioFadeOutEnabled},
        {QStringLiteral("audioFadeOutText"), s.audioFadeOutText},
        {QStringLiteral("opacityOverrideEnabled"), s.opacityOverrideEnabled},
        {QStringLiteral("opacityText"), s.opacityText},
        {QStringLiteral("volumeOverrideEnabled"), s.volumeOverrideEnabled},
        {QStringLiteral("volumeText"), s.volumeText},
        {QStringLiteral("hideDelayEnabled"), s.hideDelayEnabled},
        {QStringLiteral("hideDelayText"), s.hideDelayText},
        {QStringLiteral("hideWhenVideoEnds"), s.hideWhenVideoEnds},
        {QStringLiteral("muteDelayEnabled"), s.muteDelayEnabled},
        {QStringLiteral("muteDelayText"), s.muteDelayText},
        {QStringLiteral("muteWhenVideoEnds"), s.muteWhenVideoEnds}
    };
}

bool MediaSettingsSerialization::fromProjectJson(
    const QJsonObject& object, MediaSettingsState* settings)
{
    if (!settings
        || object.value(QStringLiteral("schemaVersion")).toInt(-1)
            != kSchemaVersion) {
        return false;
    }
    MediaSettingsState restored;
    const auto readBool = [&object](const char* name, bool* output) {
        const QJsonValue value = object.value(QString::fromLatin1(name));
        if (!value.isBool()) return false;
        *output = value.toBool();
        return true;
    };
    const auto readString = [&object](const char* name, QString* output) {
        const QJsonValue value = object.value(QString::fromLatin1(name));
        if (!value.isString()) return false;
        *output = value.toString();
        return true;
    };
    if (!readBool("displayAutomatically", &restored.displayAutomatically)
        || !readBool("displayDelayEnabled", &restored.displayDelayEnabled)
        || !readString("displayDelayText", &restored.displayDelayText)
        || !readBool("unmuteAutomatically", &restored.unmuteAutomatically)
        || !readBool("unmuteDelayEnabled", &restored.unmuteDelayEnabled)
        || !readString("unmuteDelayText", &restored.unmuteDelayText)
        || !readBool("playAutomatically", &restored.playAutomatically)
        || !readBool("playDelayEnabled", &restored.playDelayEnabled)
        || !readString("playDelayText", &restored.playDelayText)
        || !readBool("pauseDelayEnabled", &restored.pauseDelayEnabled)
        || !readString("pauseDelayText", &restored.pauseDelayText)
        || !readBool("repeatEnabled", &restored.repeatEnabled)
        || !readString("repeatCountText", &restored.repeatCountText)
        || !readBool("fadeInEnabled", &restored.fadeInEnabled)
        || !readString("fadeInText", &restored.fadeInText)
        || !readBool("fadeOutEnabled", &restored.fadeOutEnabled)
        || !readString("fadeOutText", &restored.fadeOutText)
        || !readBool("audioFadeInEnabled", &restored.audioFadeInEnabled)
        || !readString("audioFadeInText", &restored.audioFadeInText)
        || !readBool("audioFadeOutEnabled", &restored.audioFadeOutEnabled)
        || !readString("audioFadeOutText", &restored.audioFadeOutText)
        || !readBool("opacityOverrideEnabled", &restored.opacityOverrideEnabled)
        || !readString("opacityText", &restored.opacityText)
        || !readBool("volumeOverrideEnabled", &restored.volumeOverrideEnabled)
        || !readString("volumeText", &restored.volumeText)
        || !readBool("hideDelayEnabled", &restored.hideDelayEnabled)
        || !readString("hideDelayText", &restored.hideDelayText)
        || !readBool("hideWhenVideoEnds", &restored.hideWhenVideoEnds)
        || !readBool("muteDelayEnabled", &restored.muteDelayEnabled)
        || !readString("muteDelayText", &restored.muteDelayText)
        || !readBool("muteWhenVideoEnds", &restored.muteWhenVideoEnds)) {
        return false;
    }
    *settings = restored;
    return true;
}

double MediaSettingsSerialization::durationSeconds(
    bool enabled, const QString& secondsText)
{
    if (!enabled) return 0.0;
    bool ok = false;
    const double seconds = secondsText.trimmed().toDouble(&ok);
    return ok && std::isfinite(seconds) ? qBound(0.0, seconds, 3600.0) : 0.0;
}

int MediaSettingsSerialization::delayMilliseconds(
    bool enabled, const QString& secondsText)
{
    return qMax(0, signedDelayMilliseconds(enabled, secondsText));
}

int MediaSettingsSerialization::signedDelayMilliseconds(
    bool enabled, const QString& secondsText)
{
    if (!enabled) return 0;
    bool ok = false;
    const double seconds = secondsText.trimmed().toDouble(&ok);
    return ok && std::isfinite(seconds)
        ? qRound64(qBound(-86400.0, seconds, 86400.0) * 1000.0) : 0;
}

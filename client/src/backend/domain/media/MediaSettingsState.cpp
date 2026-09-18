#include "MediaSettingsState.h"
QJsonObject MediaSettingsSerialization::toProjectJson(const MediaSettingsState& s)
{
    return {{"schemaVersion",2},{"opacityOverrideEnabled",s.opacityOverrideEnabled},
            {"opacityText",s.opacityText},{"volumeOverrideEnabled",s.volumeOverrideEnabled},
            {"volumeText",s.volumeText}};
}
bool MediaSettingsSerialization::fromProjectJson(const QJsonObject& o,MediaSettingsState* s)
{
    if(!s || o.value("schemaVersion").toInt()!=2 || !o.value("opacityOverrideEnabled").isBool()
        || !o.value("opacityText").isString() || !o.value("volumeOverrideEnabled").isBool()
        || !o.value("volumeText").isString()) return false;
    s->opacityOverrideEnabled=o.value("opacityOverrideEnabled").toBool();s->opacityText=o.value("opacityText").toString();
    s->volumeOverrideEnabled=o.value("volumeOverrideEnabled").toBool();s->volumeText=o.value("volumeText").toString();return true;
}

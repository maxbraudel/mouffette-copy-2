#include "frontend/qml/MediaSettingsViewModel.h"

#include "backend/domain/media/CanvasMedia.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"

#include <QColor>

MediaSettingsViewModel::MediaSettingsViewModel(QObject* parent)
    : QObject(parent)
{}

void MediaSettingsViewModel::setController(QuickCanvasController* controller)
{
    if (m_controller == controller) return;
    if (m_controller) disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &QuickCanvasController::editingEnabledChanged,
                this, &MediaSettingsViewModel::refresh);
        connect(m_controller, &QuickCanvasController::selectedMediaChanged,
                this, &MediaSettingsViewModel::refresh);
    }
    refresh();
}

CanvasMedia* MediaSettingsViewModel::media() const
{
    return m_controller ? m_controller->selectedMediaItem() : nullptr;
}

bool MediaSettingsViewModel::available() const
{
    return m_controller && m_controller->editingEnabled()
        && media() != nullptr;
}
QString MediaSettingsViewModel::mediaId() const { return media() ? media()->mediaId() : QString(); }
QString MediaSettingsViewModel::mediaName() const { return media() ? media()->displayName() : QString(); }
bool MediaSettingsViewModel::video() const { return media() && media()->isVideo(); }
bool MediaSettingsViewModel::textMedia() const { return media() && media()->isText(); }

#define MEDIA_STATE_BOOL_GETTER(name, field) \
bool MediaSettingsViewModel::name() const { \
    return media() ? media()->settings().field : false; \
}
#define MEDIA_STATE_TEXT_GETTER(name, field, fallback) \
QString MediaSettingsViewModel::name() const { \
    if (!media()) return QStringLiteral(fallback); \
    const QString value = media()->settings().field; \
    return value.isEmpty() ? QStringLiteral(fallback) : value; \
}
#define MEDIA_STATE_BOOL_SETTER(Name, field) \
void MediaSettingsViewModel::set##Name(bool value) { \
    updateSettings([value](CanvasMedia* item) { \
        auto state = item->settings(); state.field = value; \
        item->setSettings(state); \
    }); \
}
#define MEDIA_STATE_TEXT_SETTER(Name, field) \
void MediaSettingsViewModel::set##Name(const QString& value) { \
    updateSettings([value](CanvasMedia* item) { \
        auto state = item->settings(); state.field = value.trimmed(); \
        item->setSettings(state); \
    }); \
}

MEDIA_STATE_BOOL_GETTER(displayAutomatically, displayAutomatically)
MEDIA_STATE_BOOL_GETTER(displayDelayEnabled, displayDelayEnabled)
MEDIA_STATE_TEXT_GETTER(displayDelayText, displayDelayText, "1")
MEDIA_STATE_BOOL_GETTER(playAutomatically, playAutomatically)
MEDIA_STATE_BOOL_GETTER(playDelayEnabled, playDelayEnabled)
MEDIA_STATE_TEXT_GETTER(playDelayText, playDelayText, "1")
MEDIA_STATE_BOOL_GETTER(pauseDelayEnabled, pauseDelayEnabled)
MEDIA_STATE_TEXT_GETTER(pauseDelayText, pauseDelayText, "1")
MEDIA_STATE_BOOL_GETTER(repeatEnabled, repeatEnabled)
MEDIA_STATE_TEXT_GETTER(repeatCountText, repeatCountText, "1")
MEDIA_STATE_BOOL_GETTER(fadeInEnabled, fadeInEnabled)
MEDIA_STATE_TEXT_GETTER(fadeInText, fadeInText, "1")
MEDIA_STATE_BOOL_GETTER(fadeOutEnabled, fadeOutEnabled)
MEDIA_STATE_TEXT_GETTER(fadeOutText, fadeOutText, "1")
MEDIA_STATE_BOOL_GETTER(opacityOverrideEnabled, opacityOverrideEnabled)
MEDIA_STATE_TEXT_GETTER(opacityText, opacityText, "100")
MEDIA_STATE_BOOL_GETTER(hideDelayEnabled, hideDelayEnabled)
MEDIA_STATE_TEXT_GETTER(hideDelayText, hideDelayText, "1")
MEDIA_STATE_BOOL_GETTER(unmuteAutomatically, unmuteAutomatically)
MEDIA_STATE_BOOL_GETTER(unmuteDelayEnabled, unmuteDelayEnabled)
MEDIA_STATE_TEXT_GETTER(unmuteDelayText, unmuteDelayText, "0")
MEDIA_STATE_BOOL_GETTER(muteDelayEnabled, muteDelayEnabled)
MEDIA_STATE_TEXT_GETTER(muteDelayText, muteDelayText, "1")
MEDIA_STATE_BOOL_GETTER(hideWhenVideoEnds, hideWhenVideoEnds)
MEDIA_STATE_BOOL_GETTER(muteWhenVideoEnds, muteWhenVideoEnds)
MEDIA_STATE_BOOL_GETTER(audioFadeInEnabled, audioFadeInEnabled)
MEDIA_STATE_TEXT_GETTER(audioFadeInText, audioFadeInText, "1")
MEDIA_STATE_BOOL_GETTER(audioFadeOutEnabled, audioFadeOutEnabled)
MEDIA_STATE_TEXT_GETTER(audioFadeOutText, audioFadeOutText, "1")

MEDIA_STATE_BOOL_SETTER(DisplayAutomatically, displayAutomatically)
MEDIA_STATE_BOOL_SETTER(DisplayDelayEnabled, displayDelayEnabled)
MEDIA_STATE_TEXT_SETTER(DisplayDelayText, displayDelayText)
MEDIA_STATE_BOOL_SETTER(PlayAutomatically, playAutomatically)
MEDIA_STATE_BOOL_SETTER(PlayDelayEnabled, playDelayEnabled)
MEDIA_STATE_TEXT_SETTER(PlayDelayText, playDelayText)
MEDIA_STATE_BOOL_SETTER(PauseDelayEnabled, pauseDelayEnabled)
MEDIA_STATE_TEXT_SETTER(PauseDelayText, pauseDelayText)
MEDIA_STATE_BOOL_SETTER(RepeatEnabled, repeatEnabled)
MEDIA_STATE_TEXT_SETTER(RepeatCountText, repeatCountText)
MEDIA_STATE_BOOL_SETTER(FadeInEnabled, fadeInEnabled)
MEDIA_STATE_TEXT_SETTER(FadeInText, fadeInText)
MEDIA_STATE_BOOL_SETTER(FadeOutEnabled, fadeOutEnabled)
MEDIA_STATE_TEXT_SETTER(FadeOutText, fadeOutText)
MEDIA_STATE_BOOL_SETTER(OpacityOverrideEnabled, opacityOverrideEnabled)
MEDIA_STATE_TEXT_SETTER(OpacityText, opacityText)
MEDIA_STATE_BOOL_SETTER(HideDelayEnabled, hideDelayEnabled)
MEDIA_STATE_TEXT_SETTER(HideDelayText, hideDelayText)
MEDIA_STATE_BOOL_SETTER(UnmuteAutomatically, unmuteAutomatically)
MEDIA_STATE_BOOL_SETTER(UnmuteDelayEnabled, unmuteDelayEnabled)
MEDIA_STATE_TEXT_SETTER(UnmuteDelayText, unmuteDelayText)
MEDIA_STATE_BOOL_SETTER(MuteDelayEnabled, muteDelayEnabled)
MEDIA_STATE_TEXT_SETTER(MuteDelayText, muteDelayText)
MEDIA_STATE_BOOL_SETTER(HideWhenVideoEnds, hideWhenVideoEnds)
MEDIA_STATE_BOOL_SETTER(MuteWhenVideoEnds, muteWhenVideoEnds)
MEDIA_STATE_BOOL_SETTER(AudioFadeInEnabled, audioFadeInEnabled)
MEDIA_STATE_TEXT_SETTER(AudioFadeInText, audioFadeInText)
MEDIA_STATE_BOOL_SETTER(AudioFadeOutEnabled, audioFadeOutEnabled)
MEDIA_STATE_TEXT_SETTER(AudioFadeOutText, audioFadeOutText)

#undef MEDIA_STATE_BOOL_GETTER
#undef MEDIA_STATE_TEXT_GETTER
#undef MEDIA_STATE_BOOL_SETTER
#undef MEDIA_STATE_TEXT_SETTER

bool MediaSettingsViewModel::audioEnabled() const
{
    return video() && !media()->muted();
}

void MediaSettingsViewModel::setAudioEnabled(bool enabled)
{
    updateSettings([enabled](CanvasMedia* item) {
        if (item->isVideo()) item->setMuted(!enabled);
    });
}

QString MediaSettingsViewModel::volumeText() const
{
    return QString::number(video() ? qRound(media()->volume() * 100.0) : 100);
}

void MediaSettingsViewModel::setVolumeText(const QString& value)
{
    bool ok = false;
    const int percent = value.trimmed().toInt(&ok);
    if (!ok || !video() || !m_controller) return;
    // Share the slider's normalization and persistence path. Changing volume
    // must never change the independent muted state.
    m_controller->handleOverlayVolumeChange(media()->mediaId(),
                                           qBound(0, percent, 100) / 100.0);
}

static CanvasMedia* asText(CanvasMedia* item)
{
    return item && item->isText() ? item : nullptr;
}

bool MediaSettingsViewModel::textColorOverrideEnabled() const { auto* t = asText(media()); return t && t->textColorOverrideEnabled(); }
QString MediaSettingsViewModel::textColor() const { auto* t = asText(media()); return t ? t->textColor().name(QColor::HexArgb) : QStringLiteral("#FFFFFFFF"); }
bool MediaSettingsViewModel::highlightEnabled() const { auto* t = asText(media()); return t && t->highlightEnabled(); }
QString MediaSettingsViewModel::highlightColor() const { auto* t = asText(media()); return t ? t->highlightColor().name(QColor::HexArgb) : QStringLiteral("#00000000"); }
bool MediaSettingsViewModel::textBorderWidthOverrideEnabled() const { auto* t = asText(media()); return t && t->outlineWidthOverrideEnabled(); }
QString MediaSettingsViewModel::textBorderWidthText() const { auto* t = asText(media()); return t ? QString::number(t->outlineWidthPercent(), 'g', 4) : QStringLiteral("0"); }
bool MediaSettingsViewModel::textBorderColorOverrideEnabled() const { auto* t = asText(media()); return t && t->outlineColorOverrideEnabled(); }
QString MediaSettingsViewModel::textBorderColor() const { auto* t = asText(media()); return t ? t->outlineColor().name(QColor::HexArgb) : QStringLiteral("#00000000"); }
bool MediaSettingsViewModel::fontWeightOverrideEnabled() const { auto* t = asText(media()); return t && t->fontWeightOverrideEnabled(); }
QString MediaSettingsViewModel::fontWeightText() const { auto* t = asText(media()); return QString::number(t ? t->fontWeight() : 400); }
bool MediaSettingsViewModel::italic() const { auto* t = asText(media()); return t && t->italic(); }
bool MediaSettingsViewModel::underline() const { auto* t = asText(media()); return t && t->underline(); }
bool MediaSettingsViewModel::uppercase() const { auto* t = asText(media()); return t && t->uppercase(); }

void MediaSettingsViewModel::setTextColorOverrideEnabled(bool v) { updateSettings([v](auto* i){ if (auto* t=asText(i)) t->setTextColorOverrideEnabled(v); }); }
void MediaSettingsViewModel::setTextColor(const QString& v) { updateSettings([v](auto* i){ QColor c(v); if (auto* t=asText(i); t && c.isValid()) t->setTextColor(c); }); }
void MediaSettingsViewModel::setHighlightEnabled(bool v) { updateSettings([v](auto* i){ if (auto* t=asText(i)) t->setHighlightEnabled(v); }); }
void MediaSettingsViewModel::setHighlightColor(const QString& v) { updateSettings([v](auto* i){ QColor c(v); if (auto* t=asText(i); t && c.isValid()) t->setHighlightColor(c); }); }
void MediaSettingsViewModel::setTextBorderWidthOverrideEnabled(bool v) { updateSettings([v](auto* i){ if (auto* t=asText(i)) t->setOutlineWidthOverrideEnabled(v); }); }
void MediaSettingsViewModel::setTextBorderWidthText(const QString& v) { updateSettings([v](auto* i){ bool ok=false; const qreal n=v.toDouble(&ok); if (auto* t=asText(i); t && ok) t->setOutlineWidthPercent(n); }); }
void MediaSettingsViewModel::setTextBorderColorOverrideEnabled(bool v) { updateSettings([v](auto* i){ if (auto* t=asText(i)) t->setOutlineColorOverrideEnabled(v); }); }
void MediaSettingsViewModel::setTextBorderColor(const QString& v) { updateSettings([v](auto* i){ QColor c(v); if (auto* t=asText(i); t && c.isValid()) t->setOutlineColor(c); }); }
void MediaSettingsViewModel::setFontWeightOverrideEnabled(bool v) { updateSettings([v](auto* i){ if (auto* t=asText(i)) t->setFontWeightOverrideEnabled(v); }); }
void MediaSettingsViewModel::setFontWeightText(const QString& v) { updateSettings([v](auto* i){ bool ok=false; int n=v.toInt(&ok); if (auto* t=asText(i); t && ok) t->setFontWeight(qBound(1,n,1000)); }); }
void MediaSettingsViewModel::setItalic(bool v) { updateSettings([v](auto* i){ if (auto* t=asText(i)) t->setItalic(v); }); }
void MediaSettingsViewModel::setUnderline(bool v) { updateSettings([v](auto* i){ if (auto* t=asText(i)) t->setUnderline(v); }); }
void MediaSettingsViewModel::setUppercase(bool v) { updateSettings([v](auto* i){ if (auto* t=asText(i)) t->setUppercase(v); }); }

void MediaSettingsViewModel::updateSettings(
    const std::function<void(CanvasMedia*)>& update)
{
    CanvasMedia* item = media();
    if (!item || !m_controller || !m_controller->projectEditingEnabled()
        || m_controller->editsLocked()) return;
    update(item);
    m_controller->refreshMediaProjection();
    emit changed();
}

void MediaSettingsViewModel::refresh()
{
    CanvasMedia* next = media();
    if (m_observedMedia != next) {
        if (m_observedMedia) disconnect(m_observedMedia, nullptr, this, nullptr);
        m_observedMedia = next;
        if (m_observedMedia) {
            connect(m_observedMedia, &CanvasMedia::changed,
                    this, &MediaSettingsViewModel::changed);
            connect(m_observedMedia, &CanvasMedia::audioStateChanged,
                    this, &MediaSettingsViewModel::changed);
        }
    }
    emit changed();
}

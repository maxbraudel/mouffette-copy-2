#pragma once

#include <QObject>
#include <QPointer>
#include <functional>

class CanvasMedia;
class QuickCanvasController;

class MediaSettingsViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString mediaName READ mediaName NOTIFY changed)
    Q_PROPERTY(bool video READ video NOTIFY changed)
    Q_PROPERTY(bool textMedia READ textMedia NOTIFY changed)

    Q_PROPERTY(bool displayAutomatically READ displayAutomatically WRITE setDisplayAutomatically NOTIFY changed)
    Q_PROPERTY(bool displayDelayEnabled READ displayDelayEnabled WRITE setDisplayDelayEnabled NOTIFY changed)
    Q_PROPERTY(QString displayDelayText READ displayDelayText WRITE setDisplayDelayText NOTIFY changed)
    Q_PROPERTY(bool playAutomatically READ playAutomatically WRITE setPlayAutomatically NOTIFY changed)
    Q_PROPERTY(bool playDelayEnabled READ playDelayEnabled WRITE setPlayDelayEnabled NOTIFY changed)
    Q_PROPERTY(QString playDelayText READ playDelayText WRITE setPlayDelayText NOTIFY changed)
    Q_PROPERTY(bool pauseDelayEnabled READ pauseDelayEnabled WRITE setPauseDelayEnabled NOTIFY changed)
    Q_PROPERTY(QString pauseDelayText READ pauseDelayText WRITE setPauseDelayText NOTIFY changed)
    Q_PROPERTY(bool repeatEnabled READ repeatEnabled WRITE setRepeatEnabled NOTIFY changed)
    Q_PROPERTY(QString repeatCountText READ repeatCountText WRITE setRepeatCountText NOTIFY changed)
    Q_PROPERTY(bool fadeInEnabled READ fadeInEnabled WRITE setFadeInEnabled NOTIFY changed)
    Q_PROPERTY(QString fadeInText READ fadeInText WRITE setFadeInText NOTIFY changed)
    Q_PROPERTY(bool fadeOutEnabled READ fadeOutEnabled WRITE setFadeOutEnabled NOTIFY changed)
    Q_PROPERTY(QString fadeOutText READ fadeOutText WRITE setFadeOutText NOTIFY changed)
    Q_PROPERTY(bool opacityOverrideEnabled READ opacityOverrideEnabled WRITE setOpacityOverrideEnabled NOTIFY changed)
    Q_PROPERTY(QString opacityText READ opacityText WRITE setOpacityText NOTIFY changed)
    Q_PROPERTY(bool hideDelayEnabled READ hideDelayEnabled WRITE setHideDelayEnabled NOTIFY changed)
    Q_PROPERTY(QString hideDelayText READ hideDelayText WRITE setHideDelayText NOTIFY changed)

    Q_PROPERTY(bool unmuteAutomatically READ unmuteAutomatically WRITE setUnmuteAutomatically NOTIFY changed)
    Q_PROPERTY(bool unmuteDelayEnabled READ unmuteDelayEnabled WRITE setUnmuteDelayEnabled NOTIFY changed)
    Q_PROPERTY(QString unmuteDelayText READ unmuteDelayText WRITE setUnmuteDelayText NOTIFY changed)
    Q_PROPERTY(bool muteDelayEnabled READ muteDelayEnabled WRITE setMuteDelayEnabled NOTIFY changed)
    Q_PROPERTY(QString muteDelayText READ muteDelayText WRITE setMuteDelayText NOTIFY changed)
    Q_PROPERTY(bool hideWhenVideoEnds READ hideWhenVideoEnds WRITE setHideWhenVideoEnds NOTIFY changed)
    Q_PROPERTY(bool muteWhenVideoEnds READ muteWhenVideoEnds WRITE setMuteWhenVideoEnds NOTIFY changed)
    Q_PROPERTY(bool audioFadeInEnabled READ audioFadeInEnabled WRITE setAudioFadeInEnabled NOTIFY changed)
    Q_PROPERTY(QString audioFadeInText READ audioFadeInText WRITE setAudioFadeInText NOTIFY changed)
    Q_PROPERTY(bool audioFadeOutEnabled READ audioFadeOutEnabled WRITE setAudioFadeOutEnabled NOTIFY changed)
    Q_PROPERTY(QString audioFadeOutText READ audioFadeOutText WRITE setAudioFadeOutText NOTIFY changed)
    Q_PROPERTY(bool volumeOverrideEnabled READ volumeOverrideEnabled WRITE setVolumeOverrideEnabled NOTIFY changed)
    Q_PROPERTY(QString volumeText READ volumeText WRITE setVolumeText NOTIFY changed)

    Q_PROPERTY(bool textColorOverrideEnabled READ textColorOverrideEnabled WRITE setTextColorOverrideEnabled NOTIFY changed)
    Q_PROPERTY(QString textColor READ textColor WRITE setTextColor NOTIFY changed)
    Q_PROPERTY(bool highlightEnabled READ highlightEnabled WRITE setHighlightEnabled NOTIFY changed)
    Q_PROPERTY(QString highlightColor READ highlightColor WRITE setHighlightColor NOTIFY changed)
    Q_PROPERTY(bool textBorderWidthOverrideEnabled READ textBorderWidthOverrideEnabled WRITE setTextBorderWidthOverrideEnabled NOTIFY changed)
    Q_PROPERTY(QString textBorderWidthText READ textBorderWidthText WRITE setTextBorderWidthText NOTIFY changed)
    Q_PROPERTY(bool textBorderColorOverrideEnabled READ textBorderColorOverrideEnabled WRITE setTextBorderColorOverrideEnabled NOTIFY changed)
    Q_PROPERTY(QString textBorderColor READ textBorderColor WRITE setTextBorderColor NOTIFY changed)
    Q_PROPERTY(bool fontWeightOverrideEnabled READ fontWeightOverrideEnabled WRITE setFontWeightOverrideEnabled NOTIFY changed)
    Q_PROPERTY(QString fontWeightText READ fontWeightText WRITE setFontWeightText NOTIFY changed)
    Q_PROPERTY(bool italic READ italic WRITE setItalic NOTIFY changed)
    Q_PROPERTY(bool underline READ underline WRITE setUnderline NOTIFY changed)
    Q_PROPERTY(bool uppercase READ uppercase WRITE setUppercase NOTIFY changed)

public:
    explicit MediaSettingsViewModel(QObject* parent = nullptr);
    void setController(QuickCanvasController* controller);

    bool available() const;
    QString mediaName() const;
    bool video() const;
    bool textMedia() const;

    bool displayAutomatically() const; void setDisplayAutomatically(bool value);
    bool displayDelayEnabled() const; void setDisplayDelayEnabled(bool value);
    QString displayDelayText() const; void setDisplayDelayText(const QString& value);
    bool playAutomatically() const; void setPlayAutomatically(bool value);
    bool playDelayEnabled() const; void setPlayDelayEnabled(bool value);
    QString playDelayText() const; void setPlayDelayText(const QString& value);
    bool pauseDelayEnabled() const; void setPauseDelayEnabled(bool value);
    QString pauseDelayText() const; void setPauseDelayText(const QString& value);
    bool repeatEnabled() const; void setRepeatEnabled(bool value);
    QString repeatCountText() const; void setRepeatCountText(const QString& value);
    bool fadeInEnabled() const; void setFadeInEnabled(bool value);
    QString fadeInText() const; void setFadeInText(const QString& value);
    bool fadeOutEnabled() const; void setFadeOutEnabled(bool value);
    QString fadeOutText() const; void setFadeOutText(const QString& value);
    bool opacityOverrideEnabled() const; void setOpacityOverrideEnabled(bool value);
    QString opacityText() const; void setOpacityText(const QString& value);
    bool hideDelayEnabled() const; void setHideDelayEnabled(bool value);
    QString hideDelayText() const; void setHideDelayText(const QString& value);

    bool unmuteAutomatically() const; void setUnmuteAutomatically(bool value);
    bool unmuteDelayEnabled() const; void setUnmuteDelayEnabled(bool value);
    QString unmuteDelayText() const; void setUnmuteDelayText(const QString& value);
    bool muteDelayEnabled() const; void setMuteDelayEnabled(bool value);
    QString muteDelayText() const; void setMuteDelayText(const QString& value);
    bool hideWhenVideoEnds() const; void setHideWhenVideoEnds(bool value);
    bool muteWhenVideoEnds() const; void setMuteWhenVideoEnds(bool value);
    bool audioFadeInEnabled() const; void setAudioFadeInEnabled(bool value);
    QString audioFadeInText() const; void setAudioFadeInText(const QString& value);
    bool audioFadeOutEnabled() const; void setAudioFadeOutEnabled(bool value);
    QString audioFadeOutText() const; void setAudioFadeOutText(const QString& value);
    bool volumeOverrideEnabled() const; void setVolumeOverrideEnabled(bool value);
    QString volumeText() const; void setVolumeText(const QString& value);

    bool textColorOverrideEnabled() const; void setTextColorOverrideEnabled(bool value);
    QString textColor() const; void setTextColor(const QString& value);
    bool highlightEnabled() const; void setHighlightEnabled(bool value);
    QString highlightColor() const; void setHighlightColor(const QString& value);
    bool textBorderWidthOverrideEnabled() const; void setTextBorderWidthOverrideEnabled(bool value);
    QString textBorderWidthText() const; void setTextBorderWidthText(const QString& value);
    bool textBorderColorOverrideEnabled() const; void setTextBorderColorOverrideEnabled(bool value);
    QString textBorderColor() const; void setTextBorderColor(const QString& value);
    bool fontWeightOverrideEnabled() const; void setFontWeightOverrideEnabled(bool value);
    QString fontWeightText() const; void setFontWeightText(const QString& value);
    bool italic() const; void setItalic(bool value);
    bool underline() const; void setUnderline(bool value);
    bool uppercase() const; void setUppercase(bool value);

signals:
    void changed();

private:
    CanvasMedia* media() const;
    void updateSettings(const std::function<void(CanvasMedia*)>& update);
    void refresh();

    QPointer<QuickCanvasController> m_controller;
    QPointer<CanvasMedia> m_observedMedia;
};

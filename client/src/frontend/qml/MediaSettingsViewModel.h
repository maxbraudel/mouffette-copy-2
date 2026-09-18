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
    Q_PROPERTY(QString mediaId READ mediaId NOTIFY changed)
    Q_PROPERTY(QString mediaName READ mediaName NOTIFY changed)
    Q_PROPERTY(bool video READ video NOTIFY changed)
    Q_PROPERTY(bool textMedia READ textMedia NOTIFY changed)

    Q_PROPERTY(bool opacityOverrideEnabled READ opacityOverrideEnabled WRITE setOpacityOverrideEnabled NOTIFY changed)
    Q_PROPERTY(QString opacityText READ opacityText WRITE setOpacityText NOTIFY changed)

    Q_PROPERTY(bool audioEnabled READ audioEnabled WRITE setAudioEnabled NOTIFY changed)
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
    QString mediaId() const;
    QString mediaName() const;
    bool video() const;
    bool textMedia() const;

    bool opacityOverrideEnabled() const; void setOpacityOverrideEnabled(bool value);
    QString opacityText() const; void setOpacityText(const QString& value);

    bool audioEnabled() const; void setAudioEnabled(bool value);
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

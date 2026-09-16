#pragma once

#include "backend/domain/media/MediaSettingsState.h"

#include <QColor>
#include <QMediaPlayer>
#include <QObject>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVariantMap>

class QAudioOutput;
class QVideoSink;

// Renderer-independent media node. QML receives immutable projections of this
// object; all mutations go through CanvasDocument or a typed view model.
class CanvasMedia final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal animatedDisplayOpacity READ animatedDisplayOpacity
               WRITE setAnimatedDisplayOpacity NOTIFY changed)

public:
    enum class Type { Image, Video, Text };
    Q_ENUM(Type)
    enum class UploadState { NotUploaded, Uploading, Uploaded };
    Q_ENUM(UploadState)

    explicit CanvasMedia(Type type, const QSize& baseSize,
                         QObject* parent = nullptr);
    ~CanvasMedia() override;

    Type type() const { return m_type; }
    bool isVideo() const { return m_type == Type::Video; }
    bool isText() const { return m_type == Type::Text; }
    QString typeName() const;

    QString mediaId() const { return m_mediaId; }
    void restoreMediaId(const QString& id);
    QString fileId() const { return m_fileId; }
    void setFileId(const QString& id);
    QString sourcePath() const { return m_sourcePath; }
    void setSourcePath(const QString& path);
    QString displayName() const;

    QSize baseSize() const { return m_baseSize; }
    void setBaseSize(const QSize& size);
    QPointF position() const { return m_position; }
    void setPosition(const QPointF& position);
    qreal scale() const { return m_scale; }
    void setScale(qreal scale);
    qreal z() const { return m_z; }
    void setZ(qreal z);
    QRectF sceneRect() const;

    bool selected() const { return m_selected; }
    void setSelected(bool selected);
    bool contentVisible() const { return m_contentVisible; }
    void setContentVisible(bool visible);
    qreal contentOpacity() const { return m_contentOpacity; }
    void setContentOpacity(qreal opacity);
    qreal animatedDisplayOpacity() const { return m_animatedDisplayOpacity; }
    void setAnimatedDisplayOpacity(qreal opacity);

    UploadState uploadState() const { return m_uploadState; }
    int uploadProgress() const { return m_uploadProgress; }
    void setUploadNotUploaded();
    void setUploadUploading(int progress);
    void setUploadUploaded();

    const MediaSettingsState& settings() const { return m_settings; }
    void setSettings(const MediaSettingsState& settings);

    QString text() const { return m_text; }
    void setText(const QString& text);
    QString fontFamily() const { return m_fontFamily; }
    void setFontFamily(const QString& family);
    int fontPixelSize() const { return m_fontPixelSize; }
    void setFontPixelSize(int size);
    int fontWeight() const { return m_fontWeight; }
    int renderedFontWeight() const;
    void setFontWeight(int weight);
    bool fontWeightOverrideEnabled() const { return m_fontWeightOverride; }
    void setFontWeightOverrideEnabled(bool enabled);
    bool italic() const { return m_italic; }
    void setItalic(bool italic);
    bool underline() const { return m_underline; }
    void setUnderline(bool underline);
    bool uppercase() const { return m_uppercase; }
    void setUppercase(bool uppercase);
    QColor textColor() const { return m_textColor; }
    QColor renderedTextColor() const;
    void setTextColor(const QColor& color);
    bool textColorOverrideEnabled() const { return m_textColorOverride; }
    void setTextColorOverrideEnabled(bool enabled);
    bool highlightEnabled() const { return m_highlightEnabled; }
    void setHighlightEnabled(bool enabled);
    QColor highlightColor() const { return m_highlightColor; }
    void setHighlightColor(const QColor& color);
    qreal outlineWidthPercent() const { return m_outlineWidthPercent; }
    qreal renderedOutlineWidthPercent() const;
    void setOutlineWidthPercent(qreal width);
    bool outlineWidthOverrideEnabled() const { return m_outlineWidthOverride; }
    void setOutlineWidthOverrideEnabled(bool enabled);
    QColor outlineColor() const { return m_outlineColor; }
    QColor renderedOutlineColor() const;
    void setOutlineColor(const QColor& color);
    bool outlineColorOverrideEnabled() const { return m_outlineColorOverride; }
    void setOutlineColorOverrideEnabled(bool enabled);
    bool fitToTextEnabled() const { return m_fitToText; }
    void setFitToTextEnabled(bool enabled);
    void fitTextToContent();
    QString horizontalAlignment() const { return m_horizontalAlignment; }
    void setHorizontalAlignment(const QString& alignment);
    QString verticalAlignment() const { return m_verticalAlignment; }
    void setVerticalAlignment(const QString& alignment);

    void initializeVideoRuntime();
    QMediaPlayer* player() const { return m_player; }
    QVideoSink* videoSink() const { return m_videoSink; }
    QAudioOutput* audioOutput() const { return m_audioOutput; }
    bool isPlaying() const;
    bool muted() const;
    void setMuted(bool muted);
    qreal volume() const;
    void setVolume(qreal volume);
    bool repeatEnabled() const { return m_repeatEnabled; }
    void setRepeatEnabled(bool enabled);
    qint64 startMarkerMs() const { return m_startMarkerMs; }
    qint64 endMarkerMs() const { return m_endMarkerMs; }
    bool setPlaybackRange(qint64 startMs, qint64 endMs);
    bool canPlaceStart() const;
    bool canPlaceEnd() const;
    qint64 playbackStartMs() const;
    qint64 playbackEndMs() const;
    void beginScenePlayback();
    void endScenePlayback();
    bool repeatAvailable() const;
    void togglePlayPause();
    void stopToBeginning();
    void seekToRatio(qreal ratio);
    void setPositionMs(qint64 positionMs);
    qint64 positionMs() const;
    bool hasRenderedFrame() const { return m_hasRenderedFrame; }
    bool firstFramePrimed() const { return m_firstFramePrimed; }

    QVariantMap toModelMap(qreal sceneUnitScale = 1.0) const;

signals:
    void changed();
    void uploadStateChanged();
    void runtimeStateChanged();
    void audioStateChanged();

private:
    void notifyChanged();
    bool updateFitToTextGeometry();
    void notifyTextMetricsChanged();
    void updateVideoLoops();
    void enforcePlaybackEnd(qint64 position, bool atEnd = false);

    Type m_type;
    QString m_mediaId;
    QString m_fileId;
    QString m_sourcePath;
    qint64 m_sourceSizeBytes = -1;
    QSize m_baseSize;
    QPointF m_position;
    qreal m_scale = 1.0;
    qreal m_z = 1.0;
    bool m_selected = false;
    bool m_contentVisible = true;
    qreal m_contentOpacity = 1.0;
    qreal m_animatedDisplayOpacity = 1.0;
    UploadState m_uploadState = UploadState::NotUploaded;
    int m_uploadProgress = 0;
    MediaSettingsState m_settings;

    QString m_text = QStringLiteral("Text");
    QString m_fontFamily = QStringLiteral("Impact");
    int m_fontPixelSize = 64;
    int m_fontWeight = 400;
    bool m_fontWeightOverride = false;
    bool m_italic = false;
    bool m_underline = false;
    bool m_uppercase = false;
    QColor m_textColor = Qt::white;
    bool m_textColorOverride = false;
    bool m_highlightEnabled = false;
    QColor m_highlightColor = QColor(255, 255, 0, 128);
    qreal m_outlineWidthPercent = 0.0;
    bool m_outlineWidthOverride = false;
    QColor m_outlineColor = Qt::black;
    bool m_outlineColorOverride = false;
    bool m_fitToText = true;
    QString m_horizontalAlignment = QStringLiteral("center");
    QString m_verticalAlignment = QStringLiteral("center");

    QMediaPlayer* m_player = nullptr;
    QVideoSink* m_videoSink = nullptr;
    QAudioOutput* m_audioOutput = nullptr;
    qint64 m_pendingPositionMs = -1;
    bool m_repeatEnabled = false;
    qint64 m_startMarkerMs = -1;
    qint64 m_endMarkerMs = -1;
    bool m_scenePlayback = false;
    int m_repeatRemaining = 0;
    bool m_handlingPlaybackEnd = false;
    bool m_hasRenderedFrame = false;
    bool m_firstFramePrimed = false;
};

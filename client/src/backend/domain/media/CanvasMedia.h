#pragma once

#include "backend/domain/media/MediaSettingsState.h"
#include "backend/domain/scene/SceneTimeline.h"
#include <optional>
#include "backend/media/ResidentVideoPlayer.h"

#include <QColor>
#include <QMediaPlayer>
#include <QObject>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVariantMap>

class QAudioOutput;
class QVideoSink;
class RemoteVideoFrameSource;

// Renderer-independent media node. QML receives immutable projections of this
// object; all mutations go through CanvasDocument or a typed view model.
class CanvasMedia final : public QObject
{
    Q_OBJECT

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
    QString residencyOwnerId() const { return m_residencyOwnerId; }
    void restoreMediaId(const QString& id);
    QString fileId() const { return m_fileId; }
    void setFileId(const QString& id);
    QString sourcePath() const { return m_sourcePath; }
    void setSourcePath(const QString& path, const QString& expectedSha256 = {});
    QString displayName() const;
    bool residencyReady() const;
    QString residencyState() const;
    double residencyProgress() const;
    QString residencyError() const;
    // Drop this occurrence's lease and decoded resources without retiring its
    // authoring identity. Shared assets remain available to other owners.
    void setResidencySuspended(bool suspended);
    bool residencySuspended() const { return m_residencySuspended; }
    void retireResidency();

    QSize baseSize() const { return m_baseSize.toSize(); }
    void setBaseSize(const QSize& size);
    QPointF position() const { return m_position; }
    void setPosition(const QPointF& position);
    qreal scale() const { return m_scale; }
    void setScale(qreal scale);
    void setPositionAndScale(const QPointF& position, qreal scale);
    qreal z() const { return m_z; }
    void setZ(qreal z);
    QRectF sceneRect() const;

    bool selected() const { return m_selected; }
    void setSelected(bool selected);
    // Presentation-only presence; never changes intrinsic visibility or saved keys.
    bool clipActive() const { return m_clipActive; }
    void setClipActive(bool active);
    bool contentVisible() const { return m_contentVisible; }
    void setContentVisible(bool visible);
    qreal contentOpacity() const { return m_contentOpacity; }
    void setContentOpacity(qreal opacity);

    UploadState uploadState() const { return m_uploadState; }
    int uploadProgress() const { return m_uploadProgress; }
    void setUploadNotUploaded();
    void setUploadUploading(int progress);
    void setUploadUploaded();

    SceneTimeline::ElementState authorElementState() const;
    SceneTimeline::ElementState displayedElementState() const;
    void setElementState(const SceneTimeline::ElementState& state);
    void setEvaluatedElementState(const SceneTimeline::ElementState& state);
    void clearEvaluatedElementState();
    bool hasEvaluatedElementState() const { return m_authorElementState.has_value(); }
    bool hasElementDraft() const { return m_elementDraft; }
    void beginElementEdit();
    const SceneTimeline::MediaTrack& timelineTrack() const { return m_timelineTrack; }
    void setTimelineTrack(const SceneTimeline::MediaTrack& track);
    void ensureDefaultClip(const SceneTimeline::SceneSettings& settings);

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
    ResidentVideoPlayer* player() const { return m_player; }
    QVideoSink* videoSink() const { return m_videoSink; }
    QAudioOutput* audioOutput() const { return m_audioOutput; }
    bool isPlaying() const;
    bool muted() const;
    void setMuted(bool muted, bool updateAudioOutput = true);
    qreal volume() const;
    void setVolume(qreal volume);
    void setPositionMs(qint64 positionMs);
    qint64 positionMs() const;
    qint64 sourceDurationMs() const { return m_sourceDurationMs; }
    void restoreSourceDurationMs(qint64 durationMs);
    bool hasRenderedFrame() const { return m_hasRenderedFrame; }
    bool firstFramePrimed() const { return m_firstFramePrimed; }

    QVariantMap toModelMap(qreal sceneUnitScale = 1.0) const;

signals:
    void changed();
    void presentationChanged();
    void draftChanged();
    void uploadStateChanged();
    void runtimeStateChanged();
    void audioStateChanged();
    void residencyChanged();
    void identityReady(const QString& fileId);
    void sourceInvalidated(const QString& reason);

private:
    void notifyChanged();
    SceneTimeline::ElementState captureElementFields() const;
    void applyElementFields(const SceneTimeline::ElementState& state);
    bool updateFitToTextGeometry();
    void notifyTextMetricsChanged();
    void refreshResidency();
    void requestResidency();
    void releaseResidencyResources();
    void initializeVideoOutputs();

    Type m_type;
    QString m_mediaId;
    QString m_fileId;
    QString m_sourcePath;
    QString m_expectedSha256;
    bool m_identityPublished = false;
    bool m_sourceInvalidationReported = false;
    bool m_residencyRetired = false;
    bool m_residencySuspended = false;
    bool m_residencyAcquired = false;
    quint64 m_residencyGeneration = 0;
    QString m_residencyOwnerId;
    qint64 m_sourceSizeBytes = -1;
    QSizeF m_baseSize;
    QPointF m_position;
    qreal m_scale = 1.0;
    qreal m_z = 1.0;
    bool m_selected = false;
    bool m_clipActive = false;
    bool m_contentVisible = true;
    qreal m_contentOpacity = 1.0;
    UploadState m_uploadState = UploadState::NotUploaded;
    int m_uploadProgress = 0;
    MediaSettingsState m_settings;
    SceneTimeline::MediaTrack m_timelineTrack;
    std::optional<SceneTimeline::ElementState> m_authorElementState;
    std::optional<SceneTimeline::ElementState> m_evaluatedElementState;
    bool m_elementDraft = false;

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

    ResidentVideoPlayer* m_player = nullptr;
    RemoteVideoFrameSource* m_residentFrameSource = nullptr;
    QVideoSink* m_videoSink = nullptr;
    QAudioOutput* m_audioOutput = nullptr;
    bool m_audioInitializationPending = false;
    bool m_muted = false;
    qreal m_volume = 1.0;
    qint64 m_pendingPositionMs = -1;
    qint64 m_sourceDurationMs = 0;
    bool m_hasRenderedFrame = false;
    bool m_firstFramePrimed = false;
};

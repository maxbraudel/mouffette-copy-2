#pragma once

#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QSizeF>
#include <QString>

// Authoring data and the deterministic, clock-independent scene evaluator.
namespace SceneTimeline {
inline constexpr int RenderSchemaVersion = 3;
inline constexpr qint64 MaximumSupportedDurationMs = 604800000;

struct ElementState {
    QString type = QStringLiteral("image");
    QPointF position;
    QSizeF size = QSizeF(1, 1); // final scene dimensions, interpolated linearly
    QSizeF baseSize = QSizeF(1, 1);
    qreal scale = 1.0;
    qreal z = 1.0;
    bool visible = true;
    qreal opacity = 1.0;
    bool muted = false;
    qreal volume = 1.0;
    QString text = QStringLiteral("Text");
    QString fontFamily = QStringLiteral("Impact");
    qreal fontPixelSize = 64.0;
    qreal fontWeight = 400.0;
    bool italic = false;
    bool underline = false;
    bool uppercase = false;
    QColor textColor = Qt::white;
    bool highlightEnabled = false;
    QColor highlightColor = QColor(255, 255, 0, 128);
    qreal outlineWidthPercent = 0.0;
    QColor outlineColor = Qt::black;
    bool fitToText = true;
    QString horizontalAlignment = QStringLiteral("center");
    QString verticalAlignment = QStringLiteral("center");

    // Effective values above remain independent from these author controls.
    // Interpolation never reapplies a discrete override to a continuous value.
    bool opacityOverrideEnabled = false;
    qreal rawOpacity = 1.0;
    bool fontWeightOverrideEnabled = false;
    qreal rawFontWeight = 400.0;
    bool textColorOverrideEnabled = false;
    QColor rawTextColor = Qt::white;
    bool outlineWidthOverrideEnabled = false;
    qreal rawOutlineWidthPercent = 0.0;
    bool outlineColorOverrideEnabled = false;
    QColor rawOutlineColor = Qt::black;

    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject&, ElementState*, QString* error = nullptr);
    static bool fromMediaJson(const QJsonObject&, ElementState*, QString* error = nullptr);
};

struct Keyframe {
    QString id;
    qint64 timeMs = 0;
    ElementState state;
};
struct VideoClip {
    QString id;
    qint64 startMs = 0;
    qint64 sourceInMs = 0;
    qint64 sourceOutMs = 0; // exclusive
    qint64 durationMs() const { return sourceOutMs - sourceInMs; }
    qint64 endMs() const { return startMs + durationMs(); }
};
struct MediaTrack {
    QList<Keyframe> keyframes;
    QList<VideoClip> clips;
    bool clipsInitialized = false;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject&, MediaTrack*, qint64 maxDurationMs,
                         QString* error = nullptr);
};
struct SceneSettings {
    qint64 maxDurationMs = 180000;
    qint64 stopTimeMs = -1;
    qint64 effectiveStopMs() const { return stopTimeMs < 0 ? maxDurationMs : stopTimeMs; }
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject&, SceneSettings*, QString* error = nullptr);
};
struct VideoSample {
    qint64 sourceTimeMs = 0;
    bool playing = false;
    QString clipId;
};

ElementState evaluate(const ElementState& base, const MediaTrack&, qint64 timeMs);
ElementState materialize(const ElementState& evaluated);
VideoSample evaluateVideo(const MediaTrack&, qint64 timeMs, qint64 sourceDurationMs);
QJsonObject evaluateMedia(const QJsonObject& media, qint64 timeMs);
QString newId();
bool upsertKeyframe(MediaTrack&, Keyframe, qint64 maxDurationMs);
bool removeKeyframe(MediaTrack&, const QString& id);
bool moveKeyframe(MediaTrack&, const QString& id, qint64 timeMs, qint64 maxDurationMs);
// Insertion/movement/extension overwrite only their occupied interval, keeping
// source-correct fragments on either side. Operations are atomic.
bool insertClip(MediaTrack&, VideoClip, qint64 maxDurationMs);
bool removeClip(MediaTrack&, const QString& id);
bool moveClip(MediaTrack&, const QString& id, qint64 startMs, qint64 maxDurationMs);
bool splitClip(MediaTrack&, const QString& id, qint64 timeMs);
bool trimClip(MediaTrack&, const QString& id, qint64 startMs, qint64 endMs,
              qint64 maxDurationMs, qint64 sourceDurationMs);
}

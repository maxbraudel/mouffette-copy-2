#pragma once

#include <QObject>
#include "backend/domain/scene/SceneTimeline.h"
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>

class CanvasDocument;
class CanvasMedia;
class QuickCanvasHost;
namespace SceneTimeline { struct MediaTrack; }

// Authoring commands and immutable QML projections. The host owns transport;
// the document owns saved tracks, evaluated states and selection.
class TimelineController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal positionMs READ positionMs NOTIFY transportChanged)
    Q_PROPERTY(qreal maxDurationMs READ maxDurationMs NOTIFY changed)
    Q_PROPERTY(qreal stopTimeMs READ stopTimeMs NOTIFY changed)
    Q_PROPERTY(qreal effectiveEndMs READ effectiveEndMs NOTIFY changed)
    Q_PROPERTY(qint64 positionSlot READ positionSlot NOTIFY transportChanged)
    Q_PROPERTY(int slotsPerSecond READ slotsPerSecond NOTIFY changed)
    Q_PROPERTY(bool playing READ playing NOTIFY transportChanged)
    Q_PROPERTY(bool remoteActive READ remoteActive NOTIFY transportChanged)
    Q_PROPERTY(bool editable READ editable NOTIFY transportChanged)
    Q_PROPERTY(bool hasDraft READ hasDraft NOTIFY transportChanged)
    Q_PROPERTY(QString primaryMediaId READ primaryMediaId NOTIFY changed)
    Q_PROPERTY(QString primaryMediaName READ primaryMediaName NOTIFY changed)
    Q_PROPERTY(bool primaryIsVideo READ primaryIsVideo NOTIFY changed)
    Q_PROPERTY(QVariantList keyframes READ keyframes NOTIFY tracksChanged)
    Q_PROPERTY(QVariantList otherKeyframes READ otherKeyframes NOTIFY tracksChanged)
    Q_PROPERTY(QVariantList clips READ clips NOTIFY tracksChanged)
    Q_PROPERTY(QVariantList otherClips READ otherClips NOTIFY tracksChanged)
    Q_PROPERTY(QString selectedKeyframeId READ selectedKeyframeId NOTIFY changed)
    Q_PROPERTY(QString selectedClipId READ selectedClipId NOTIFY changed)
    Q_PROPERTY(bool canCapture READ canCapture NOTIFY transportChanged)
    Q_PROPERTY(bool canInsertClip READ canInsertClip NOTIFY transportChanged)
    Q_PROPERTY(bool canSplit READ canSplit NOTIFY transportChanged)
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY transportChanged)
    Q_PROPERTY(bool hasKeyframeAtPosition READ hasKeyframeAtPosition NOTIFY transportChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    Q_PROPERTY(int timelineHeightPx READ timelineHeightPx CONSTANT)
    Q_PROPERTY(int rulerHeightPx READ rulerHeightPx CONSTANT)
    Q_PROPERTY(int clipTrackHeightPx READ clipTrackHeightPx CONSTANT)
    Q_PROPERTY(int keyframeSizePx READ keyframeSizePx CONSTANT)
    Q_PROPERTY(qreal otherKeyframeOpacity READ otherKeyframeOpacity CONSTANT)
    Q_PROPERTY(int snapDistancePx READ snapDistancePx CONSTANT)
    Q_PROPERTY(int initialViewDurationMs READ initialViewDurationMs CONSTANT)

public:
    explicit TimelineController(QObject* parent = nullptr);
    void setHost(QuickCanvasHost* host);
    qreal positionMs() const;
    qreal maxDurationMs() const;
    qreal stopTimeMs() const;
    qreal effectiveEndMs() const;
    qint64 positionSlot() const;
    int slotsPerSecond() const;
    Q_INVOKABLE qreal gridTime(qreal timeMs) const;
    Q_INVOKABLE void stepSlots(int delta);
    bool playing() const;
    bool remoteActive() const;
    bool editable() const;
    bool hasDraft() const;
    QString primaryMediaId() const;
    QString primaryMediaName() const;
    bool primaryIsVideo() const;
    QVariantList keyframes() const;
    QVariantList otherKeyframes() const;
    QVariantList clips() const;
    QVariantList otherClips() const;
    QString selectedKeyframeId() const { return m_keyframeId; }
    QString selectedClipId() const { return m_clipId; }
    bool canCapture() const;
    bool canSplit() const;
    bool canInsertClip() const;
    bool canPaste() const;
    bool hasKeyframeAtPosition() const;
    QString errorText() const { return m_error; }
    int timelineHeightPx() const;
    int rulerHeightPx() const;
    int clipTrackHeightPx() const;
    int keyframeSizePx() const;
    qreal otherKeyframeOpacity() const;
    int snapDistancePx() const;
    int initialViewDurationMs() const;

    Q_INVOKABLE void seek(qreal timeMs);
    Q_INVOKABLE void togglePlayback();
    Q_INVOKABLE void goToStart();
    Q_INVOKABLE void goToEnd();
    Q_INVOKABLE void placeKeyframe();
    Q_INVOKABLE void selectKeyframe(const QString& id);
    Q_INVOKABLE void moveKeyframe(const QString& id, qreal timeMs);
    Q_INVOKABLE void selectClip(const QString& id);
    Q_INVOKABLE void moveClip(const QString& id, qreal startMs);
    Q_INVOKABLE void trimClip(const QString& id, qreal startMs, qreal endMs);
    Q_INVOKABLE void splitClip();
    Q_INVOKABLE void insertClip();
    Q_INVOKABLE void deleteSelected();
    Q_INVOKABLE void copySelected();
    Q_INVOKABLE void paste();
    Q_INVOKABLE void placeStop();
    Q_INVOKABLE void setStopTime(qreal timeMs);
    Q_INVOKABLE void removeStop();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE QVariantMap snapTime(qreal timeMs, qreal pixelsPerMs,
                                    const QString& excludeId,
                                    qreal clipDurationMs = 0) const;

signals:
    void changed();
    void transportChanged();
    void tracksChanged();

private:
    SceneTimeline::SceneSettings grid() const;
    CanvasMedia* primary() const;
    void refresh();
    void reevaluate();
    bool commitTrack(CanvasMedia* media, const SceneTimeline::MediaTrack& track);
    void error(const QString& text);
    QPointer<QuickCanvasHost> m_host;
    QPointer<CanvasDocument> m_document;
    QString m_primaryId;
    QString m_keyframeId;
    QString m_clipId;
    QString m_error;
    QVariantList m_publishedKeys;
    QVariantList m_publishedOtherKeys;
    QVariantList m_publishedClips;
    QVariantList m_publishedOtherClips;
};

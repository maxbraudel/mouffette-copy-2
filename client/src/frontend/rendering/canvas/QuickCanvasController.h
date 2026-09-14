#pragma once

#include "backend/domain/canvas/CanvasDocument.h"

#include <QImage>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class CanvasMedia;
class MediaListModel;
class QQuickWindow;
class RemoteVideoFrameSource;
class QTimer;

// Thin Qt Quick projection/interaction adapter. The CanvasDocument remains the
// sole source of truth; this class owns no scene graph items and never reaches
// into QML controls by object name.
class QuickCanvasController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* mediaModel READ mediaModel CONSTANT)
    Q_PROPERTY(QVariantList mediaSnapshot READ mediaSnapshot NOTIFY mediaSnapshotChanged)
    Q_PROPERTY(QVariantList selectionChromeModel READ selectionChromeModel NOTIFY selectionChromeModelChanged)
    Q_PROPERTY(QVariantList screensModel READ screensModel NOTIFY presentationChanged)
    Q_PROPERTY(QVariantList uiZonesModel READ uiZonesModel NOTIFY presentationChanged)
    Q_PROPERTY(QVariantList snapGuidesModel READ snapGuidesModel NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap videoStateModel READ videoStateModel NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap dropPreviewModel READ dropPreviewModel NOTIFY presentationChanged)
    Q_PROPERTY(QObject* dropPreviewFrameSource READ dropPreviewFrameSource CONSTANT)
    Q_PROPERTY(bool remoteActive READ remoteActive NOTIFY presentationChanged)
    Q_PROPERTY(bool textToolActive READ textToolActive NOTIFY presentationChanged)
    Q_PROPERTY(qreal viewScale READ viewScale NOTIFY presentationChanged)
    Q_PROPERTY(qreal panX READ panX NOTIFY presentationChanged)
    Q_PROPERTY(qreal panY READ panY NOTIFY presentationChanged)
    Q_PROPERTY(bool remoteCursorVisible READ remoteCursorVisible NOTIFY presentationChanged)
    Q_PROPERTY(qreal remoteCursorX READ remoteCursorX NOTIFY presentationChanged)
    Q_PROPERTY(qreal remoteCursorY READ remoteCursorY NOTIFY presentationChanged)
    Q_PROPERTY(QString liveSnapDragMediaId READ liveSnapDragMediaId NOTIFY presentationChanged)
    Q_PROPERTY(qreal liveSnapDragX READ liveSnapDragX NOTIFY presentationChanged)
    Q_PROPERTY(qreal liveSnapDragY READ liveSnapDragY NOTIFY presentationChanged)
    Q_PROPERTY(bool liveResizeActive READ liveResizeActive NOTIFY presentationChanged)
    Q_PROPERTY(QString liveResizeMediaId READ liveResizeMediaId NOTIFY presentationChanged)
    Q_PROPERTY(qreal liveResizeX READ liveResizeX NOTIFY presentationChanged)
    Q_PROPERTY(qreal liveResizeY READ liveResizeY NOTIFY presentationChanged)
    Q_PROPERTY(qreal liveResizeScale READ liveResizeScale NOTIFY presentationChanged)
    Q_PROPERTY(bool liveAltResizeActive READ liveAltResizeActive NOTIFY presentationChanged)
    Q_PROPERTY(QString liveAltResizeMediaId READ liveAltResizeMediaId NOTIFY presentationChanged)
    Q_PROPERTY(qreal liveAltResizeX READ liveAltResizeX NOTIFY presentationChanged)
    Q_PROPERTY(qreal liveAltResizeY READ liveAltResizeY NOTIFY presentationChanged)
    Q_PROPERTY(qreal liveAltResizeWidth READ liveAltResizeWidth NOTIFY presentationChanged)
    Q_PROPERTY(qreal liveAltResizeHeight READ liveAltResizeHeight NOTIFY presentationChanged)
    Q_PROPERTY(qreal liveAltResizeScale READ liveAltResizeScale NOTIFY presentationChanged)

public:
    explicit QuickCanvasController(CanvasDocument* document,
                                   QObject* parent = nullptr);
    ~QuickCanvasController() override;

    bool initialize(QString* errorMessage = nullptr);
    QQuickWindow* renderWindow() const;

    CanvasDocument* document() const { return m_document; }
    MediaListModel* mediaListModel() const { return m_mediaListModel; }
    CanvasMedia* selectedMediaItem() const;
    QObject* mediaModel() const;
    QVariantList mediaSnapshot() const { return m_mediaSnapshot; }
    QVariantList selectionChromeModel() const { return m_selectionChromeModel; }
    QVariantList screensModel() const { return m_screensModel; }
    QVariantList uiZonesModel() const { return m_uiZonesModel; }
    QVariantList snapGuidesModel() const { return m_snapGuidesModel; }
    QVariantMap videoStateModel() const { return m_videoStateModel; }
    QVariantMap dropPreviewModel() const { return m_dropPreviewModel; }
    QObject* dropPreviewFrameSource() const;
    bool remoteActive() const { return m_shellActive; }
    bool projectEditingEnabled() const { return m_projectEditingEnabled; }
    void setProjectEditingEnabled(bool enabled);
    bool textToolActive() const { return m_textToolActive; }
    qreal viewScale() const { return m_viewScale; }
    qreal panX() const { return m_panX; }
    qreal panY() const { return m_panY; }
    bool remoteCursorVisible() const { return m_remoteCursorVisible; }
    qreal remoteCursorX() const { return m_remoteCursorX; }
    qreal remoteCursorY() const { return m_remoteCursorY; }
    QString liveSnapDragMediaId() const { return m_liveSnapDragMediaId; }
    qreal liveSnapDragX() const { return m_liveSnapDragX; }
    qreal liveSnapDragY() const { return m_liveSnapDragY; }
    bool liveResizeActive() const { return m_liveResizeActive; }
    QString liveResizeMediaId() const { return m_liveResizeMediaId; }
    qreal liveResizeX() const { return m_liveResizeRect.x(); }
    qreal liveResizeY() const { return m_liveResizeRect.y(); }
    qreal liveResizeScale() const { return m_liveResizeScale; }
    bool liveAltResizeActive() const { return m_liveAltResizeActive; }
    QString liveAltResizeMediaId() const { return m_liveAltResizeMediaId; }
    qreal liveAltResizeX() const { return m_liveAltResizeRect.x(); }
    qreal liveAltResizeY() const { return m_liveAltResizeRect.y(); }
    qreal liveAltResizeWidth() const { return m_liveAltResizeRect.width(); }
    qreal liveAltResizeHeight() const { return m_liveAltResizeRect.height(); }
    qreal liveAltResizeScale() const { return m_liveAltResizeScale; }
    bool editsLocked() const;
    void refreshMediaProjection();
    void selectMedia(const QString& mediaId, bool additive = false);

    void setShellActive(bool active);
    void updateRemoteCursor(int globalX, int globalY);
    void hideRemoteCursor();
    void resetView();
    void recenterView();
    void setTextToolActive(bool active);
    qreal currentViewScale() const;
    void ensureInitialFit(int marginPx = 53);

    bool beginLocalFileDrag(const QVariantList& urls, qreal viewX, qreal viewY);
    bool updateLocalFileDrag(qreal viewX, qreal viewY);
    bool commitLocalFileDrop(qreal viewX, qreal viewY);
    void cancelLocalFileDrag();
    Q_INVOKABLE void registerWindow(QQuickWindow* window);
    Q_INVOKABLE void updateCamera(qreal scale, qreal panX, qreal panY);

signals:
    void presentationChanged();
    void mediaSnapshotChanged();
    void selectionChromeModelChanged();
    void textToolActiveChanged();
    void selectedMediaChanged();
    void mediaVisibilityToggleRequested(const QString& mediaId, bool visible);
    void mediaBringForwardRequested(const QString& mediaId);
    void mediaBringBackwardRequested(const QString& mediaId);
    void mediaDeleteRequested(const QString& mediaId);
    void mediaPlayPauseRequested(const QString& mediaId);
    void mediaStopRequested(const QString& mediaId);
    void mediaRepeatToggleRequested(const QString& mediaId);
    void mediaMuteToggleRequested(const QString& mediaId);
    void mediaVolumeChangeRequested(const QString& mediaId, qreal value);
    void mediaSeekRequested(const QString& mediaId, qreal ratio);
    void mediaFitToTextToggleRequested(const QString& mediaId);
    void mediaHorizontalAlignRequested(const QString& mediaId,
                                       const QString& alignment);
    void mediaVerticalAlignRequested(const QString& mediaId,
                                     const QString& alignment);

public slots:
    void handleMediaSelectRequested(const QString& mediaId, bool additive);
    void handleClearSelectionRequested();
    void handleMediaMoveStarted(const QString& mediaId, qreal sceneX,
                                qreal sceneY, bool snap);
    void handleMediaMoveUpdated(const QString& mediaId, qreal sceneX,
                                qreal sceneY, bool snap);
    void handleMediaMoveEnded(const QString& mediaId, qreal sceneX,
                              qreal sceneY, bool snap);
    void handleMediaResizeRequested(const QString& mediaId,
                                    const QString& handleId,
                                    qreal sceneX, qreal sceneY,
                                    bool snap, bool altPressed);
    void handleMediaResizeEnded(const QString& mediaId);
    void handleTextCommitRequested(const QString& mediaId,
                                   const QString& text);
    void handleTextLiveUpdateRequested(const QString& mediaId,
                                       const QString& text);
    void handleTextCreateRequested(qreal viewX, qreal viewY);
    void handleOverlayVisibilityToggle(const QString& mediaId, bool visible);
    void handleOverlayBringForward(const QString& mediaId);
    void handleOverlayBringBackward(const QString& mediaId);
    void handleOverlayDelete(const QString& mediaId);
    void handleOverlayPlayPause(const QString& mediaId);
    void handleOverlayStop(const QString& mediaId);
    void handleOverlayRepeatToggle(const QString& mediaId);
    void handleOverlayMuteToggle(const QString& mediaId);
    void handleOverlayVolumeChange(const QString& mediaId, qreal value);
    void handleOverlaySeekBegin(const QString& mediaId, qreal ratio);
    void handleOverlaySeekUpdate(const QString& mediaId, qreal ratio);
    void handleOverlaySeekEnd(const QString& mediaId, qreal ratio);
    void handleOverlayFitToTextToggle(const QString& mediaId);
    void handleOverlayHorizontalAlign(const QString& mediaId,
                                      const QString& alignment);
    void handleOverlayVerticalAlign(const QString& mediaId,
                                    const QString& alignment);
    void handleDropPreviewContentReady(const QString& mediaId);

private:
    void publishAll();
    void publishMedia();
    void publishSelection();
    void publishScreens();
    void publishRemoteCursor();
    void publishVideoState();
    void publishDropPreview(bool visible, const QString& handoffId = {});
    void publishSnapGuides(const QVariantList& guides);
    QPointF mapViewPointToScene(const QPointF& viewPoint) const;
    QPointF snappedPosition(CanvasMedia* media, const QPointF& proposed,
                            QVariantList* guides) const;
    QRectF snappedResizeRect(const QRectF& proposed, const QRectF& original,
                             const QString& handle, bool altPressed,
                             QVariantList* guides);
    void rebuildSnapTargets(CanvasMedia* activeMedia);
    void clearSnapTargets();
    void resetResizeSnapState();
    void appendAlignedResizeGuides(const QRectF& rect, bool snappedX,
                                   bool snappedY, QVariantList* guides) const;
    static QRectF resizedRect(const QRectF& original, const QString& handle,
                              const QPointF& movingPoint, bool uniform);
    void clearLiveResize();

    QPointer<CanvasDocument> m_document;
    QPointer<QQuickWindow> m_renderWindow;
    MediaListModel* m_mediaListModel = nullptr;
    RemoteVideoFrameSource* m_dropFrameSource = nullptr;
    QTimer* m_videoStateTimer = nullptr;
    bool m_textToolActive = false;
    bool m_shellActive = false;
    bool m_projectEditingEnabled = false;
    bool m_initialFitDone = false;
    QString m_lastSelectedId;
    QString m_dragMediaId;
    QPointF m_lastSnappedPosition;
    bool m_lastMoveSnapped = false;
    QString m_resizeMediaId;
    QString m_resizeHandleId;
    QRectF m_resizeOriginalRect;
    qreal m_resizeOriginalScale = 1.0;
    QRectF m_pendingResizeRect;
    bool m_pendingResizeAlt = false;
    QVector<QRectF> m_snapTargetRects;
    QVector<qreal> m_snapEdgesX;
    QVector<qreal> m_snapEdgesY;
    QVector<qreal> m_snapCentersX;
    QVector<qreal> m_snapCentersY;
    QVector<QPointF> m_snapCorners;
    bool m_resizeSnapModeAlt = false;
    bool m_resizeSnapBoxActive = false;
    QRectF m_resizeSnapBox;
    bool m_resizeSnapCornerActive = false;
    QPointF m_resizeSnapCorner;
    bool m_resizeSnapXActive = false;
    qreal m_resizeSnapX = 0.0;
    bool m_resizeSnapYActive = false;
    qreal m_resizeSnapY = 0.0;
    QString m_dropPath;
    QSize m_dropNativeSize;
    bool m_dropVideo = false;
    QPointF m_dropCenter;
    QImage m_dropFrame;
    QHash<QString, QPointer<RemoteVideoFrameSource>> m_videoPosterSources;
    QVariantList m_mediaSnapshot;
    QVariantList m_selectionChromeModel;
    QVariantList m_screensModel;
    QVariantList m_uiZonesModel;
    QVariantList m_snapGuidesModel;
    QVariantMap m_videoStateModel;
    QVariantMap m_dropPreviewModel{{QStringLiteral("visible"), false}};
    qreal m_viewScale = 1.0;
    qreal m_panX = 0.0;
    qreal m_panY = 0.0;
    bool m_remoteCursorVisible = false;
    qreal m_remoteCursorX = 0.0;
    qreal m_remoteCursorY = 0.0;
    QString m_liveSnapDragMediaId;
    qreal m_liveSnapDragX = 0.0;
    qreal m_liveSnapDragY = 0.0;
    bool m_liveResizeActive = false;
    QString m_liveResizeMediaId;
    QRectF m_liveResizeRect;
    qreal m_liveResizeScale = 1.0;
    bool m_liveAltResizeActive = false;
    QString m_liveAltResizeMediaId;
    QRectF m_liveAltResizeRect;
    qreal m_liveAltResizeScale = 1.0;
};

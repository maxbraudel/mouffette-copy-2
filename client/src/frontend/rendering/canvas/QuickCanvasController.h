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
    Q_PROPERTY(QString primarySelectedMediaId READ primarySelectedMediaId NOTIFY selectedMediaChanged)
    Q_PROPERTY(QObject* mediaModel READ mediaModel CONSTANT)
    Q_PROPERTY(bool editingEnabled READ editingEnabled NOTIFY editingEnabledChanged)
    Q_PROPERTY(QVariantMap liveTransforms READ liveTransforms NOTIFY liveTransformsChanged)
    Q_PROPERTY(QVariantList mediaSnapshot READ mediaSnapshot NOTIFY mediaSnapshotChanged)
    Q_PROPERTY(QVariantList selectionChromeModel READ selectionChromeModel NOTIFY selectionChromeModelChanged)
    Q_PROPERTY(QVariantList screensModel READ screensModel NOTIFY presentationChanged)
    Q_PROPERTY(QVariantList uiZonesModel READ uiZonesModel NOTIFY presentationChanged)
    Q_PROPERTY(QVariantList snapGuidesModel READ snapGuidesModel NOTIFY presentationChanged)
    Q_PROPERTY(QVariantMap videoStateModel READ videoStateModel NOTIFY presentationChanged)
    Q_PROPERTY(bool remoteActive READ remoteActive NOTIFY presentationChanged)
    Q_PROPERTY(bool textToolActive READ textToolActive NOTIFY presentationChanged)
    Q_PROPERTY(qreal viewScale READ viewScale NOTIFY presentationChanged)
    Q_PROPERTY(qreal panX READ panX NOTIFY presentationChanged)
    Q_PROPERTY(qreal panY READ panY NOTIFY presentationChanged)
    Q_PROPERTY(int remoteCursorDiameter READ remoteCursorDiameter CONSTANT)
    Q_PROPERTY(bool remoteCursorVisible READ remoteCursorVisible NOTIFY remoteCursorChanged)
    Q_PROPERTY(qreal remoteCursorX READ remoteCursorX NOTIFY remoteCursorChanged)
    Q_PROPERTY(qreal remoteCursorY READ remoteCursorY NOTIFY remoteCursorChanged)
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
    QString primarySelectedMediaId() const;
    void discardPendingEdits();
    QObject* mediaModel() const;
    QVariantList mediaSnapshot() const { return m_mediaSnapshot; }
    QVariantList selectionChromeModel() const { return m_selectionChromeModel; }
    QVariantList screensModel() const { return m_screensModel; }
    QVariantList uiZonesModel() const { return m_uiZonesModel; }
    QVariantList snapGuidesModel() const { return m_snapGuidesModel; }
    QVariantMap videoStateModel() const { return m_videoStateModel; }
    bool remoteActive() const { return m_shellActive; }
    bool projectEditingEnabled() const { return m_projectEditingEnabled; }
    bool editingEnabled() const { return m_projectEditingEnabled && !editsLocked(); }
    QVariantMap liveTransforms() const { return m_liveTransforms; }
    void setProjectEditingEnabled(bool enabled);
    bool textToolActive() const { return m_textToolActive; }
    qreal viewScale() const { return m_viewScale; }
    qreal panX() const { return m_panX; }
    qreal panY() const { return m_panY; }
    int remoteCursorDiameter() const;
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
    void updateRemoteCursor(int screenId, const QPointF& screenPosition);
    void hideRemoteCursor();
    Q_INVOKABLE void resetView();
    Q_INVOKABLE void recenterView(int marginPx = 53);
    Q_INVOKABLE bool fitToScreens(int marginPx = 53);
    Q_INVOKABLE bool fitToBounds(qreal x, qreal y, qreal width, qreal height,
                                qreal marginPx = 53);
    void setTextToolActive(bool active);
    qreal currentViewScale() const;
    void ensureInitialFit(int marginPx = 53);

    bool beginLocalFileDrag(const QVariantList& urls, qreal viewX, qreal viewY);
    bool updateLocalFileDrag(qreal viewX, qreal viewY);
    bool commitLocalFileDrop(qreal viewX, qreal viewY);
    void cancelLocalFileDrag();
    Q_INVOKABLE void registerWindow(QQuickWindow* window);
    Q_INVOKABLE void setViewportSize(qreal width, qreal height);
    Q_INVOKABLE void updateCamera(qreal scale, qreal panX, qreal panY);
    Q_INVOKABLE void panBy(qreal dx, qreal dy);
    Q_INVOKABLE void zoomAt(qreal x, qreal y, qreal factor);
    Q_INVOKABLE void scaleSelectionBy(qreal factor);
    Q_INVOKABLE void updateSelectionScaleGesture(qreal factor, bool phased);
    Q_INVOKABLE void finishSelectionScaleGesture();

signals:
    void editingEnabledChanged();
    void pendingEditsCanceled();
    void presentationChanged();
    void remoteCursorChanged();
    void liveTransformsChanged();
    void mediaSnapshotChanged();
    void selectionChromeModelChanged();
    void textToolActiveChanged();
    void textEditingRequested(const QString& mediaId);
    void selectedMediaChanged();
    void mediaVisibilityToggleRequested(const QString& mediaId, bool visible);
    void mediaBringForwardRequested(const QString& mediaId);
    void mediaBringBackwardRequested(const QString& mediaId);
    void mediaDeleteRequested(const QString& mediaId);
    void mediaMuteToggleRequested(const QString& mediaId);
    void mediaVolumeChangeRequested(const QString& mediaId, qreal value);
    void mediaFitToTextToggleRequested(const QString& mediaId);
    void mediaHorizontalAlignRequested(const QString& mediaId,
                                       const QString& alignment);
    void mediaVerticalAlignRequested(const QString& mediaId,
                                     const QString& alignment);

public slots:
    void copySelectedMedia();
    void pasteMedia();
    void deleteSelectedMedia();
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
    void handleOverlayMuteToggle(const QString& mediaId);
    void handleOverlayVolumeChange(const QString& mediaId, qreal value);
    void handleOverlayFitToTextToggle(const QString& mediaId);
    void handleOverlayHorizontalAlign(const QString& mediaId,
                                      const QString& alignment);
    void handleOverlayVerticalAlign(const QString& mediaId,
                                    const QString& alignment);

private:
    void publishAll();
    void publishCamera();
    void publishMedia();
    void publishSelection();
    void publishScreens();
    void publishRemoteCursor();
    void publishVideoState();
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
    void cancelPendingEdits();
    void captureTransformSelection(CanvasMedia* activeMedia);
    void previewMove(const QPointF& position);
    void previewResize();
    void commitTransforms(bool resize, bool alt);
    void flushSelectionScalePreview();

    struct TransformStart {
        QPointer<CanvasMedia> media;
        QRectF rect;
        QSize baseSize;
        qreal scale = 1.0;
    };
    QHash<QString, TransformStart> m_transformStarts;
    QVariantMap m_liveTransforms;
    bool m_scaleGestureActive = false;
    bool m_scalePreviewPending = false;
    qreal m_scaleGestureFactor = 1.0;
    qreal m_scaleMinimumFactor = 1.0;
    QTimer* m_scaleGestureEndTimer = nullptr;
    QMetaObject::Connection m_scaleFrameConnection;

    QPointer<CanvasDocument> m_document;
    QPointer<QQuickWindow> m_renderWindow;
    MediaListModel* m_mediaListModel = nullptr;
    QTimer* m_videoStateTimer = nullptr;
    bool m_textToolActive = false;
    bool m_shellActive = false;
    bool m_projectEditingEnabled = false;
    QSizeF m_viewportSize;
    int m_initialFitMargin = 53;
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
    QVariantList m_mediaSnapshot;
    QVariantList m_selectionChromeModel;
    QVariantList m_screensModel;
    QVariantList m_uiZonesModel;
    QVariantList m_snapGuidesModel;
    QVariantMap m_videoStateModel;
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

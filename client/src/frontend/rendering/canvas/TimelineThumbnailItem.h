#pragma once

#include "backend/media/ResidentMediaAsset.h"
#include <QQuickItem>

// One scene-graph strip per visible clip. Holds no residency lease or decoder;
// repeated samples share a texture and zoom only updates visible quads.
class TimelineThumbnailItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString ownerId READ ownerId WRITE setOwnerId NOTIFY ownerIdChanged)
    Q_PROPERTY(qreal sourceInMs READ sourceInMs WRITE setSourceInMs NOTIFY layoutChanged)
    Q_PROPERTY(qreal pixelsPerMs READ pixelsPerMs WRITE setPixelsPerMs NOTIFY layoutChanged)
    Q_PROPERTY(qreal visibleLeft READ visibleLeft WRITE setVisibleLeft NOTIFY layoutChanged)
    Q_PROPERTY(qreal visibleRight READ visibleRight WRITE setVisibleRight NOTIFY layoutChanged)
    Q_PROPERTY(bool hasThumbnails READ hasThumbnails NOTIFY hasThumbnailsChanged)
public:
    explicit TimelineThumbnailItem(QQuickItem* parent = nullptr);
    QString ownerId() const { return m_ownerId; }
    void setOwnerId(const QString& value);
    qreal sourceInMs() const { return m_sourceInMs; }
    void setSourceInMs(qreal value);
    qreal pixelsPerMs() const { return m_pixelsPerMs; }
    void setPixelsPerMs(qreal value);
    qreal visibleLeft() const { return m_visibleLeft; }
    void setVisibleLeft(qreal value);
    qreal visibleRight() const { return m_visibleRight; }
    void setVisibleRight(qreal value);
    bool hasThumbnails() const { return m_hasThumbnails; }
signals:
    void ownerIdChanged();
    void layoutChanged();
    void hasThumbnailsChanged();
protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& geometry, const QRectF& oldGeometry) override;
private:
    void refresh();
    QString m_ownerId;
    std::weak_ptr<const ResidentMediaAsset> m_asset;
    qreal m_sourceInMs = 0;
    qreal m_pixelsPerMs = 1;
    qreal m_visibleLeft = 0;
    qreal m_visibleRight = 0;
    bool m_hasThumbnails = false;
};

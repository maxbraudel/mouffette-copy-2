#pragma once

#include "backend/media/ResidentMediaAsset.h"
#include <QQuickItem>
#include <QHash>
#include <QSet>
#include <QTimer>

// A source-time filmstrip, projected into viewport pixels. Only visible cells
// retain images; the neighbourhood borrows the scheduler's bounded cache.
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
    ~TimelineThumbnailItem() override;
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
    quint64 retainedThumbnailBytes() const;
    static constexpr quint64 VisibleImageLimit = 2ULL * 1024 * 1024;
signals:
    void ownerIdChanged();
    void layoutChanged();
    void hasThumbnailsChanged();
protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& geometry, const QRectF& oldGeometry) override;
private:
    using Image = std::shared_ptr<const QImage>;
    struct Tile {
        qreal x = 0, width = 0;
        qint64 timeUs = 0; // Nominal grid time, never shifted to a VFR frame PTS.
        int frameIndex = -1;
        Image image;
        qint64 imageTimeUs = 0;
        int imageFrameIndex = -1;
    };
    void refresh();
    void rebuildLayout();
    void requestVisible();
    void clearRequests();
    QVector<Tile> cells(qreal left, qreal right, const ResidentMediaAsset* asset,
                        bool video, const QSize& displaySize) const;
    QTimer m_requestTimer;
    QVector<Tile> m_tiles;
    QHash<int, std::weak_ptr<const QImage>> m_samples;
    QSet<int> m_failedSamples;
    QSet<int> m_pendingSamples;
    QSet<int> m_pendingVisibleSamples;
    quint64 m_generation = 0;
    QString m_ownerId;
    QString m_sourceHash;
    std::weak_ptr<const ResidentMediaAsset> m_asset;
    qreal m_stepMs = 0;
    qreal m_sourceInMs = 0;
    qreal m_pixelsPerMs = 1;
    qreal m_visibleLeft = 0;
    qreal m_visibleRight = 0;
    bool m_hasThumbnails = false;
};

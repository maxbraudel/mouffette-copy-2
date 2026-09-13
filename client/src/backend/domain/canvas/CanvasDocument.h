#pragma once

#include "backend/domain/models/ClientInfo.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QStringList>

class CanvasMedia;
class FileManager;

// The complete persistent authoring graph. It has no rendering dependency and
// is safe to use from the application runtime, upload code and Qt Quick view
// models without a hidden widget or scene.
class CanvasDocument final : public QObject
{
    Q_OBJECT

public:
    explicit CanvasDocument(QObject* parent = nullptr);
    ~CanvasDocument() override;

    void setFileManager(FileManager* manager) { m_fileManager = manager; }
    FileManager* fileManager() const { return m_fileManager; }
    void setCanvasSessionId(const QString& id) { m_canvasSessionId = id; }
    QString canvasSessionId() const { return m_canvasSessionId; }

    QList<CanvasMedia*> media() const { return m_media; }
    CanvasMedia* mediaById(const QString& mediaId) const;
    CanvasMedia* addText(const QPointF& position,
                         const QString& text = QStringLiteral("Text"));
    CanvasMedia* addPreparedFile(const QString& sourcePath,
                                 const QSize& nativeSize,
                                 bool video,
                                 const QPointF& position);
    bool removeMedia(const QString& mediaId);
    void clear();
    void moveForward(const QString& mediaId);
    void moveBackward(const QString& mediaId);

    QStringList selectedMediaIds() const;
    CanvasMedia* selectedMedia() const;
    void select(const QString& mediaId, bool additive = false);
    void clearSelection();

    void setScreens(const QList<ScreenInfo>& screens);
    QList<ScreenInfo> screens() const { return m_screens; }
    QHash<int, QRectF> screenRects() const { return m_screenRects; }
    bool hasActiveScreens() const { return !m_screens.isEmpty(); }

    void setCamera(qreal scale, qreal panX, qreal panY);
    qreal cameraScale() const { return m_cameraScale; }
    qreal cameraPanX() const { return m_cameraPanX; }
    qreal cameraPanY() const { return m_cameraPanY; }
    void resetCamera();

    void setRemoteCursor(bool visible, const QPointF& scenePosition);
    bool remoteCursorVisible() const { return m_remoteCursorVisible; }
    QPointF remoteCursorPosition() const { return m_remoteCursorPosition; }
    bool mapRemoteCursor(int globalX, int globalY, QPointF* scenePosition) const;

    void setEditsLocked(bool locked);
    bool editsLocked() const { return m_editsLocked; }
    void setContentAvailable(bool available);
    bool contentAvailable() const { return m_contentAvailable; }

    QJsonObject serializeSceneState() const;
    QJsonObject serializeProjectState() const;
    bool restoreProjectState(const QJsonObject& state,
                             const QHash<QString, QString>& sourcePathByMediaId,
                             QStringList* skippedMediaIds = nullptr);

signals:
    void mediaAdded(CanvasMedia* media);
    void mediaAboutToBeRemoved(CanvasMedia* media);
    void mediaRemoved(const QString& mediaId);
    void mediaChanged(const QString& mediaId);
    void selectionChanged();
    void screensChanged();
    void cameraChanged();
    void remoteCursorChanged();
    void editsLockedChanged();
    void contentAvailabilityChanged();
    void documentChanged();

private:
    void adoptMedia(CanvasMedia* media);
    void rebuildScreenRects();
    qreal nextZ() const;

    QList<CanvasMedia*> m_media;
    QList<ScreenInfo> m_screens;
    QHash<int, QRectF> m_screenRects;
    FileManager* m_fileManager = nullptr;
    QString m_canvasSessionId;
    qreal m_cameraScale = 1.0;
    qreal m_cameraPanX = 0.0;
    qreal m_cameraPanY = 0.0;
    bool m_remoteCursorVisible = false;
    QPointF m_remoteCursorPosition;
    bool m_editsLocked = false;
    bool m_contentAvailable = true;
};

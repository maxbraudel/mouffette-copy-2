#pragma once

#include "backend/domain/models/ClientInfo.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QStringList>
#include <QSet>

#include <atomic>
#include <memory>

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
    void setClientWorkspaceId(const QString& id);
    quint64 importGeneration() const { return m_importGeneration; }
    QString projectId() const { return m_projectId; }

    QList<CanvasMedia*> media() const { return m_media; }
    CanvasMedia* mediaById(const QString& mediaId) const;
    // A zero initial height preserves the default scale for noninteractive callers.
    CanvasMedia* addText(const QPointF& position,
                         const QString& text = QStringLiteral("Text"),
                         qreal initialSceneHeight = 0.0);
    CanvasMedia* addPreparedFile(const QString& sourcePath,
                                 const QSize& nativeSize,
                                 bool video,
                                 const QPointF& position);
    // Persist the accepted drop before metadata work starts. Its identity is
    // retained when the selected, exact-size CanvasMedia is created.
    QString queueFileImport(const QString& sourcePath, const QPointF& center);
    bool hasPendingImports() const { return !m_pendingImports.isEmpty(); }
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

    // Legacy pixel transform, converted on the first valid viewport size.
    void setCamera(qreal scale, qreal panX, qreal panY);
    bool hasCamera() const { return m_hasCamera; }
    bool hasNormalizedCamera() const { return m_hasNormalizedCamera; }
    QPointF cameraCenter() const { return m_cameraCenter; }
    qreal cameraSquareSceneSize() const { return m_cameraSquareSceneSize; }
    void setCameraView(const QPointF& center, qreal squareSceneSize);
    // Compatibility snapshot only: resizing must not change the logical camera
    // or emit cameraChanged (which schedules project autosave).
    void setCameraProjection(qreal scale, qreal panX, qreal panY);
    qreal cameraScale() const { return m_cameraScale; }
    qreal cameraPanX() const { return m_cameraPanX; }
    qreal cameraPanY() const { return m_cameraPanY; }
    void resetCamera();

    void updateRemoteCursor(int screenId, const QPointF& screenPosition);
    void hideRemoteCursor();
    bool remoteCursorVisible() const { return m_remoteCursorVisible; }
    QPointF remoteCursorPosition() const { return m_remoteCursorPosition; }
    bool mapRemoteCursor(int screenId, const QPointF& screenPosition,
                         QPointF* scenePosition) const;

    void setEditsLocked(bool locked);
    bool editsLocked() const { return m_editsLocked; }
    void setContentAvailable(bool available);
    bool contentAvailable() const { return m_contentAvailable; }

    QJsonObject serializeSceneState() const;
    QJsonObject serializeProjectState() const;
    bool restoreProjectState(const QJsonObject& state,
                             const QHash<QString, QString>& sourcePathByMediaId,
                             QStringList* skippedMediaIds = nullptr);

    QStringList pasteMediaState(const QJsonObject& state,
                                const QHash<QString, QString>& sourcePaths,
                                QStringList* skippedMediaIds = nullptr);

signals:
    void mediaAdded(CanvasMedia* media);
    void mediaAboutToBeRemoved(CanvasMedia* media);
    void mediaRemoved(const QString& mediaId);
    void mediaChanged(const QString& mediaId);
    void mediaSourceInvalidated(const QString& mediaId, const QString& reason);
    void mediaImportFailed(const QString& mediaId, const QString& path,
                           const QString& reason);
    void pendingImportsChanged();
    void selectionChanged();
    void screensChanged();
    void cameraChanged();
    void remoteCursorChanged();
    void editsLockedChanged();
    void contentAvailabilityChanged();
    void documentChanged();

private:
    struct PendingImport {
        QString mediaId;
        QString sourcePath;
        QString sourceSignature;
        QPointF center;
        std::shared_ptr<std::atomic_bool> cancelled;
    };
    void startPendingImport(const QString& mediaId);
    void cancelPendingImportTasks();
    void adoptMedia(CanvasMedia* media);
    QStringList insertProjectMedia(const QJsonObject& state,
                                  const QHash<QString, QString>& sourcePaths,
                                  QStringList* skippedMediaIds, bool freshIds);
    void rebuildScreenRects();
    void setRemoteCursor(bool visible, const QPointF& scenePosition);
    qreal nextZ() const;

    quint64 m_importGeneration = 0;
    QHash<QString, PendingImport> m_pendingImports;
    QSet<QString> m_activeImports;
    QList<CanvasMedia*> m_media;
    QList<ScreenInfo> m_screens;
    QHash<int, QRectF> m_screenRects;
    FileManager* m_fileManager = nullptr;
    QString m_projectId;
    bool m_hasCamera = false;
    bool m_hasNormalizedCamera = false;
    QPointF m_cameraCenter;
    qreal m_cameraSquareSceneSize = 1000.0;
    qreal m_cameraScale = 1.0;
    qreal m_cameraPanX = 0.0;
    qreal m_cameraPanY = 0.0;
    bool m_remoteCursorVisible = false;
    QPointF m_remoteCursorPosition;
    int m_remoteCursorScreenId = -1;
    QPointF m_remoteCursorScreenPosition;
    bool m_editsLocked = false;
    bool m_contentAvailable = true;
};

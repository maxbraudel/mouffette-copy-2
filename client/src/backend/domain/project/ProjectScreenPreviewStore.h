#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QVideoFrame>
#include <memory>

// Optional project snapshots, separate from live stream/decoder ownership.
// Public methods and signals belong to the GUI thread; image conversion and
// sidecar I/O run on a single worker. The directory is profile-specific.
class ProjectScreenPreviewStore final : public QObject {
    Q_OBJECT
public:
    explicit ProjectScreenPreviewStore(QString directory, QObject* parent = nullptr);
    ~ProjectScreenPreviewStore() override;

    void retain(const QString& projectId, int screenId, const QVideoFrame& frame);
    void restore(const QString& projectId, const QList<int>& screenIds);
    void flush();
    void waitForDone();
    bool clearAll();
    void removeProject(const QString& projectId);
    void pruneProjects(const QStringList& projectIds);

signals:
    void frameRestored(const QString& projectId, int screenId, const QImage& image);
    void persistenceError(const QString& message);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

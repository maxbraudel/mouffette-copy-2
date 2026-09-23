#include "ProjectScreenPreviewStore.h"
#include "backend/runtime/storage/StorageVersions.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <utility>

namespace {
constexpr int MaximumEdge = 3840;
constexpr qsizetype MaximumPendingFrames = 64;
constexpr qint64 MaximumPendingBytes = 192 * 1024 * 1024;
constexpr qint64 MaximumFileBytes = 64 * 1024 * 1024;

bool validProject(const QString& projectId)
{
    return !projectId.trimmed().isEmpty() && projectId.size() <= 4096;
}

bool validScreen(int screenId) { return screenId >= 0 && screenId <= 1000000; }

QString projectHash(const QString& projectId)
{
    return QString::fromLatin1(QCryptographicHash::hash(projectId.toUtf8(),
        QCryptographicHash::Sha256).toHex());
}

QString frameKey(const QString& hash, int screenId)
{
    return hash + QLatin1Char('/') + QString::number(screenId);
}

QString newGeneration() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

bool validGeneration(const QString& value)
{
    return !QUuid(value).isNull()
        && QUuid(value).toString(QUuid::WithoutBraces) == value;
}

bool prepareDirectory(const QString& directory)
{
    const QFileInfo info(directory);
    if (info.isSymLink() || (info.exists() && !info.isDir())) return false;
    return QDir().mkpath(directory);
}

bool saveGeneration(const QString& directory, const QString& generation)
{
    if (!prepareDirectory(directory)) return false;
    const QString path = QDir(directory).filePath(QStringLiteral("current.json"));
    if (QFileInfo(path).isSymLink()) return false;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(QJsonObject{{"schemaVersion", StorageVersions::ProjectScreenPreviews},
        {"generation", generation}}).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

QString saveFrame(const QString& generationDirectory, const QString& key,
                  const QVideoFrame& frame)
{
    const QString path = QDir(generationDirectory).filePath(key + QStringLiteral(".png"));
    if (QFileInfo(QFileInfo(generationDirectory).absolutePath()).isSymLink()
        || !prepareDirectory(generationDirectory)
        || !prepareDirectory(QFileInfo(path).absolutePath()) || QFileInfo(path).isSymLink())
        return QObject::tr("Could not create the project's screen preview directory.");
    const QImage image = frame.toImage();
    if (image.isNull()) return QObject::tr("Could not convert a project's screen preview.");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit())
        return QObject::tr("Could not save a project's screen preview: %1").arg(file.errorString());
    return {};
}

QImage readFrame(const QString& path)
{
    // The PNG leaf alone is insufficient: an owned generation/project
    // directory must not redirect a restore into somebody else's files.
    QString parent = QFileInfo(path).absolutePath();
    for (int depth = 0; depth < 3; ++depth) {
        if (QFileInfo(parent).isSymLink()) return {};
        parent = QFileInfo(parent).absolutePath();
    }
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile() || info.size() <= 0 || info.size() > MaximumFileBytes)
        return {};
    QImageReader reader(path, "PNG");
    const QSize size = reader.size();
    if (size.width() <= 0 || size.height() <= 0
        || size.width() > MaximumEdge || size.height() > MaximumEdge) return {};
    return reader.read();
}

bool removeDirectory(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) return true;
    if (info.isSymLink() || !info.isDir()) return QFile::remove(path);
    return QDir(path).removeRecursively();
}

bool removeObsoleteGenerations(const QString& directory, const QString& generation)
{
    if (QFileInfo(directory).isSymLink()) return false;
    bool success = true;
    const auto entries = QDir(directory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& entry : entries)
        if (entry.fileName() != generation && validGeneration(entry.fileName()))
            success &= removeDirectory(entry.absoluteFilePath());
    return success;
}
}

struct ProjectScreenPreviewStore::Private {
    struct Pending {
        QVideoFrame frame;
        qint64 bytes = 0;
        quint64 sequence = 0;
    };
    QString directory;
    QString generation;
    bool available = false;
    QThreadPool pool;
    QTimer timer;
    QHash<QString, Pending> pending;
    QHash<QString, Pending> inFlight;
    QHash<QString, quint64> revisions;
    QHash<QString, quint64> projectRevisions;
    QHash<QString, quint64> restoreRequests;
    QSet<QString> removedProjects;
    qint64 pendingBytes = 0;
    quint64 sequence = 0;
    quint64 jobSequence = 0;
    quint64 activeJob = 0;
    quint64 restoreSequence = 0;
    bool memoryWarningReported = false;

    QString generationDirectory() const { return QDir(directory).filePath(generation); }

    void discardProject(const QString& hash)
    {
        ++projectRevisions[hash];
        removedProjects.insert(hash);
        const QString prefix = hash + QLatin1Char('/');
        for (auto it = pending.begin(); it != pending.end();) {
            if (it.key().startsWith(prefix)) {
                pendingBytes -= it->bytes;
                it = pending.erase(it);
            } else ++it;
        }
        for (auto it = revisions.begin(); it != revisions.end();) {
            if (it.key().startsWith(prefix)) it = revisions.erase(it);
            else ++it;
        }
        for (auto it = restoreRequests.begin(); it != restoreRequests.end();) {
            if (it.key().startsWith(prefix)) it = restoreRequests.erase(it);
            else ++it;
        }
    }
};

ProjectScreenPreviewStore::ProjectScreenPreviewStore(QString directory, QObject* parent)
    : QObject(parent), d(std::make_unique<Private>())
{
    d->directory = QFileInfo(std::move(directory)).absoluteFilePath();
    d->pool.setMaxThreadCount(1);
    d->timer.setSingleShot(true);
    d->timer.setInterval(1000);
    connect(&d->timer, &QTimer::timeout, this, &ProjectScreenPreviewStore::flush);
    const QString manifest = QDir(d->directory).filePath(QStringLiteral("current.json"));
    QFile file(manifest);
    if (!QFileInfo(d->directory).isSymLink() && !QFileInfo(manifest).isSymLink()
        && file.open(QIODevice::ReadOnly) && file.size() <= 4096) {
        const auto object = QJsonDocument::fromJson(file.readAll()).object();
        if (object.value("schemaVersion").toInt() == StorageVersions::ProjectScreenPreviews
            && validGeneration(object.value("generation").toString())) {
            d->generation = object.value("generation").toString();
            d->available = !QFileInfo(d->directory).isSymLink();
        }
    }
    if (!d->available) {
        d->generation = newGeneration();
        d->available = saveGeneration(d->directory, d->generation);
    }
    if (!d->available) QTimer::singleShot(0, this, [this] {
        emit persistenceError(tr("Could not open the project's screen preview storage."));
    });
}

ProjectScreenPreviewStore::~ProjectScreenPreviewStore() { waitForDone(); }

void ProjectScreenPreviewStore::retain(const QString& projectId, int screenId,
                                        const QVideoFrame& frame)
{
    if (!d->available || !validProject(projectId) || !validScreen(screenId)
        || !frame.isValid() || frame.width() <= 0 || frame.height() <= 0
        || frame.width() > MaximumEdge || frame.height() > MaximumEdge) return;
    const QString hash = projectHash(projectId);
    const QString key = frameKey(hash, screenId);
    d->removedProjects.remove(hash);
    ++d->revisions[key];
    d->restoreRequests.remove(key);
    const qint64 bytes = qint64(frame.width()) * frame.height() * 4;
    if (d->pending.contains(key)) d->pendingBytes -= d->pending.take(key).bytes;
    // A shallow video handle is retained, never a GUI-thread RGB conversion.
    // Very large/many-screen layouts cannot build an unbounded write queue.
    while (!d->pending.isEmpty()
           && (d->pending.size() >= MaximumPendingFrames || d->pendingBytes + bytes > MaximumPendingBytes)) {
        auto oldest = d->pending.begin();
        for (auto it = d->pending.begin(); it != d->pending.end(); ++it)
            if (it->sequence < oldest->sequence) oldest = it;
        d->pendingBytes -= oldest->bytes;
        d->pending.erase(oldest);
        if (!d->memoryWarningReported) {
            d->memoryWarningReported = true;
            emit persistenceError(tr("Some screen previews could not be saved because their pending images exceeded the memory limit."));
        }
    }
    d->pending.insert(key, {frame, bytes, ++d->sequence});
    d->pendingBytes += bytes;
    if (!d->timer.isActive() && !d->activeJob) d->timer.start();
}

void ProjectScreenPreviewStore::flush()
{
    d->timer.stop();
    if (!d->available || d->activeJob || d->pending.isEmpty()) return;
    d->inFlight = std::move(d->pending);
    d->pending.clear();
    d->pendingBytes = 0;
    const quint64 job = d->activeJob = ++d->jobSequence;
    const QString directory = d->generationDirectory();
    const auto frames = d->inFlight;
    d->pool.start([this, job, directory, frames] {
        QStringList errors;
        for (auto it = frames.cbegin(); it != frames.cend(); ++it) {
            const QString error = saveFrame(directory, it.key(), it->frame);
            if (!error.isEmpty() && !errors.contains(error)) errors.append(error);
        }
        QMetaObject::invokeMethod(this, [this, job, errors] {
            for (const auto& error : errors) emit persistenceError(error);
            if (d->activeJob != job) return;
            d->activeJob = 0;
            d->inFlight.clear();
            if (!d->pending.isEmpty()) d->timer.start();
        }, Qt::QueuedConnection);
    });
}

void ProjectScreenPreviewStore::waitForDone()
{
    d->timer.stop();
    d->pool.waitForDone();
    d->activeJob = 0;
    d->inFlight.clear();
    flush();
    d->pool.waitForDone();
    d->activeJob = 0;
    d->inFlight.clear();
}

void ProjectScreenPreviewStore::restore(const QString& projectId, const QList<int>& screenIds)
{
    if (!d->available || !validProject(projectId)) return;
    const QString hash = projectHash(projectId);
    if (d->removedProjects.contains(hash)) return;
    const QString generation = d->generation;
    const QString directory = d->generationDirectory();
    const quint64 projectRevision = d->projectRevisions[hash];
    QSet<int> unique;
    for (const int screenId : screenIds) {
        if (!validScreen(screenId) || unique.contains(screenId) || unique.size() >= 64) continue;
        unique.insert(screenId);
        const QString key = frameKey(hash, screenId);
        if (d->restoreRequests.contains(key) || d->restoreRequests.size() >= 64) continue;
        const quint64 request = ++d->restoreSequence;
        d->restoreRequests.insert(key, request);
        const quint64 revision = d->revisions.value(key);
        // Reopening before the next checkpoint still uses the freshest image.
        const QVideoFrame pendingFrame = d->pending.contains(key) ? d->pending.value(key).frame
            : d->inFlight.value(key).frame;
        d->pool.start([this, projectId, hash, screenId, key, request, revision, projectRevision,
                       generation, directory, pendingFrame] {
            const QImage image = pendingFrame.isValid() ? pendingFrame.toImage()
                : readFrame(QDir(directory).filePath(key + QStringLiteral(".png")));
            QMetaObject::invokeMethod(this, [this, projectId, hash, screenId, key, request, revision,
                                           projectRevision, generation, image] {
                if (d->restoreRequests.value(key) != request) return;
                d->restoreRequests.remove(key);
                if (image.isNull() || !d->available || d->generation != generation
                    || d->removedProjects.contains(hash)
                    || d->projectRevisions.value(hash) != projectRevision
                    || d->revisions.value(key) != revision) return;
                emit frameRestored(projectId, screenId, image);
            }, Qt::QueuedConnection);
        });
    }
}

bool ProjectScreenPreviewStore::clearAll()
{
    d->timer.stop();
    d->pending.clear();
    d->pendingBytes = 0;
    d->inFlight.clear();
    d->revisions.clear();
    d->projectRevisions.clear();
    d->restoreRequests.clear();
    d->removedProjects.clear();
    d->memoryWarningReported = false;
    d->generation = newGeneration(); // Fence every queued restore, even on I/O failure.
    d->available = saveGeneration(d->directory, d->generation);
    if (!d->available) {
        emit persistenceError(tr("Could not erase the saved screen previews durably."));
        return false;
    }
    const QString directory = d->directory;
    const QString generation = d->generation;
    // Prior writes may finish only in obsolete generations. Cleanup runs
    // behind them; the manifest already makes their images unreachable.
    d->pool.start([this, directory, generation] {
        if (!removeObsoleteGenerations(directory, generation)) QMetaObject::invokeMethod(this, [this] {
            emit persistenceError(tr("Some obsolete screen preview files could not be removed."));
        }, Qt::QueuedConnection);
    });
    return true;
}

void ProjectScreenPreviewStore::removeProject(const QString& projectId)
{
    if (!validProject(projectId)) return;
    const QString hash = projectHash(projectId);
    d->discardProject(hash);
    const QString path = QDir(d->generationDirectory()).filePath(hash);
    d->pool.start([this, path] {
        const QString generationDirectory = QFileInfo(path).absolutePath();
        if (QFileInfo(generationDirectory).isSymLink()
            || QFileInfo(QFileInfo(generationDirectory).absolutePath()).isSymLink()
            || !removeDirectory(path)) QMetaObject::invokeMethod(this, [this] {
            emit persistenceError(tr("A deleted project's screen previews could not be removed."));
        }, Qt::QueuedConnection);
    });
}

void ProjectScreenPreviewStore::pruneProjects(const QStringList& projectIds)
{
    QSet<QString> keep;
    for (const auto& id : projectIds) if (validProject(id)) keep.insert(projectHash(id));
    QSet<QString> known;
    for (auto it = d->revisions.cbegin(); it != d->revisions.cend(); ++it)
        known.insert(it.key().section(QLatin1Char('/'), 0, 0));
    for (auto it = d->projectRevisions.cbegin(); it != d->projectRevisions.cend(); ++it)
        known.insert(it.key());
    for (const auto& hash : known) if (!keep.contains(hash)) d->discardProject(hash);
    const QString directory = d->generationDirectory();
    const QString root = d->directory;
    const QString generation = d->generation;
    d->pool.start([this, directory, root, generation, keep] {
        // A crash immediately after Hide can leave unreachable generations;
        // startup pruning completes their deferred deletion as well.
        const bool cleanedGenerations = removeObsoleteGenerations(root, generation);
        bool success = !QFileInfo(directory).isSymLink()
            && !QFileInfo(QFileInfo(directory).absolutePath()).isSymLink();
        const auto entries = success ? QDir(directory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)
                                     : QFileInfoList{};
        for (const auto& entry : entries)
            if (!keep.contains(entry.fileName())) success &= removeDirectory(entry.absoluteFilePath());
        success &= cleanedGenerations;
        if (!success) QMetaObject::invokeMethod(this, [this] {
            emit persistenceError(tr("Some deleted projects' screen previews could not be removed."));
        }, Qt::QueuedConnection);
    });
}

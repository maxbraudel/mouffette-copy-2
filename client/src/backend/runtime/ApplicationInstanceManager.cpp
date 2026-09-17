#include "backend/runtime/ApplicationInstanceManager.h"
#include "backend/config/AppConfig.h"
#include "backend/security/DeviceIdentityStore.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>

#include <limits>
#include <algorithm>
#include <utility>

namespace {
QString safeKey(const QString& input)
{
    return QString::fromLatin1(QCryptographicHash::hash(
        input.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
}

bool acquireRecoveringStaleLock(QLockFile& lock)
{
    // tryLock owns stale-PID detection, native locking and serialized stale
    // removal. Forcing removeStaleLockFile after contention can race a live
    // initializer and bypass Qt's .rmlock/recheck protocol.
    lock.setStaleLockTime(0);
    return lock.tryLock(0);
}
}

ApplicationInstanceManager::ApplicationInstanceManager(
    QString applicationKey,
    bool allowMultipleInstances,
    QString coordinationRoot,
    QObject* parent)
    : QObject(parent)
    , m_applicationKey(std::move(applicationKey))
    , m_allowMultipleInstances(allowMultipleInstances)
    , m_requestedCoordinationRoot(std::move(coordinationRoot))
{
}

ApplicationInstanceManager::~ApplicationInstanceManager()
{
    if (m_activationServer) {
        m_activationServer->close();
    }
    if (m_profileLock) {
        m_profileLock->unlock();
    }
    if (m_profile.isTemporary()) {
        const QString profile = QDir::cleanPath(m_profile.rootPath);
        const QString profilesRoot = QDir(m_coordinationRoot).filePath(QStringLiteral("profiles"));
        // A failure before assigning rootPath must never turn QDir("") into
        // a recursive removal of the process working directory.
        const bool owned = !m_profile.rootPath.isEmpty()
            && QFileInfo(profile).isAbsolute()
            && profile.startsWith(QDir::cleanPath(profilesRoot) + QLatin1Char('/'));
        const bool removed = !owned || (QFileInfo(profile).isSymLink()
            ? QFile::remove(profile) : QDir(profile).removeRecursively());
        if (!removed)
            qWarning() << "Temporary instance profile removal failed; next startup will retry"
                       << m_profile.rootPath;
    }
    if (m_slotLock) {
        m_slotLock->unlock();
    }
}

void ApplicationInstanceManager::releaseProfileLockForRemoval()
{
    m_profileLock.reset();
}

bool ApplicationInstanceManager::prepareCoordinationRoot(QString* errorMessage)
{
    if (!m_requestedCoordinationRoot.isEmpty()) {
        m_coordinationRoot = QDir::cleanPath(m_requestedCoordinationRoot);
    } else {
        QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (base.isEmpty()) {
            base = QDir::tempPath();
        }
        const QString perUserKey = m_applicationKey + QLatin1Char('\n') + QDir::homePath();
        m_coordinationRoot = QDir(base).filePath(
            QStringLiteral("mouffette-instances-%1").arg(safeKey(perUserKey)));
    }
    if (!QDir().mkpath(m_coordinationRoot)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot create instance coordination directory: %1")
                                .arg(m_coordinationRoot);
        }
        return false;
    }
    return true;
}

void ApplicationInstanceManager::cleanupAbandonedProfiles()
{
    const QDir profiles(QDir(m_coordinationRoot).filePath(QStringLiteral("profiles")));
    if (!profiles.exists()) {
        return;
    }
    const QFileInfoList entries = profiles.entryInfoList(
        {QStringLiteral("instance-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry : entries) {
        if (entry.isSymLink()) {
            // Never follow an untrusted abandoned-profile link outside the
            // coordination root.
            if (!QFile::remove(entry.absoluteFilePath())) {
                qWarning() << "instance_profile_cleanup_deferred"
                           << "reason=link_removal_failed" << "path=" << entry.absoluteFilePath();
            }
            continue;
        }
        QLockFile ownership(QDir(entry.absoluteFilePath()).filePath(QStringLiteral("active.lock")));
        if (!acquireRecoveringStaleLock(ownership)) {
            if (ownership.error() != QLockFile::LockFailedError) {
                qWarning() << "instance_profile_cleanup_deferred"
                           << "reason=ownership_unavailable" << "path=" << entry.absoluteFilePath();
            }
            continue;
        }
        ownership.unlock();
        if (!QDir(entry.absoluteFilePath()).removeRecursively()) {
            qWarning() << "instance_profile_cleanup_deferred"
                       << "reason=removal_failed" << "path=" << entry.absoluteFilePath();
        }
    }
}

bool ApplicationInstanceManager::acquireSlot(int ordinal, QString* errorMessage)
{
    auto candidate = std::make_unique<QLockFile>(
        QDir(m_coordinationRoot).filePath(QStringLiteral("slot-%1.lock").arg(ordinal)));
    if (!acquireRecoveringStaleLock(*candidate)) {
        if (candidate->error() != QLockFile::LockFailedError && errorMessage)
            *errorMessage = QStringLiteral("Cannot lock application instance slot %1").arg(ordinal);
        return false;
    }
    m_slotLock = std::move(candidate);
    m_profile.ordinal = ordinal;
    return true;
}

bool ApplicationInstanceManager::createTemporaryProfile(QString* errorMessage)
{
    m_profile.instanceId = DeviceIdentityStore::instanceIdForOrdinal(m_profile.ordinal);
    m_profile.profileId = QStringLiteral("instance-%1-%2")
                              .arg(m_profile.ordinal).arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_profile.persistent = false;
    const QString profilesRoot = QDir(m_coordinationRoot).filePath(QStringLiteral("profiles"));
    if (!QDir().mkpath(profilesRoot)) {
        if (errorMessage) *errorMessage = QStringLiteral("Cannot create temporary profile root");
        return false;
    }
    m_profile.rootPath = QDir(profilesRoot).filePath(m_profile.profileId);
    if (!QDir().mkpath(m_profile.rootPath)) {
        if (errorMessage) *errorMessage = QStringLiteral("Cannot create temporary instance profile");
        return false;
    }
    m_profileLock = std::make_unique<QLockFile>(
        QDir(m_profile.rootPath).filePath(QStringLiteral("active.lock")));
    m_profileLock->setStaleLockTime(0);
    if (!m_profileLock->tryLock(0)) {
        if (errorMessage) *errorMessage = QStringLiteral("Cannot lock temporary instance profile");
        return false;
    }
    return true;
}

QString ApplicationInstanceManager::activationServerName(int ordinal) const
{
    const QString primaryName = QStringLiteral("mouffette-activate-%1").arg(safeKey(
        m_applicationKey + QLatin1Char('\n') + m_coordinationRoot));
    return ordinal == 1 ? primaryName
        : primaryName + QStringLiteral("-%1").arg(ordinal);
}

bool ApplicationInstanceManager::startActivationServer(QString* errorMessage)
{
    m_activationServer = std::make_unique<QLocalServer>();
    const QString name = activationServerName(m_profile.ordinal);
    QLocalServer::removeServer(name);
    if (!m_activationServer->listen(name)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot create activation channel: %1")
                                .arg(m_activationServer->errorString());
        }
        return false;
    }
    connect(m_activationServer.get(), &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket* socket = m_activationServer->nextPendingConnection()) {
            socket->setParent(m_activationServer.get());
            connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
                if (socket->bytesAvailable() > 64) {
                    socket->disconnectFromServer();
                    return;
                }
                if (!socket->canReadLine()) return;
                const QByteArray request = socket->readLine().trimmed();
                if (request == QByteArrayLiteral("activate")) {
                    emit activationRequested();
                    socket->write("ok\n");
                    socket->flush();
                }
                socket->disconnectFromServer();
            });
        }
    });
    return true;
}

bool ApplicationInstanceManager::requestActivation(int ordinal) const
{
    const QString name = activationServerName(ordinal);
    const AppConfig& config = AppConfig::instance();
    for (int attempt = 0; attempt < 30; ++attempt) {
        QLocalSocket socket;
        socket.connectToServer(name, QIODevice::ReadWrite);
        if (socket.waitForConnected(config.instanceActivationConnectTimeoutMs())
            && socket.write("activate\n") == 9) {
            socket.flush();
            QElapsedTimer acknowledgementDeadline;
            acknowledgementDeadline.start();
            while (acknowledgementDeadline.elapsed()
                   < config.instanceActivationAckTimeoutMs()) {
                const int remainingMs = config.instanceActivationAckTimeoutMs()
                    - static_cast<int>(acknowledgementDeadline.elapsed());
                const int waitSliceMs = qMax(
                    1, qMin(config.instanceActivationRetryIntervalMs(),
                            remainingMs));
                // This also makes the class deterministic when two managers
                // are exercised on the same application thread.
                QCoreApplication::processEvents(QEventLoop::AllEvents,
                                                waitSliceMs);
                if (socket.canReadLine()
                    && socket.readLine().trimmed() == QByteArrayLiteral("ok")) {
                    return true;
                }
                if (socket.state() == QLocalSocket::UnconnectedState) {
                    break;
                }
                socket.waitForReadyRead(waitSliceMs);
            }
        }
        QThread::msleep(config.instanceActivationRetryIntervalMs());
    }
    return false;
}

ApplicationInstanceManager::StartResult
ApplicationInstanceManager::start(QString* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (!prepareCoordinationRoot(errorMessage)) {
        return StartResult::Failed;
    }
    // Cleanup must not observe another initializer between mkdir(profile)
    // and publication of its active.lock. Keep that whole transaction under
    // one short-lived coordination lock, using Qt's normal crash recovery.
    QLockFile initializationLock(
        QDir(m_coordinationRoot).filePath(QStringLiteral("initialization.lock")));
    initializationLock.setStaleLockTime(0);
    if (!initializationLock.tryLock(5000)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Cannot coordinate concurrent application startup");
        return StartResult::Failed;
    }
    cleanupAbandonedProfiles();
    m_profile.useNativeIdentityVault = m_requestedCoordinationRoot.isEmpty();
    if (!m_requestedCoordinationRoot.isEmpty()) {
        const QString persistentBase = QDir(m_coordinationRoot).filePath(
            QStringLiteral("persistent/%1").arg(m_profile.channel));
        m_profile.installationRootPath = QDir(persistentBase).filePath(QStringLiteral("installation"));
        m_profile.legacyPrimaryRootPath = QDir(persistentBase).filePath(QStringLiteral("instance-1"));
    } else {
        const QString platformBase = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString persistentBase = platformBase.isEmpty()
            ? QDir(QDir::homePath()).filePath(QStringLiteral(".mouffette/data")) : platformBase;
        m_profile.installationRootPath = RuntimeProfile::persistentInstallationRoot(persistentBase, m_profile.channel);
        m_profile.legacyPrimaryRootPath = RuntimeProfile::persistentRoot(persistentBase, m_profile.channel);
    }

    if (!m_allowMultipleInstances) {
        // A surviving secondary remains an application instance even after
        // #1 exits. The build flag limits launches; it must not renumber or
        // ignore processes already holding a different slot.
        QList<int> ordinals;
        const QFileInfoList slotFiles = QDir(m_coordinationRoot).entryInfoList(
            {QStringLiteral("slot-*.lock")}, QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo& slot : slotFiles) {
            const QString filename = slot.fileName();
            bool valid = false;
            const int ordinal = filename.mid(5, filename.size() - 10).toInt(&valid);
            if (valid && ordinal > 0 && filename == QStringLiteral("slot-%1.lock").arg(ordinal))
                ordinals.append(ordinal);
        }
        std::sort(ordinals.begin(), ordinals.end());
        for (const int ordinal : ordinals) {
            QLockFile candidate(QDir(m_coordinationRoot).filePath(QStringLiteral("slot-%1.lock").arg(ordinal)));
            if (acquireRecoveringStaleLock(candidate)) {
                candidate.unlock();
                continue;
            }
            if (candidate.error() != QLockFile::LockFailedError) {
                if (errorMessage) *errorMessage = QStringLiteral("Cannot inspect application instance slot %1").arg(ordinal);
                return StartResult::Failed;
            }
            initializationLock.unlock();
            if (requestActivation(ordinal)) {
                return StartResult::ActivatedExisting;
            }
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "Mouffette is already running but its activation channel did not respond");
            }
            return StartResult::Failed;
        }
        QString slotError;
        if (!acquireSlot(1, &slotError)) {
            if (errorMessage) *errorMessage = slotError.isEmpty()
                ? QStringLiteral("The primary application instance slot became unavailable") : slotError;
            return StartResult::Failed;
        }
    } else {
        for (int ordinal = 1; ordinal < std::numeric_limits<int>::max(); ++ordinal) {
            QString slotError;
            if (acquireSlot(ordinal, &slotError)) {
                break;
            }
            if (!slotError.isEmpty()) {
                if (errorMessage) *errorMessage = slotError;
                return StartResult::Failed;
            }
        }
        if (!m_slotLock) {
            if (errorMessage) *errorMessage = QStringLiteral("No application instance slot is available");
            return StartResult::Failed;
        }
    }

    if (m_profile.ordinal == 1) {
        m_profile.instanceId = QStringLiteral("primary");
        m_profile.profileId = QStringLiteral("instance-1");
        m_profile.persistent = true;
        m_profile.rootPath = m_profile.legacyPrimaryRootPath;
        const QFileInfo profileRoot(m_profile.rootPath);
        if (profileRoot.isSymLink() || (profileRoot.exists() && !profileRoot.isDir())
            || !QDir().mkpath(m_profile.rootPath)) {
            if (errorMessage) *errorMessage = QStringLiteral("Cannot prepare persistent runtime");
            return StartResult::Failed;
        }
        const QString lockPath = QDir(m_profile.rootPath).filePath(QStringLiteral("active.lock"));
        if (QFileInfo(lockPath).isSymLink()) {
            if (errorMessage) *errorMessage = QStringLiteral("Unsafe persistent runtime lock");
            return StartResult::Failed;
        }
        m_profileLock = std::make_unique<QLockFile>(lockPath);
        m_profileLock->setStaleLockTime(0);
        if (!m_profileLock->tryLock(0)) {
            if (errorMessage) *errorMessage = QStringLiteral("Persistent runtime is already in use");
            return StartResult::Failed;
        }
    } else if (!createTemporaryProfile(errorMessage)) {
        return StartResult::Failed;
    }
    if (!startActivationServer(errorMessage)) {
        return StartResult::Failed;
    }
    if (errorMessage) errorMessage->clear();
    return StartResult::Started;
}

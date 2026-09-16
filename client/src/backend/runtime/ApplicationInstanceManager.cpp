#include "backend/runtime/ApplicationInstanceManager.h"
#include "backend/config/AppConfig.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>

#include <limits>
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
    if (m_slotLock) {
        m_slotLock->unlock();
    }
    if (m_profile.isTemporary()) {
        QDir(m_profile.rootPath).removeRecursively();
    }
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
            QFile::remove(entry.absoluteFilePath());
            continue;
        }
        QLockFile ownership(QDir(entry.absoluteFilePath()).filePath(QStringLiteral("active.lock")));
        if (!acquireRecoveringStaleLock(ownership)) {
            continue;
        }
        ownership.unlock();
        QDir(entry.absoluteFilePath()).removeRecursively();
    }
}

bool ApplicationInstanceManager::acquireSlot(int ordinal)
{
    auto candidate = std::make_unique<QLockFile>(
        QDir(m_coordinationRoot).filePath(QStringLiteral("slot-%1.lock").arg(ordinal)));
    if (!acquireRecoveringStaleLock(*candidate)) {
        return false;
    }
    m_slotLock = std::move(candidate);
    m_profile.ordinal = ordinal;
    return true;
}

bool ApplicationInstanceManager::createTemporaryProfile(QString* errorMessage)
{
    m_profile.instanceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_profile.profileId = QStringLiteral("instance-%1-%2")
                              .arg(m_profile.ordinal).arg(m_profile.instanceId);
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
    if (!m_profileLock->tryLock(0)) {
        if (errorMessage) *errorMessage = QStringLiteral("Cannot lock temporary instance profile");
        return false;
    }
    return true;
}

QString ApplicationInstanceManager::activationServerName() const
{
    return QStringLiteral("mouffette-activate-%1").arg(safeKey(
        m_applicationKey + QLatin1Char('\n') + m_coordinationRoot));
}

bool ApplicationInstanceManager::startActivationServer(QString* errorMessage)
{
    m_activationServer = std::make_unique<QLocalServer>();
    const QString name = activationServerName();
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
                const QByteArray request = socket->readAll();
                if (request.startsWith("activate")) {
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

bool ApplicationInstanceManager::requestActivation() const
{
    const QString name = activationServerName();
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
    if (!prepareCoordinationRoot(errorMessage)) {
        return StartResult::Failed;
    }
    // Cleanup must not observe another initializer between mkdir(profile)
    // and publication of its active.lock. Keep that whole transaction under
    // one short-lived coordination lock, using Qt's normal crash recovery.
    QLockFile initializationLock(
        QDir(m_coordinationRoot).filePath(QStringLiteral("initialization.lock")));
    if (!initializationLock.tryLock(5000)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Cannot coordinate concurrent application startup");
        return StartResult::Failed;
    }
    cleanupAbandonedProfiles();

    if (!m_allowMultipleInstances) {
        if (!acquireSlot(1)) {
            initializationLock.unlock();
            if (requestActivation()) {
                return StartResult::ActivatedExisting;
            }
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "Mouffette is already running but its activation channel did not respond");
            }
            return StartResult::Failed;
        }
    } else {
        for (int ordinal = 1; ordinal < std::numeric_limits<int>::max(); ++ordinal) {
            if (acquireSlot(ordinal)) {
                break;
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
        if (!m_requestedCoordinationRoot.isEmpty()) {
            // Test/embedded callers that explicitly isolate coordination also
            // isolate the persistent runtime from the real user profile.
            m_profile.rootPath = QDir(m_coordinationRoot).filePath(
                QStringLiteral("persistent/instance-1"));
        } else {
            const QString base = QStandardPaths::writableLocation(
                QStandardPaths::AppDataLocation);
            const QString persistentBase = base.isEmpty()
                ? QDir(QDir::homePath()).filePath(QStringLiteral(".mouffette/data"))
                : base;
            m_profile.rootPath = QDir(persistentBase).filePath(
                QStringLiteral("runtimes/instance-1"));
        }
        if (!startActivationServer(errorMessage)) {
            return StartResult::Failed;
        }
    } else if (!createTemporaryProfile(errorMessage)) {
        return StartResult::Failed;
    }
    if (errorMessage) errorMessage->clear();
    return StartResult::Started;
}

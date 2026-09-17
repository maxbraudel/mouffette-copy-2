#ifndef APPLICATIONINSTANCEMANAGER_H
#define APPLICATIONINSTANCEMANAGER_H

#include "backend/runtime/RuntimeProfile.h"

#include <QObject>

#include <memory>

class QLocalServer;
class QLockFile;

class ApplicationInstanceManager final : public QObject {
    Q_OBJECT

public:
    enum class StartResult {
        Started,
        ActivatedExisting,
        Failed
    };

    ApplicationInstanceManager(QString applicationKey,
                               bool allowMultipleInstances,
                               QString coordinationRoot = QString(),
                               QObject* parent = nullptr);
    ~ApplicationInstanceManager() override;

    StartResult start(QString* errorMessage = nullptr);
    RuntimeProfileContext profile() const { return m_profile; }
    // Shutdown only: all profile consumers must already be destroyed. The
    // instance slot and activation server remain owned until this destructor.
    void releaseProfileLockForRemoval();
    QString coordinationRoot() const { return m_coordinationRoot; }

signals:
    void activationRequested();

private:
    bool prepareCoordinationRoot(QString* errorMessage);
    bool acquireSlot(int ordinal, QString* errorMessage);
    bool createTemporaryProfile(QString* errorMessage);
    bool startActivationServer(QString* errorMessage);
    bool requestActivation(int ordinal) const;
    void cleanupAbandonedProfiles();
    QString activationServerName(int ordinal) const;

    QString m_applicationKey;
    bool m_allowMultipleInstances = false;
    QString m_requestedCoordinationRoot;
    QString m_coordinationRoot;
    RuntimeProfileContext m_profile;
    std::unique_ptr<QLockFile> m_slotLock;
    std::unique_ptr<QLockFile> m_profileLock;
    std::unique_ptr<QLocalServer> m_activationServer;
};

#endif // APPLICATIONINSTANCEMANAGER_H

#ifndef RUNTIMESTORAGEBOOTSTRAP_H
#define RUNTIMESTORAGEBOOTSTRAP_H

#include "backend/runtime/RuntimeProfile.h"
#include "storage/StorageUpgradeEngine.h"

#include <QString>
#include <QStringList>

#include <functional>

class RuntimeStorageBootstrap final {
public:
    enum class Status {
        Success,
        SuccessWithReset,
        RecoverableFailure
    };

    enum class Stage {
        PreparingRuntime,
        PurgingCache,
        ValidatingSettings,
        ValidatingProjects,
        ValidatingHistory,
        ValidatingIdentity
    };

    struct Result {
        Status status = Status::RecoverableFailure;
        QString code;
        QString cause;
        QList<RuntimeStorage::Report> components;
        QStringList resetCategories;

        bool succeeded() const { return status != Status::RecoverableFailure; }
        bool hadReset() const { return status == Status::SuccessWithReset; }
    };

    using ProgressCallback = std::function<void(Stage)>;

    explicit RuntimeStorageBootstrap(RuntimeProfileContext context);

    Result run(const ProgressCallback& progress = {});
    static QString stageLabel(Stage stage);

private:
    RuntimeProfileContext m_context;
};

#endif // RUNTIMESTORAGEBOOTSTRAP_H

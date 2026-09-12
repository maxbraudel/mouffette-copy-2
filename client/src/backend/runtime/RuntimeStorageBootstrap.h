#ifndef RUNTIMESTORAGEBOOTSTRAP_H
#define RUNTIMESTORAGEBOOTSTRAP_H

#include "backend/runtime/RuntimeProfile.h"

#include <QString>
#include <QStringList>

#include <functional>

class RuntimeStorageBootstrap final {
public:
    static constexpr int StorageVersion = 1;

    enum class Status {
        Success,
        SuccessWithReset,
        RecoverableFailure
    };

    enum class Stage {
        PreparingRuntime,
        ValidatingManifest,
        PurgingCache,
        ValidatingSettings,
        ValidatingProjects,
        ValidatingIdentity
    };

    struct Result {
        Status status = Status::RecoverableFailure;
        QString code;
        QString cause;
        QString foundVersion;
        int expectedVersion = StorageVersion;
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

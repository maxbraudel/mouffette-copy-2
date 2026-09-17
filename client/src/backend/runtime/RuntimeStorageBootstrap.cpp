#include "RuntimeStorageBootstrap.h"
#include "storage/StorageRegistry.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <utility>

RuntimeStorageBootstrap::RuntimeStorageBootstrap(RuntimeProfileContext context)
    : m_context(std::move(context))
{
}

QString RuntimeStorageBootstrap::stageLabel(Stage stage)
{
    switch (stage) {
    case Stage::PreparingRuntime: return QStringLiteral("Preparing runtime");
    case Stage::PurgingCache: return QStringLiteral("Preparing received-media cache");
    case Stage::ValidatingSettings: return QStringLiteral("Preparing settings");
    case Stage::ValidatingProjects: return QStringLiteral("Preparing projects");
    case Stage::ValidatingHistory: return QStringLiteral("Preparing notification history");
    case Stage::ValidatingIdentity: return QStringLiteral("Preparing device identity");
    }
    return QStringLiteral("Preparing Mouffette");
}

RuntimeStorageBootstrap::Result RuntimeStorageBootstrap::run(const ProgressCallback& progress)
{
    Result result;
    const auto fail = [&result](const QString& code, const QString& cause) {
        result.status = Status::RecoverableFailure;
        result.code = code;
        result.cause = cause;
        return result;
    };
    if (progress) progress(Stage::PreparingRuntime);
    const QFileInfo root(m_context.rootPath);
    if (m_context.rootPath.trimmed().isEmpty() || root.isSymLink()
        || (root.exists() && !root.isDir()) || !QDir().mkpath(m_context.rootPath))
        return fail(QStringLiteral("runtime_root_unavailable"),
                    QStringLiteral("The runtime directory could not be prepared."));
    if (m_context.channel != QLatin1String("development")
        && m_context.channel != QLatin1String("production"))
        return fail(QStringLiteral("invalid_runtime_channel"), QStringLiteral("Unknown runtime channel."));

    // ApplicationInstanceManager also owns a lifetime profile lock. This
    // short-lived lock protects direct/bootstrap callers and retry operations.
    const QString lockPath = QDir(m_context.rootPath).filePath(QStringLiteral("bootstrap.lock"));
    if (QFileInfo(lockPath).isSymLink())
        return fail(QStringLiteral("runtime_lock_unsafe"), QStringLiteral("Unsafe bootstrap lock."));
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return fail(QStringLiteral("runtime_locked"), QStringLiteral("Cannot acquire the runtime bootstrap lock."));
    RuntimeProfile::configure(m_context);

    for (const RuntimeStorage::Component& component : RuntimeStorage::components(m_context)) {
        Stage stage = Stage::PreparingRuntime;
        if (component.id == QLatin1String("settings")) stage = Stage::ValidatingSettings;
        else if (component.id == QLatin1String("projects")) stage = Stage::ValidatingProjects;
        else if (component.id == QLatin1String("history")) stage = Stage::ValidatingHistory;
        else if (component.id == QLatin1String("identity")) stage = Stage::ValidatingIdentity;
        else if (component.id == QLatin1String("cache")) stage = Stage::PurgingCache;
        if (progress) progress(stage);
        RuntimeStorage::Report report = RuntimeStorage::upgrade(component);
        if (report.succeeded() && component.id == QLatin1String("cache")) {
            // Reset/initialization already emptied it. On compatible launches
            // this is session maintenance, never a schema reset.
            if (report.action == RuntimeStorage::Action::Preserved
                || report.action == RuntimeStorage::Action::Migrated) {
                const RuntimeStorage::Operation purged = RuntimeStorage::purgeReceivedMedia(m_context);
                report.failure = purged.failure;
                if (!purged.succeeded()) report.reason = purged.reason;
            }
            report.maintenancePerformed = report.succeeded();
        }
        result.components.append(report);
        qInfo().noquote() << "[Storage]" << m_context.channel << component.id
                         << report.foundVersion << "->" << report.expectedVersion
                         << RuntimeStorage::actionName(report.action) << report.reason;
        if (!report.succeeded())
            return fail(component.id + QStringLiteral("_storage_failed"), report.reason);
        if (report.action == RuntimeStorage::Action::Reset)
            result.resetCategories.append(component.id);
    }
    result.status = result.resetCategories.isEmpty() ? Status::Success : Status::SuccessWithReset;
    return result;
}

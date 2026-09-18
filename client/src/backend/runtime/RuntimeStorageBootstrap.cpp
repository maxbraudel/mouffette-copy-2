#include "RuntimeStorageBootstrap.h"
#include "InstallationIdentityBootstrap.h"
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

    // Identity belongs to the installation, not to the disposable profile.
    // Adopt the primary's historical key before any profile error can offer
    // Clear storage; a fresh secondary must never reset this shared resource.
    if (progress) progress(Stage::ValidatingIdentity);
    QString identityError;
    RuntimeStorage::Report identityReport;
    if (!InstallationIdentityBootstrap::prepare(m_context, &identityError, &identityReport))
        return fail(QStringLiteral("installation_identity_failed"), identityError);
    result.components.append(identityReport);

    for (const RuntimeStorage::Component& component : RuntimeStorage::components(m_context)) {
        Stage stage = Stage::PreparingRuntime;
        if (component.id == QLatin1String("settings")) stage = Stage::ValidatingSettings;
        else if (component.id == QLatin1String("projects")) stage = Stage::ValidatingProjects;
        else if (component.id == QLatin1String("history")) stage = Stage::ValidatingHistory;
        else if (component.id == QLatin1String("identity")) stage = Stage::ValidatingIdentity;
        else if (component.id == QLatin1String("cache")) stage = Stage::PurgingCache;
        if (progress) progress(stage);
        RuntimeStorage::Report report = RuntimeStorage::upgrade(component);
        // Compatible retained media survives a primary profile restart. The
        // cache store fences abandoned sessions and expires retained bytes.
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

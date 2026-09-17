#include "StorageUpgradeEngine.h"

#include <QSet>

namespace RuntimeStorage {
QString actionName(Action action)
{
    switch (action) {
    case Action::None: return QStringLiteral("none");
    case Action::Preserved: return QStringLiteral("preserved");
    case Action::Initialized: return QStringLiteral("initialized");
    case Action::Migrated: return QStringLiteral("migrated");
    case Action::Reset: return QStringLiteral("reset");
    }
    return {};
}

Report upgrade(const Component& component)
{
    const Inspection initial = component.inspect();
    Report report{component.id, initial.version, component.targetVersion,
                  Action::None, initial.reason};
    const auto failed = [&report](Failure failure, const QString& reason) {
        report.failure = failure;
        report.reason = reason;
        return report;
    };
    const auto verify = [&component]() {
        const Inspection stored = component.inspect();
        if (stored.state == State::Current && stored.version == component.targetVersion)
            return Operation{};
        return Operation{stored.state == State::IoError ? Failure::IoError
                                                       : Failure::InvalidData,
                         QStringLiteral("Post-write validation failed: %1").arg(stored.reason)};
    };
    const auto reset = [&](Action action) {
        const Operation operation = component.reset();
        if (!operation.succeeded()) return failed(operation.failure, operation.reason);
        const Operation checked = verify();
        if (!checked.succeeded()) return failed(checked.failure, checked.reason);
        report.action = action;
        return report;
    };
    if (initial.state == State::IoError) return failed(Failure::IoError, initial.reason);
    if (initial.state == State::Missing) return reset(Action::Initialized);
    if (initial.state == State::Current && initial.version == component.targetVersion) {
        report.action = Action::Preserved;
        return report;
    }
    if (initial.state == State::Corrupt) return reset(Action::Reset);

    // Resolve the entire path before making changes. A reset barrier discards
    // the old representation and creates the final version directly.
    QList<const Transition*> path;
    QSet<int> visited;
    int version = initial.version;
    while (version != component.targetVersion) {
        if (visited.contains(version))
            return failed(Failure::InvalidData, QStringLiteral("Cyclic migration registry"));
        visited.insert(version);
        const Transition* selected = nullptr;
        for (const Transition& transition : component.transitions) {
            if (transition.from != version) continue;
            if (selected)
                return failed(Failure::InvalidData, QStringLiteral("Ambiguous migration registry"));
            selected = &transition;
        }
        if (!selected || selected->kind == Transition::Kind::Reset) {
            report.reason = selected ? QStringLiteral("Explicit reset transition")
                                     : QStringLiteral("No supported migration path");
            return reset(Action::Reset);
        }
        if (!selected->apply || selected->to <= version
            || selected->to > component.targetVersion)
            return failed(Failure::InvalidData, QStringLiteral("Invalid migration registry"));
        path.append(selected);
        version = selected->to;
    }
    for (const Transition* transition : path) {
        const Operation operation = transition->apply();
        if (operation.failure == Failure::IoError) return failed(operation.failure, operation.reason);
        if (operation.failure == Failure::InvalidData) {
            report.reason = operation.reason;
            return reset(Action::Reset);
        }
        const Inspection committed = component.inspect();
        if (committed.state == State::IoError) return failed(Failure::IoError, committed.reason);
        if (committed.version != transition->to
            || (committed.state != State::Current && committed.state != State::Incompatible)) {
            report.reason = QStringLiteral("Migration produced invalid storage");
            return reset(Action::Reset);
        }
    }
    const Operation checked = verify();
    if (!checked.succeeded()) return failed(checked.failure, checked.reason);
    report.action = Action::Migrated;
    return report;
}
}

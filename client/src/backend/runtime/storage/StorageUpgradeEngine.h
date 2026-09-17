#pragma once

#include <QList>
#include <QString>
#include <functional>

namespace RuntimeStorage {
enum class State { Missing, Current, Incompatible, Corrupt, IoError };
enum class Failure { None, InvalidData, IoError };
enum class Action { None, Preserved, Initialized, Migrated, Reset };

struct Inspection {
    State state = State::IoError;
    int version = -1; // -1: absent/unreadable; 0: recognized unversioned storage.
    QString reason;
};

struct Operation {
    Failure failure = Failure::None;
    QString reason;
    bool succeeded() const { return failure == Failure::None; }
};

struct Transition {
    enum class Kind { Migrate, Reset };
    int from;
    int to;
    Kind kind;
    std::function<Operation()> apply;
};

struct Component {
    QString id;
    int targetVersion;
    std::function<Inspection()> inspect;
    // Initializes missing storage or replaces incompatible/corrupt storage.
    // Multi-resource implementations must be restartable (see README).
    std::function<Operation()> reset;
    QList<Transition> transitions;
};

struct Report {
    QString component;
    int foundVersion = -1;
    int expectedVersion = -1;
    Action action = Action::None;
    QString reason;
    Failure failure = Failure::None;
    bool maintenancePerformed = false;
    bool succeeded() const { return failure == Failure::None; }
};

// Inspects again after every committed operation. No global compatibility
// marker: a later failure cannot invalidate a component already upgraded.
Report upgrade(const Component& component);
QString actionName(Action action);
}

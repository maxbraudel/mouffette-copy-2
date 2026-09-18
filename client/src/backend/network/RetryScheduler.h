#pragma once

#include "backend/runtime/SuspendInclusiveClock.h"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <limits>

enum class RetryAction { Idle, Scheduled, Running, Suspended, Blocked };

// One coalesced, single-shot timer per owner. Keys identify operations, not
// deliveries. Cancellation/replacement fences callbacks already selected by a
// wake-up. Deadlines include sleep; waking never replays a backlog of ticks.
class RetryScheduler : public QObject {
public:
    using Clock = std::function<qint64()>;
    explicit RetryScheduler(QObject* parent = nullptr,
                            Clock clock = [] { return MouffetteClock::nowMs(); })
        : QObject(parent), m_clock(std::move(clock)) {
        m_timer.setSingleShot(true);
        m_timer.setTimerType(Qt::PreciseTimer);
        connect(&m_timer, &QTimer::timeout, this, [this] { processDue(); });
    }
    void schedule(const QString& key, qint64 delayMs, std::function<void()> callback) {
        m_tasks.insert(key, Task{m_clock() + std::max<qint64>(0, delayMs),
                                ++m_revision, std::move(callback)});
        arm();
    }
    void cancel(const QString& key) { m_tasks.remove(key); arm(); }
    void cancelAll() { m_tasks.clear(); m_timer.stop(); }
    bool contains(const QString& key) const { return m_tasks.contains(key); }
    qint64 dueAt(const QString& key) const {
        const auto it = m_tasks.constFind(key);
        return it == m_tasks.cend() ? -1 : it->due;
    }
    int size() const { return m_tasks.size(); }
    void processDue() {
        const qint64 now = m_clock();
        const auto snapshot = m_tasks;
        QPointer<RetryScheduler> alive(this);
        for (auto it = snapshot.cbegin(); it != snapshot.cend(); ++it) {
            const auto current = m_tasks.constFind(it.key());
            if (it->due > now || current == m_tasks.cend()
                || current->revision != it->revision) continue;
            auto callback = it->callback;
            m_tasks.remove(it.key());
            callback();
            if (!alive) return;
        }
        arm();
    }
private:
    struct Task { qint64 due; quint64 revision; std::function<void()> callback; };
    void arm() {
        if (m_tasks.isEmpty()) { m_timer.stop(); return; }
        qint64 due = std::numeric_limits<qint64>::max();
        for (const auto& task : std::as_const(m_tasks)) due = std::min(due, task.due);
        m_timer.start(static_cast<int>(std::clamp<qint64>(due - m_clock(), 0,
                                                        std::numeric_limits<int>::max())));
    }
    Clock m_clock;
    QTimer m_timer;
    QHash<QString, Task> m_tasks;
    quint64 m_revision = 0;
};

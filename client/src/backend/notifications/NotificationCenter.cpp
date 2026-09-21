#include "backend/notifications/NotificationCenter.h"

#include <QDateTime>
#include <QUuid>

namespace {
QString scopedTerminalCorrelation(const QString& scope, const QString& id)
{
    const QString normalized = id.trimmed();
    return normalized.isEmpty()
        ? QString()
        : scope + QLatin1Char(':') + normalized;
}
}

QString NotificationCorrelation::upload(const QString& uploadId)
{
    return scopedTerminalCorrelation(QStringLiteral("upload"), uploadId);
}

QString NotificationCorrelation::sceneRun(const QString& sceneRunId)
{
    return scopedTerminalCorrelation(QStringLiteral("scene-run"), sceneRunId);
}

QString NotificationCorrelation::teardown(const QString& teardownId)
{
    return scopedTerminalCorrelation(QStringLiteral("teardown"), teardownId);
}

NotificationCenter::NotificationCenter(QObject* parent)
    : QObject(parent)
    , m_ownedStore(std::make_unique<HistoryStore>())
    , m_store(m_ownedStore.get())
{
    load();
}

NotificationCenter::NotificationCenter(HistoryStore* store, QObject* parent)
    : QObject(parent)
    , m_store(store)
{
    load();
}

bool NotificationCenter::load()
{
    m_lastError.clear();
    m_history = {};
    m_terminalCorrelations.clear();
    if (!m_store) {
        m_lastError = QStringLiteral("Notification history store is not configured");
        emit persistenceError(m_lastError);
        return false;
    }
    if (!m_store->load(&m_history)) {
        m_lastError = m_store->lastError();
        emit persistenceError(m_lastError);
        return false;
    }
    rebuildTerminalIndex();
    emit historyChanged();
    emit unreadCountChanged(unreadCount());
    return true;
}

void NotificationCenter::rebuildTerminalIndex()
{
    m_terminalCorrelations.clear();
    for (const QString& id : m_history.terminalCorrelationIds) {
        m_terminalCorrelations.insert(id);
    }
}

int NotificationCenter::unreadCount() const
{
    int unread = 0;
    for (const NotificationEntry& entry : m_history.entries) {
        if (!entry.read) {
            ++unread;
        }
    }
    return unread;
}

QString NotificationCenter::publish(const NotificationRequest& request)
{
    const QString message = request.message.trimmed();
    if (message.isEmpty()) {
        return {};
    }
    const QString correlationId = request.correlationId.trimmed();
    if (request.terminal && correlationId.isEmpty()) {
        return {};
    }
    if (request.terminal && m_terminalCorrelations.contains(correlationId)) {
        return {}; // Idempotent terminal replay.
    }

    const int previousUnread = unreadCount();
    const NotificationHistoryData previousHistory = m_history;
    NotificationEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.timestampMs = request.timestampMs >= 0
        ? request.timestampMs
        : QDateTime::currentMSecsSinceEpoch();
    entry.severity = request.severity;
    entry.category = request.category.trimmed().isEmpty()
        ? QStringLiteral("General")
        : request.category.trimmed();
    entry.message = message;
    entry.read = m_historyVisible;
    entry.correlationId = correlationId;
    entry.projectId = request.projectId;
    entry.remoteSessionId = request.remoteSessionId;
    entry.sceneRunId = request.sceneRunId;
    entry.terminal = request.terminal;
    entry.peers = request.peers;
    if (!entry.isValid()) return {};

    m_history.entries.prepend(entry);
    while (m_history.entries.size() > HistoryStore::MaximumEntries) {
        m_history.entries.removeLast();
    }
    if (entry.terminal) {
        m_terminalCorrelations.insert(correlationId);
        m_history.terminalCorrelationIds.append(correlationId);
        while (m_history.terminalCorrelationIds.size() > HistoryStore::MaximumTerminalCorrelations) {
            const QString evicted = m_history.terminalCorrelationIds.takeFirst();
            m_terminalCorrelations.remove(evicted);
        }
    }

    // A toast is emitted only after its history record has been atomically
    // committed. If storage is unavailable, roll the in-memory mutation back
    // as well so a later notification cannot appear to have been recorded.
    if (!persist()) {
        m_history = previousHistory;
        rebuildTerminalIndex();
        return {};
    }
    emit entryAdded(entry);
    emit historyChanged();
    emitUnreadIfChanged(previousUnread);
    emit toastRequested(entry, request.toastDurationMs);
    return entry.id;
}

bool NotificationCenter::persist()
{
    if (!m_store || !m_store->save(m_history)) {
        m_lastError = m_store ? m_store->lastError()
                              : QStringLiteral("Notification history store is not configured");
        emit persistenceError(m_lastError);
        return false;
    }
    m_lastError.clear();
    return true;
}

void NotificationCenter::emitUnreadIfChanged(int previousUnread)
{
    const int current = unreadCount();
    if (current != previousUnread) {
        emit unreadCountChanged(current);
    }
}

bool NotificationCenter::setHistoryVisible(bool visible)
{
    if (m_historyVisible == visible) {
        return true;
    }
    m_historyVisible = visible;
    return visible ? markAllRead() : true;
}

bool NotificationCenter::markAllRead()
{
    const int previousUnread = unreadCount();
    if (previousUnread == 0) {
        return true;
    }
    const NotificationHistoryData previousHistory = m_history;
    for (NotificationEntry& entry : m_history.entries) {
        entry.read = true;
    }
    if (!persist()) {
        m_history = previousHistory;
        return false;
    }
    emit historyChanged();
    emitUnreadIfChanged(previousUnread);
    return true;
}

bool NotificationCenter::clearHistory()
{
    const int previousUnread = unreadCount();
    const NotificationHistoryData previousHistory = m_history;
    // Clear the user-visible records, not the idempotency tombstones. A late
    // duplicate terminal packet must not recreate a toast immediately after
    // the user cleared the page.
    m_history.entries.clear();
    if (!persist()) {
        m_history = previousHistory;
        rebuildTerminalIndex();
        return false;
    }
    emit historyCleared();
    emit historyChanged();
    emitUnreadIfChanged(previousUnread);
    return true;
}

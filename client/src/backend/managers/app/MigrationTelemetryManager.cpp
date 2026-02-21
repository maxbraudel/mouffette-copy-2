#include "backend/managers/app/MigrationTelemetryManager.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QDebug>

namespace {
void logTelemetry(const QJsonObject& payload) {
    qInfo().noquote() << "[MIGRATION_TELEMETRY]"
                      << QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
}
}

void MigrationTelemetryManager::logStartupFlag(bool useQuickCanvasRenderer, const QString& flagSource) {
    QJsonObject payload;
    payload["event"] = "startup_flag_state";
    payload["useQuickCanvasRenderer"] = useQuickCanvasRenderer;
    payload["flagSource"] = flagSource;
    logTelemetry(payload);
}

void MigrationTelemetryManager::logRendererPathResolved(const QString& location,
                                                        bool requestedQuickRenderer,
                                                        const QString& appliedRenderer,
                                                        const QString& reason) {
    QJsonObject payload;
    payload["event"] = "renderer_path_resolved";
    payload["location"] = location;
    payload["requestedQuickRenderer"] = requestedQuickRenderer;
    payload["appliedRenderer"] = appliedRenderer;
    payload["reason"] = reason;
    logTelemetry(payload);
}

void MigrationTelemetryManager::logCanvasLoadRequest(const QString& persistentClientId) {
    QJsonObject payload;
    payload["event"] = "canvas_load_request";
    payload["persistentClientId"] = persistentClientId;
    logTelemetry(payload);
}

void MigrationTelemetryManager::logCanvasLoadReady(const QString& persistentClientId,
                                                   int screenCount,
                                                   qint64 latencyMs) {
    QJsonObject payload;
    payload["event"] = "canvas_load_ready";
    payload["persistentClientId"] = persistentClientId;
    payload["screenCount"] = screenCount;
    payload["latencyMs"] = static_cast<qint64>(latencyMs);
    logTelemetry(payload);
}

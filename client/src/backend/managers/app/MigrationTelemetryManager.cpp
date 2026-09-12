#include "backend/managers/app/MigrationTelemetryManager.h"
#include "backend/config/AppConfig.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QDebug>

namespace {
bool migrationTelemetryEnabled() {
    return AppConfig::instance().migrationTelemetry();
}

void logTelemetry(const QJsonObject& payload) {
    if (!migrationTelemetryEnabled()) {
        return;
    }
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

void MigrationTelemetryManager::logCanvasLoadRequest(const QString& endpointId) {
    QJsonObject payload;
    payload["event"] = "canvas_load_request";
    payload["endpointId"] = endpointId;
    logTelemetry(payload);
}

void MigrationTelemetryManager::logCanvasLoadReady(const QString& endpointId,
                                                   int screenCount,
                                                   qint64 latencyMs) {
    QJsonObject payload;
    payload["event"] = "canvas_load_ready";
    payload["endpointId"] = endpointId;
    payload["screenCount"] = screenCount;
    payload["latencyMs"] = static_cast<qint64>(latencyMs);
    logTelemetry(payload);
}

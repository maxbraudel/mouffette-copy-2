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

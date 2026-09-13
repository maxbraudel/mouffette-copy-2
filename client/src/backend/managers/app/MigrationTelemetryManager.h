#ifndef MIGRATIONTELEMETRYMANAGER_H
#define MIGRATIONTELEMETRYMANAGER_H

#include <QString>

class MigrationTelemetryManager {
public:
    static void logCanvasLoadRequest(const QString& endpointId);
    static void logCanvasLoadReady(const QString& endpointId,
                                   int screenCount,
                                   qint64 latencyMs);
};

#endif // MIGRATIONTELEMETRYMANAGER_H

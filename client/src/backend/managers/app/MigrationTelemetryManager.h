#ifndef MIGRATIONTELEMETRYMANAGER_H
#define MIGRATIONTELEMETRYMANAGER_H

#include <QString>

class MigrationTelemetryManager {
public:
    static void logStartupFlag(bool useQuickCanvasRenderer, const QString& flagSource);
    static void logRendererPathResolved(const QString& location,
                                        bool requestedQuickRenderer,
                                        const QString& appliedRenderer,
                                        const QString& reason);
    static void logCanvasLoadRequest(const QString& persistentClientId);
    static void logCanvasLoadReady(const QString& persistentClientId,
                                   int screenCount,
                                   qint64 latencyMs);
};

#endif // MIGRATIONTELEMETRYMANAGER_H

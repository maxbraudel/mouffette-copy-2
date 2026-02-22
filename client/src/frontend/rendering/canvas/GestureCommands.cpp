#include "frontend/rendering/canvas/GestureCommands.h"

#include <QMetaObject>
#include <QVariant>

bool GestureCommands::invokeMediaTransformCommand(QObject* quickRootObject,
                                                  const char* commandName,
                                                  const QString& mediaId,
                                                  qreal sceneX,
                                                  qreal sceneY,
                                                  qreal scale,
                                                  qreal sceneUnitScale) {
    if (!quickRootObject || !commandName || mediaId.isEmpty()) {
        return false;
    }

    const qreal normalizedSceneScale = (sceneUnitScale > 1e-6) ? sceneUnitScale : 1.0;
    return QMetaObject::invokeMethod(
        quickRootObject,
        commandName,
        Q_ARG(QVariant, mediaId),
        Q_ARG(QVariant, sceneX * normalizedSceneScale),
        Q_ARG(QVariant, sceneY * normalizedSceneScale),
        Q_ARG(QVariant, scale));
}

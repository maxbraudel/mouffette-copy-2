#ifndef GESTURECOMMANDS_H
#define GESTURECOMMANDS_H

#include <QString>

class QObject;

class GestureCommands {
public:
    // Invokes a QML transform command (e.g. applyLiveResizeGeometry).
    // Returns true if the method was found and called successfully.
    static bool invokeMediaTransformCommand(QObject* quickRootObject,
                                            const char* commandName,
                                            const QString& mediaId,
                                            qreal sceneX,
                                            qreal sceneY,
                                            qreal scale,
                                            qreal sceneUnitScale);
};

#endif // GESTURECOMMANDS_H

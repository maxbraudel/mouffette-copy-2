#ifndef SNAPGUIDEPUBLISHER_H
#define SNAPGUIDEPUBLISHER_H

#include <QLineF>
#include <QVariantList>
#include <QVector>

class ScreenCanvas;

// Converts raw scene-space snap guide lines into a ready-to-publish QVariantList
// (snapGuidesModel format consumed by SnapGuides.qml).
// All methods are pure static — no state, no Qt object overhead.
class SnapGuidePublisher {
public:
    // Read current snap guide lines from a ScreenCanvas, scale by sceneUnitScale,
    // and return a ready-to-publish QVariantList for snapGuidesModel.
    static QVariantList buildFromScreenCanvas(ScreenCanvas* screenCanvas, qreal sceneUnitScale);

    // Build a snapGuidesModel from an explicit list of scene-space lines.
    // sceneUnitScale converts from backend scene coords to QML canvas coords.
    static QVariantList buildFromLines(const QVector<QLineF>& lines, qreal sceneUnitScale);

    // Returns an empty model — use to clear snap guides.
    static QVariantList emptyModel();
};

#endif // SNAPGUIDEPUBLISHER_H

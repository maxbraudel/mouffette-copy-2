#include "frontend/rendering/canvas/SnapGuidePublisher.h"

#include "frontend/rendering/canvas/ScreenCanvas.h"

QVariantList SnapGuidePublisher::buildFromScreenCanvas(ScreenCanvas* screenCanvas, qreal sceneUnitScale) {
    if (!screenCanvas) {
        return emptyModel();
    }
    return buildFromLines(screenCanvas->currentSnapGuideLines(), sceneUnitScale);
}

QVariantList SnapGuidePublisher::buildFromLines(const QVector<QLineF>& lines, qreal sceneUnitScale) {
    if (lines.isEmpty()) {
        return emptyModel();
    }
    const qreal scale = (sceneUnitScale > 1e-9) ? sceneUnitScale : 1.0;
    QVariantList model;
    model.reserve(lines.size());
    for (const QLineF& line : lines) {
        QVariantMap entry;
        entry.insert(QStringLiteral("x1"), line.x1() * scale);
        entry.insert(QStringLiteral("y1"), line.y1() * scale);
        entry.insert(QStringLiteral("x2"), line.x2() * scale);
        entry.insert(QStringLiteral("y2"), line.y2() * scale);
        model.append(entry);
    }
    return model;
}

QVariantList SnapGuidePublisher::emptyModel() {
    return QVariantList{};
}

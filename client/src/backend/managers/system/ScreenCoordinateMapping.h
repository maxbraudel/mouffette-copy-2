#ifndef SCREENCOORDINATEMAPPING_H
#define SCREENCOORDINATEMAPPING_H

#include <QPointF>
#include <QRect>
#include <cmath>

namespace ScreenCoordinateMapping {

inline QRect scaledScreenGeometry(const QRect& logicalGeometry, qreal scale)
{
    // QRect::setX/setY preserve the opposite edge and change the size. Scale
    // immutable components instead, including screens above/left of primary.
    return QRect(static_cast<int>(std::lround(logicalGeometry.x() * scale)),
                 static_cast<int>(std::lround(logicalGeometry.y() * scale)),
                 static_cast<int>(std::lround(logicalGeometry.width() * scale)),
                 static_cast<int>(std::lround(logicalGeometry.height() * scale)));
}

inline QPointF screenLocalPosition(const QPoint& logicalPosition,
                                  const QRect& logicalGeometry, qreal scale)
{
    // Scaling the desktop origin would mix coordinate systems on mixed-DPI
    // desktops. The screen id travels separately from this local position.
    return QPointF(logicalPosition - logicalGeometry.topLeft()) * scale;
}

} // namespace ScreenCoordinateMapping

#endif // SCREENCOORDINATEMAPPING_H

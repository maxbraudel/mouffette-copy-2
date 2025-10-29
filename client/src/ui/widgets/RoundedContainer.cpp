#include "RoundedContainer.h"
#include "AppColors.h"
#include "ui/StyleConfig.h"
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <algorithm>

RoundedContainer::RoundedContainer(QWidget* parent)
    : QWidget(parent)
{
}

void RoundedContainer::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal radius = qMax(0, gWindowBorderRadiusPx);
    QRectF r = rect();
    
    // Align to half-pixels for crisp 1px strokes
    QRectF borderRect = r.adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(borderRect, radius, radius);

    // Use dynamic palette color that updates automatically with theme changes
    QColor fill = AppColors::getCurrentColor(AppColors::gWindowBackgroundColorSource);
    
    // Avoid halo on rounded edges by drawing fill with Source composition
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillPath(path, fill);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    
    // No outer border drawing — only rounded fill is rendered
}

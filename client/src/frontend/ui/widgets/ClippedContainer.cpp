#include "frontend/ui/widgets/ClippedContainer.h"
#include "frontend/ui/theme/StyleConfig.h"
#include <QShowEvent>
#include <QResizeEvent>
#include <QRegion>
#include <QRect>
#include <QPainterPath>
#include <QPainter>
#include <QBitmap>
#include <QImage>
#include <QtGlobal>
#include <algorithm>

ClippedContainer::ClippedContainer(QWidget* parent)
    : QWidget(parent),
      m_lastMaskSize()
{
    // Ensure style sheets (including borders) are painted for this widget
    setAttribute(Qt::WA_StyledBackground, true);
}

void ClippedContainer::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    updateMaskIfNeeded();
}

void ClippedContainer::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateMaskIfNeeded();
}

void ClippedContainer::updateMaskIfNeeded()
{
    const QSize currentSize = size();
    
    // Skip if size hasn't changed (common during theme switches, etc.)
    if (currentSize == m_lastMaskSize && !mask().isEmpty()) {
        return;
    }
    
    // Ensure we have a valid size
    if (currentSize.width() <= 0 || currentSize.height() <= 0) {
        return;
    }
    
    // Cache the size
    m_lastMaskSize = currentSize;
    
    // Use more efficient QRegion constructor for rounded rectangles
    const int radius = qMax(0, qMin(gDynamicBoxBorderRadius, 
                                     qMin(currentSize.width(), currentSize.height()) / 2));
    const QRectF rect(0.0, 0.0, currentSize.width(), currentSize.height());

    if (radius <= 0) {
        setMask(QRegion(rect.toRect()));
        return;
    }

    const qreal inset = 0.5; // keep border pixels inside mask for crisper corners
    const qreal adjustedRadius = std::max<qreal>(0.0, radius - inset);
    QPainterPath path;
    path.addRoundedRect(rect.adjusted(inset, inset, -inset, -inset), adjustedRadius, adjustedRadius);

    const qreal dpr = devicePixelRatioF();
    const QSize scaledSize(
        qMax(1, qRound(currentSize.width() * dpr)),
        qMax(1, qRound(currentSize.height() * dpr))
    );

    QImage maskImage(scaledSize, QImage::Format_ARGB32_Premultiplied);
    maskImage.fill(Qt::black);
    maskImage.setDevicePixelRatio(dpr);

    {
        QPainter painter(&maskImage);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.scale(dpr, dpr);
        painter.fillPath(path, Qt::white);
    }

    QBitmap mask = QBitmap::fromImage(maskImage.createMaskFromColor(Qt::black, Qt::MaskInColor));
    mask.setDevicePixelRatio(dpr);
    setMask(mask);
}

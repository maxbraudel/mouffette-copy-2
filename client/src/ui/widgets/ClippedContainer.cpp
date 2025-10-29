#include "ClippedContainer.h"
#include "ui/StyleConfig.h"
#include <QShowEvent>
#include <QResizeEvent>
#include <QRegion>
#include <QRect>
#include <algorithm>

ClippedContainer::ClippedContainer(QWidget* parent)
    : QWidget(parent),
      m_lastMaskSize()
{
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
    const QRect r(0, 0, currentSize.width(), currentSize.height());
    
    // Create rounded region more efficiently using ellipse corners
    QRegion region(r);
    
    if (radius > 0) {
        // Subtract corner rectangles and add back rounded corners
        const int d = radius * 2;
        
        // Subtract corner rectangles
        region -= QRegion(0, 0, radius, radius);                                           // top-left corner
        region -= QRegion(r.width() - radius, 0, radius, radius);                         // top-right corner  
        region -= QRegion(0, r.height() - radius, radius, radius);                        // bottom-left corner
        region -= QRegion(r.width() - radius, r.height() - radius, radius, radius);       // bottom-right corner
        
        // Add rounded corners
        region += QRegion(0, 0, d, d, QRegion::Ellipse);                                  // top-left rounded
        region += QRegion(r.width() - d, 0, d, d, QRegion::Ellipse);                      // top-right rounded
        region += QRegion(0, r.height() - d, d, d, QRegion::Ellipse);                     // bottom-left rounded  
        region += QRegion(r.width() - d, r.height() - d, d, d, QRegion::Ellipse);         // bottom-right rounded
    }
    
    setMask(region);
}

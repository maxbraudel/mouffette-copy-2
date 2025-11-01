#ifndef SEGMENTEDBUTTONITEM_H
#define SEGMENTEDBUTTONITEM_H

#include <QGraphicsPathItem>
#include <QPainterPath>
#include <QRectF>
#include <QPainter>
#include <algorithm>

/**
 * A graphics item for creating segmented/fused button groups with individual corner control.
 * Supports left, middle, and right segments with appropriate border and corner radius handling.
 */
class SegmentedButtonItem : public QGraphicsPathItem {
public:
    enum class Segment {
        Left,    // Rounded left corners, no right border
        Middle,  // No rounded corners, no right border
        Right,   // Rounded right corners, has left border
        Single   // Standalone button with all corners rounded
    };
    
    explicit SegmentedButtonItem(Segment segment, QGraphicsItem* parent = nullptr)
        : QGraphicsPathItem(parent), m_segment(segment), m_radius(0.0) {}
    
    void setRect(const QRectF& r) { 
        m_rect = r; 
        updatePath(); 
    }
    
    void setRect(qreal x, qreal y, qreal w, qreal h) { 
        setRect(QRectF(x, y, w, h)); 
    }
    
    QRectF rect() const { 
        return m_rect; 
    }
    
    void setRadius(qreal radiusPx) { 
        m_radius = std::max<qreal>(0.0, radiusPx); 
        updatePath(); 
    }
    
    qreal radius() const { 
        return m_radius; 
    }
    
    Segment segment() const {
        return m_segment;
    }

private:
    void updatePath() {
        QPainterPath p;
        if (m_rect.isNull()) { 
            setPath(p); 
            return; 
        }
        
        // Clamp radius so it never exceeds half of width/height
        const qreal r = std::min({ m_radius, m_rect.width() * 0.5, m_rect.height() * 0.5 });
        
        if (m_segment == Segment::Single) {
            p.addRoundedRect(m_rect, r, r);
        } else if (r > 0.0 && (m_segment == Segment::Left || m_segment == Segment::Right)) {
            // Create path with selective corner rounding
            const qreal x = m_rect.x();
            const qreal y = m_rect.y();
            const qreal w = m_rect.width();
            const qreal h = m_rect.height();
            
            p.moveTo(x + (m_segment == Segment::Left ? r : 0), y);
            
            // Top edge
            p.lineTo(x + w - (m_segment == Segment::Right ? r : 0), y);
            
            // Top-right corner
            if (m_segment == Segment::Right) {
                p.arcTo(QRectF(x + w - 2*r, y, 2*r, 2*r), 90, -90);
            }
            
            // Right edge
            p.lineTo(x + w, y + h - (m_segment == Segment::Right ? r : 0));
            
            // Bottom-right corner
            if (m_segment == Segment::Right) {
                p.arcTo(QRectF(x + w - 2*r, y + h - 2*r, 2*r, 2*r), 0, -90);
            }
            
            // Bottom edge
            p.lineTo(x + (m_segment == Segment::Left ? r : 0), y + h);
            
            // Bottom-left corner
            if (m_segment == Segment::Left) {
                p.arcTo(QRectF(x, y + h - 2*r, 2*r, 2*r), 270, -90);
            }
            
            // Left edge
            p.lineTo(x, y + (m_segment == Segment::Left ? r : 0));
            
            // Top-left corner
            if (m_segment == Segment::Left) {
                p.arcTo(QRectF(x, y, 2*r, 2*r), 180, -90);
            }
            
            p.closeSubpath();
        } else {
            // Middle segment or no radius: just a rectangle
            p.addRect(m_rect);
        }
        
        setPath(p);
    }
    
    QRectF m_rect;
    qreal  m_radius;
    Segment m_segment;
};

#endif // SEGMENTEDBUTTONITEM_H

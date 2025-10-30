#ifndef ROUNDEDRECTITEM_H
#define ROUNDEDRECTITEM_H

#include <QGraphicsPathItem>
#include <QPainterPath>
#include <QRectF>
#include <algorithm>

/**
 * Simple rounded-rectangle graphics item with a settable rect and radius
 */
class RoundedRectItem : public QGraphicsPathItem {
public:
    explicit RoundedRectItem(QGraphicsItem* parent = nullptr)
        : QGraphicsPathItem(parent), m_radius(0.0) {}
    
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

private:
    void updatePath() {
        QPainterPath p;
        if (m_rect.isNull()) { 
            setPath(p); 
            return; 
        }
        
        // Clamp radius so it never exceeds half of width/height
        const qreal r = std::min({ m_radius, m_rect.width() * 0.5, m_rect.height() * 0.5 });
        if (r > 0.0)
            p.addRoundedRect(m_rect, r, r);
        else
            p.addRect(m_rect);
        setPath(p);
    }
    
    QRectF m_rect;
    qreal  m_radius;
};

#endif // ROUNDEDRECTITEM_H

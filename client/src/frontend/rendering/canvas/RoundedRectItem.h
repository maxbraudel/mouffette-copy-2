#ifndef ROUNDEDRECTITEM_H
#define ROUNDEDRECTITEM_H

#include <QGraphicsPathItem>
#include <QPainterPath>
#include <QRectF>
#include <algorithm>
#include <array>
#include <cmath>

/**
 * Simple rounded-rectangle graphics item with a settable rect and radius
 */
class RoundedRectItem : public QGraphicsPathItem {
public:
    enum Corner { TopLeft = 0, TopRight = 1, BottomRight = 2, BottomLeft = 3 };

    explicit RoundedRectItem(QGraphicsItem* parent = nullptr)
        : QGraphicsPathItem(parent) {
        m_cornerRadii.fill(0.0);
    }
    
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
        m_cornerRadii.fill(m_radius);
        updatePath(); 
    }
    
    qreal radius() const { 
        return m_radius; 
    }

    void setCornerRadius(Corner corner, qreal radiusPx) {
        m_cornerRadii[static_cast<int>(corner)] = std::max<qreal>(0.0, radiusPx);
        if (std::all_of(m_cornerRadii.cbegin(), m_cornerRadii.cend(), [this](qreal r){
                return std::abs(r - m_cornerRadii.front()) < 1e-6;
            })) {
            m_radius = m_cornerRadii.front();
        } else {
            m_radius = -1.0;
        }
        updatePath();
    }

    void setCornerRadii(qreal topLeft, qreal topRight, qreal bottomRight, qreal bottomLeft) {
        m_cornerRadii[TopLeft] = std::max<qreal>(0.0, topLeft);
        m_cornerRadii[TopRight] = std::max<qreal>(0.0, topRight);
        m_cornerRadii[BottomRight] = std::max<qreal>(0.0, bottomRight);
        m_cornerRadii[BottomLeft] = std::max<qreal>(0.0, bottomLeft);
        if (std::abs(m_cornerRadii[TopLeft] - m_cornerRadii[TopRight]) < 1e-6 &&
            std::abs(m_cornerRadii[TopLeft] - m_cornerRadii[BottomRight]) < 1e-6 &&
            std::abs(m_cornerRadii[TopLeft] - m_cornerRadii[BottomLeft]) < 1e-6) {
            m_radius = m_cornerRadii[TopLeft];
        } else {
            m_radius = -1.0;
        }
        updatePath();
    }

    qreal cornerRadius(Corner corner) const {
        return m_cornerRadii[static_cast<int>(corner)];
    }

private:
    void updatePath() {
        QPainterPath p;
        if (m_rect.isNull()) { 
            setPath(p); 
            return; 
        }
        
        // If all corners share the same radius (tracked via m_radius >= 0), fall back to Qt's
        // native rounded-rect path for efficiency.
        if (m_radius >= 0.0) {
            const qreal r = std::min({ m_radius, m_rect.width() * 0.5, m_rect.height() * 0.5 });
            if (r > 0.0)
                p.addRoundedRect(m_rect, r, r);
            else
                p.addRect(m_rect);
            setPath(p);
            return;
        }

        auto clamped = m_cornerRadii;
        auto clampPair = [](qreal& a, qreal& b, qreal maxSpan) {
            const qreal sum = a + b;
            if (sum > maxSpan && sum > 0.0) {
                const qreal factor = maxSpan / sum;
                a *= factor;
                b *= factor;
            }
        };

        const qreal width = m_rect.width();
        const qreal height = m_rect.height();
        const qreal halfW = width * 0.5;
        const qreal halfH = height * 0.5;
        for (qreal& v : clamped) {
            v = std::max<qreal>(0.0, v);
            v = std::min(v, std::min(halfW, halfH));
        }

        clampPair(clamped[TopLeft], clamped[TopRight], width);
        clampPair(clamped[BottomLeft], clamped[BottomRight], width);
        clampPair(clamped[TopLeft], clamped[BottomLeft], height);
        clampPair(clamped[TopRight], clamped[BottomRight], height);

        const qreal left = m_rect.left();
        const qreal right = m_rect.right();
        const qreal top = m_rect.top();
        const qreal bottom = m_rect.bottom();

        p.moveTo(left + clamped[TopLeft], top);
        p.lineTo(right - clamped[TopRight], top);
        if (clamped[TopRight] > 0.0) {
            p.quadTo(right, top, right, top + clamped[TopRight]);
        } else {
            p.lineTo(right, top);
        }

        p.lineTo(right, bottom - clamped[BottomRight]);
        if (clamped[BottomRight] > 0.0) {
            p.quadTo(right, bottom, right - clamped[BottomRight], bottom);
        } else {
            p.lineTo(right, bottom);
        }

        p.lineTo(left + clamped[BottomLeft], bottom);
        if (clamped[BottomLeft] > 0.0) {
            p.quadTo(left, bottom, left, bottom - clamped[BottomLeft]);
        } else {
            p.lineTo(left, bottom);
        }

        p.lineTo(left, top + clamped[TopLeft]);
        if (clamped[TopLeft] > 0.0) {
            p.quadTo(left, top, left + clamped[TopLeft], top);
        } else {
            p.lineTo(left, top);
        }

        p.closeSubpath();
        setPath(p);
    }
    
    QRectF m_rect;
    qreal  m_radius = 0.0;
    std::array<qreal, 4> m_cornerRadii;
};

#endif // ROUNDEDRECTITEM_H

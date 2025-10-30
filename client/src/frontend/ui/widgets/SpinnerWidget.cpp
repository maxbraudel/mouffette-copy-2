#include "frontend/ui/widgets/SpinnerWidget.h"
#include <QTimer>
#include <QPainter>
#include <QStyleOption>
#include <QPen>

SpinnerWidget::SpinnerWidget(QWidget* parent)
    : QWidget(parent)
    , m_timer(new QTimer(this))
    , m_angle(0)
    , m_radiusPx(24)
    , m_lineWidthPx(6)
    , m_color("#4a90e2")
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        m_angle = (m_angle + 6) % 360;
        update();
    });
    m_timer->setInterval(16); // ~60 FPS
}

void SpinnerWidget::start() { if (!m_timer->isActive()) m_timer->start(); }
void SpinnerWidget::stop() { if (m_timer->isActive()) m_timer->stop(); }
bool SpinnerWidget::isSpinning() const { return m_timer && m_timer->isActive(); }

void SpinnerWidget::setRadius(int radiusPx) {
    m_radiusPx = qMax(8, radiusPx);
    updateGeometry();
    update();
}
void SpinnerWidget::setLineWidth(int px) {
    m_lineWidthPx = qMax(1, px);
    update();
}
void SpinnerWidget::setColor(const QColor& c) {
    m_color = c;
    update();
}

void SpinnerWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Palette-aware background
    QStyleOption opt; opt.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);

    const int s = qMin(width(), height());
    int outer = 2 * m_radiusPx;        // desired diameter
    const int maxOuter = qMax(16, s - 12);
    outer = qMin(outer, maxOuter);
    const int thickness = qMin(m_lineWidthPx, qMax(1, outer / 2));

    QPointF center(width() / 2.0, height() / 2.0);
    QRectF rect(center.x() - outer/2.0, center.y() - outer/2.0, outer, outer);

    QColor arc = m_color;
    arc.setAlpha(230);

    p.setPen(QPen(arc, thickness, Qt::SolidLine, Qt::FlatCap));
    int span = 16 * 300; // 300-degree arc
    p.save();
    p.translate(center);
    p.rotate(m_angle);
    p.translate(-center);
    p.drawArc(rect, 0, span);
    p.restore();
}

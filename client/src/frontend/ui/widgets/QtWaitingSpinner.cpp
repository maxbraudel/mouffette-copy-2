#include "frontend/ui/widgets/QtWaitingSpinner.h"
#include <QPainter>
#include <cmath>

QtWaitingSpinner::QtWaitingSpinner(QWidget *parent)
    : QWidget(parent),
      m_timer(new QTimer(this)),
      m_isSpinning(false),
      m_numberOfLines(12),
      m_lineLength(10),
      m_lineWidth(2),
      m_innerRadius(10),
      m_roundness(70.0),
      m_minimumTrailOpacity(15.0),
      m_trailFadePercentage(70.0),
      m_revolutionsPerSecond(1.0),
      m_color(Qt::black),
      m_currentCounter(0)
{
    connect(m_timer, &QTimer::timeout, this, &QtWaitingSpinner::rotate);
    updateSize();
}

void QtWaitingSpinner::updateSize()
{
    int size = (m_innerRadius + m_lineLength) * 2;
    setFixedSize(size, size);
}

void QtWaitingSpinner::start()
{
    if (m_isSpinning) return;
    m_isSpinning = true;
    m_currentCounter = 0;
    show();
    
    if (m_timer->isActive()) {
        m_timer->stop();
    }
    m_timer->start(1000 / (m_numberOfLines * m_revolutionsPerSecond));
}

void QtWaitingSpinner::stop()
{
    if (!m_isSpinning) return;
    m_isSpinning = false;
    m_timer->stop();
    hide();
}

void QtWaitingSpinner::setRoundness(qreal roundness)
{
    m_roundness = qMax(0.0, qMin(100.0, roundness));
}

void QtWaitingSpinner::setMinimumTrailOpacity(qreal minimumTrailOpacity)
{
    m_minimumTrailOpacity = minimumTrailOpacity;
}

void QtWaitingSpinner::setTrailFadePercentage(qreal trail)
{
    m_trailFadePercentage = trail;
}

void QtWaitingSpinner::setRevolutionsPerSecond(qreal revolutionsPerSecond)
{
    m_revolutionsPerSecond = revolutionsPerSecond;
    if (m_isSpinning) {
        m_timer->setInterval(1000 / (m_numberOfLines * m_revolutionsPerSecond));
    }
}

void QtWaitingSpinner::setNumberOfLines(int lines)
{
    m_numberOfLines = lines;
    m_currentCounter = 0;
    updateSize();
}

void QtWaitingSpinner::setLineLength(int length)
{
    m_lineLength = length;
    updateSize();
}

void QtWaitingSpinner::setLineWidth(int width)
{
    m_lineWidth = width;
    updateSize();
}

void QtWaitingSpinner::setInnerRadius(int radius)
{
    m_innerRadius = radius;
    updateSize();
}

void QtWaitingSpinner::setColor(QColor color)
{
    m_color = color;
}

bool QtWaitingSpinner::isSpinning() const
{
    return m_isSpinning;
}

void QtWaitingSpinner::rotate()
{
    ++m_currentCounter;
    if (m_currentCounter >= m_numberOfLines) {
        m_currentCounter = 0;
    }
    update();
}

void QtWaitingSpinner::paintEvent(QPaintEvent *)
{
    if (!m_isSpinning) return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), Qt::transparent);
    
    int outerRadius = (width() / 2);
    painter.translate(outerRadius, outerRadius);

    for (int i = 0; i < m_numberOfLines; ++i) {
        painter.save();
        painter.rotate(360.0 * qreal(i) / qreal(m_numberOfLines));
        
        QColor color = currentLineColor(lineCountDistanceFromPrimary(i, m_currentCounter, m_numberOfLines),
                                         m_numberOfLines, m_trailFadePercentage,
                                         m_minimumTrailOpacity, m_color);
        
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        
        QRectF rect(m_innerRadius, -m_lineWidth / 2.0, m_lineLength, m_lineWidth);
        qreal radius = (m_lineWidth / 2.0) * m_roundness / 100.0;
        painter.drawRoundedRect(rect, radius, radius, Qt::AbsoluteSize);
        
        painter.restore();
    }
}

int QtWaitingSpinner::lineCountDistanceFromPrimary(int current, int primary, int totalNrOfLines) const
{
    int distance = primary - current;
    if (distance < 0) {
        distance += totalNrOfLines;
    }
    return distance;
}

QColor QtWaitingSpinner::currentLineColor(int distance, int totalNrOfLines, qreal trailFadePerc,
                                           qreal minOpacity, QColor color) const
{
    if (distance == 0) {
        return color;
    }
    
    const qreal minAlphaF = minOpacity / 100.0;
    int distanceThreshold = static_cast<int>(ceil((totalNrOfLines - 1) * trailFadePerc / 100.0));
    
    if (distance > distanceThreshold) {
        color.setAlphaF(minAlphaF);
        return color;
    }
    
    qreal alphaDiff = color.alphaF() - minAlphaF;
    qreal gradient = alphaDiff / static_cast<qreal>(distanceThreshold + 1);
    qreal resultAlpha = color.alphaF() - gradient * distance;
    
    resultAlpha = qMax(qMin(resultAlpha, 1.0), 0.0);
    color.setAlphaF(resultAlpha);
    return color;
}
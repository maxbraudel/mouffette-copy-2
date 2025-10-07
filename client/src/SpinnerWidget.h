#ifndef SPINNERWIDGET_H
#define SPINNERWIDGET_H

#include <QWidget>
#include <QColor>

class QTimer;

// Lightweight customizable spinner used during screen loading operations.
class SpinnerWidget : public QWidget {
public:
    explicit SpinnerWidget(QWidget* parent = nullptr);

    void start();
    void stop();
    bool isSpinning() const;

    void setRadius(int radiusPx);
    void setLineWidth(int px);
    void setColor(const QColor& c);

    int radius() const { return m_radiusPx; }
    int lineWidth() const { return m_lineWidthPx; }
    QColor color() const { return m_color; }

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize minimumSizeHint() const override { return QSize(18, 18); }

private:
    QTimer* m_timer;
    int m_angle;
    int m_radiusPx;
    int m_lineWidthPx;
    QColor m_color;
};

#endif // SPINNERWIDGET_H

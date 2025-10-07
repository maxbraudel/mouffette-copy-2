#ifndef QTWAITINGSPINNER_H
#define QTWAITINGSPINNER_H

#include <QWidget>
#include <QTimer>
#include <QColor>

class QtWaitingSpinner : public QWidget {
    Q_OBJECT
public:
    explicit QtWaitingSpinner(QWidget *parent = nullptr);

public slots:
    void start();
    void stop();

public:
    void setRoundness(qreal roundness);
    void setMinimumTrailOpacity(qreal minimumTrailOpacity);
    void setTrailFadePercentage(qreal trail);
    void setRevolutionsPerSecond(qreal revolutionsPerSecond);
    void setNumberOfLines(int lines);
    void setLineLength(int length);
    void setLineWidth(int width);
    void setInnerRadius(int radius);
    void setColor(QColor color);

    bool isSpinning() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void rotate();

private:
    void updateSize();
    int lineCountDistanceFromPrimary(int current, int primary, int totalNrOfLines) const;
    QColor currentLineColor(int distance, int totalNrOfLines, qreal trailFadePerc,
                            qreal minOpacity, QColor color) const;

    QTimer *m_timer;
    bool m_isSpinning;
    int m_numberOfLines;
    int m_lineLength;
    int m_lineWidth;
    int m_innerRadius;
    qreal m_roundness;
    qreal m_minimumTrailOpacity;
    qreal m_trailFadePercentage;
    qreal m_revolutionsPerSecond;
    QColor m_color;
    int m_currentCounter;
};

#endif // QTWAITINGSPINNER_H
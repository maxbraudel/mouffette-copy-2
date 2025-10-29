#ifndef CLIPPEDCONTAINER_H
#define CLIPPEDCONTAINER_H

#include <QWidget>
#include <QSize>

/**
 * @brief A container widget that clips its children to a rounded shape
 * 
 * This widget uses QRegion masking to ensure child widgets are properly
 * clipped to rounded corners. The mask is updated efficiently only when
 * the widget size changes.
 */
class ClippedContainer : public QWidget {
    Q_OBJECT

public:
    explicit ClippedContainer(QWidget* parent = nullptr);
    
protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateMaskIfNeeded();
    
    QSize m_lastMaskSize;  ///< Cache last size to avoid unnecessary recalculation
};

#endif // CLIPPEDCONTAINER_H

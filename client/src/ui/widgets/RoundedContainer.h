#ifndef ROUNDEDCONTAINER_H
#define ROUNDEDCONTAINER_H

#include <QWidget>

/**
 * @brief A container widget that paints a rounded rectangle background
 * 
 * This widget automatically adapts to theme changes by using dynamic palette colors.
 * The border radius is configured globally via ThemeManager (future implementation)
 * or accessed through gWindowBorderRadiusPx.
 */
class RoundedContainer : public QWidget {
    Q_OBJECT

public:
    explicit RoundedContainer(QWidget* parent = nullptr);
    
protected:
    void paintEvent(QPaintEvent* event) override;
};

#endif // ROUNDEDCONTAINER_H

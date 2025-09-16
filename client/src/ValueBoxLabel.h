#pragma once

#include <QLabel>
#include <QColor>

// A clickable, focusable value box that accepts keyboard input.
// Behavior:
// - Click to focus: background becomes active (bluish) and keystrokes are appended to content.
// - Backspace deletes; when empty shows placeholder "...".
// - Enter/Return or losing focus exits active state and restores inactive background.
class ValueBoxLabel : public QLabel {
    Q_OBJECT
public:
    explicit ValueBoxLabel(const QString& initial = QStringLiteral("1"), QWidget* parent = nullptr);

    void setActiveColor(const QColor& c) { m_activeColor = c; if (hasFocus()) applyActiveStyle(); }
    void setInactiveColors(const QColor& bg, const QColor& border, const QColor& text = Qt::white) {
        m_inactiveBg = bg; m_inactiveBorder = border; m_textColor = text; if (!hasFocus()) applyInactiveStyle();
    }

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

private:
    void applyActiveStyle();
    void applyInactiveStyle();
    void ensurePlaceholderIfEmpty();

    QColor m_activeColor;         // uses gOverlayActiveBackgroundColor by default
    QColor m_inactiveBg;          // default opaque gray
    QColor m_inactiveBorder;      // default opaque light gray
    QColor m_textColor = Qt::white;
    const QString m_placeholder = QStringLiteral("...");
};

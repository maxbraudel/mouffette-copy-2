#include "ValueBoxLabel.h"
#include "Theme.h"

#include <QMouseEvent>
#include <QKeyEvent>
#include <QFocusEvent>

ValueBoxLabel::ValueBoxLabel(const QString& initial, QWidget* parent)
    : QLabel(parent) {
    setText(initial.isEmpty() ? m_placeholder : initial);
    setAlignment(Qt::AlignCenter);
    setFocusPolicy(Qt::StrongFocus);
    // Default colors
    if (!m_activeColor.isValid()) m_activeColor = gOverlayActiveBackgroundColor;
    if (!m_inactiveBg.isValid()) m_inactiveBg = QColor(60,60,60);
    if (!m_inactiveBorder.isValid()) m_inactiveBorder = QColor(200,200,200);
    applyInactiveStyle();
}

void ValueBoxLabel::mousePressEvent(QMouseEvent* e) {
    QLabel::mousePressEvent(e);
    setFocus(Qt::MouseFocusReason);
    applyActiveStyle();
}

void ValueBoxLabel::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            // Move focus away from the box to ensure deactivation under QGraphicsProxyWidget
            if (QWidget* root = window()) {
                root->setFocus(Qt::OtherFocusReason);
            } else if (QWidget* p = parentWidget()) {
                p->setFocus(Qt::OtherFocusReason);
            } else {
                clearFocus();
            }
            e->accept();
            return;
        case Qt::Key_Escape:
            if (QWidget* root = window()) {
                root->setFocus(Qt::OtherFocusReason);
            } else if (QWidget* p = parentWidget()) {
                p->setFocus(Qt::OtherFocusReason);
            } else {
                clearFocus();
            }
            e->accept();
            return;
        case Qt::Key_Backspace: {
            QString t = text();
            if (t == m_placeholder) t.clear();
            if (!t.isEmpty()) t.chop(1);
            setText(t.isEmpty() ? m_placeholder : t);
            e->accept();
            return;
        }
        default: {
            const QString s = e->text();
            if (!s.isEmpty() && !s.at(0).isLowSurrogate()) {
                QString t = text();
                if (t == m_placeholder) t.clear();
                t.append(s);
                setText(t);
                e->accept();
                return;
            }
        }
    }
    QLabel::keyPressEvent(e);
}

void ValueBoxLabel::focusInEvent(QFocusEvent* e) {
    QLabel::focusInEvent(e);
    applyActiveStyle();
}

void ValueBoxLabel::focusOutEvent(QFocusEvent* e) {
    QLabel::focusOutEvent(e);
    ensurePlaceholderIfEmpty();
    applyInactiveStyle();
}

void ValueBoxLabel::applyActiveStyle() {
    // Blueish background, keep white text, slightly brighter border
    QColor border = m_activeColor;
    border.setAlpha(255);
    setStyleSheet(QString(
        "QLabel {"
        "  background-color: rgba(%1,%2,%3,%4);"
        "  border: 1px solid rgba(%1,%2,%3,255);"
        "  border-radius: 6px;"
        "  padding: 2px 10px;"
        "  color: white;"
        "}")
        .arg(m_activeColor.red())
        .arg(m_activeColor.green())
        .arg(m_activeColor.blue())
        .arg(m_activeColor.alpha())
    );
}

void ValueBoxLabel::applyInactiveStyle() {
    setStyleSheet(QString(
        "QLabel {"
        "  background-color: rgb(%1,%2,%3);"
        "  border: 1px solid rgb(%4,%5,%6);"
        "  border-radius: 6px;"
        "  padding: 2px 10px;"
        "  color: %7;"
        "}")
        .arg(m_inactiveBg.red()).arg(m_inactiveBg.green()).arg(m_inactiveBg.blue())
        .arg(m_inactiveBorder.red()).arg(m_inactiveBorder.green()).arg(m_inactiveBorder.blue())
        .arg(m_textColor == Qt::white ? "white" : m_textColor.name())
    );
}

void ValueBoxLabel::ensurePlaceholderIfEmpty() {
    if (text().trimmed().isEmpty()) setText(m_placeholder);
}

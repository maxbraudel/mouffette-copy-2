#include "frontend/ui/overlays/canvas/CanvasGlobalOverlayHost.h"

#include "backend/files/Theme.h"
#include "frontend/ui/theme/AppColors.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QSignalBlocker>
#include <QToolButton>
#include <QWidget>

#include <algorithm>

CanvasGlobalOverlayHost::CanvasGlobalOverlayHost(QObject* parent)
    : QObject(parent) {}

void CanvasGlobalOverlayHost::attachViewport(QWidget* viewport) {
    if (m_viewport == viewport) {
        return;
    }

    if (m_toolSelectorContainer) {
        m_toolSelectorContainer->deleteLater();
        m_toolSelectorContainer = nullptr;
        m_selectionToolButton = nullptr;
        m_textToolButton = nullptr;
    }
    if (m_settingsToggleButton) {
        m_settingsToggleButton->deleteLater();
        m_settingsToggleButton = nullptr;
    }

    m_viewport = viewport;
}

void CanvasGlobalOverlayHost::ensureSettingsToggleButton() {
    if (m_settingsToggleButton || !m_viewport) return;

    m_settingsToggleButton = new QToolButton(m_viewport);
    m_settingsToggleButton->setIcon(QIcon(QStringLiteral(":/icons/icons/settings.svg")));
    m_settingsToggleButton->setObjectName("SettingsToggleButton");
    m_settingsToggleButton->setCheckable(true);
    m_settingsToggleButton->setAttribute(Qt::WA_NoMousePropagation, true);
    m_settingsToggleButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_settingsToggleButton->setAutoRaise(false);
    m_settingsToggleButton->setFocusPolicy(Qt::NoFocus);
    m_settingsToggleButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    const QString baseBg = AppColors::colorToCss(AppColors::gOverlayBackgroundColor);
    const QString activeBg = AppColors::colorToCss(AppColors::gOverlayActiveBackgroundColor);
    const QString borderColor = AppColors::colorToCss(AppColors::gOverlayBorderColor);
    QColor disabledColor = AppColors::gOverlayBackgroundColor;
    disabledColor.setAlphaF(std::clamp(disabledColor.alphaF() * 0.35, 0.0, 1.0));
    const QString disabledBg = AppColors::colorToCss(disabledColor);

    const QString cornerRadiusPx = QString::number(gOverlayCornerRadiusPx) + QStringLiteral("px");
    const QString style = QStringLiteral(
        "QToolButton#SettingsToggleButton {"
        " background-color: %1;"
        " border: 1px solid %2;"
        " border-radius: %3;"
        " padding: 0;"
        " margin: 0;"
        "}"
        "QToolButton#SettingsToggleButton:hover:!disabled:!checked { background-color: %1; }"
        "QToolButton#SettingsToggleButton:pressed { background-color: %4; }"
        "QToolButton#SettingsToggleButton:checked { background-color: %4; }"
        "QToolButton#SettingsToggleButton:checked:hover { background-color: %4; }"
        "QToolButton#SettingsToggleButton:disabled { background-color: %5; border: 1px solid %2; }"
    ).arg(baseBg, borderColor, cornerRadiusPx, activeBg, disabledBg);
    m_settingsToggleButton->setStyleSheet(style);

    connect(m_settingsToggleButton, &QToolButton::toggled, this, &CanvasGlobalOverlayHost::settingsToggled);

    m_settingsToggleButton->show();
}

void CanvasGlobalOverlayHost::ensureToolSelector() {
    if (m_toolSelectorContainer || !m_viewport) return;

    m_toolSelectorContainer = new QWidget(m_viewport);
    m_toolSelectorContainer->setAttribute(Qt::WA_NoMousePropagation, true);
    m_toolSelectorContainer->setAttribute(Qt::WA_TranslucentBackground, true);
    m_toolSelectorContainer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_toolSelectorContainer->setStyleSheet("background: transparent;");

    QHBoxLayout* layout = new QHBoxLayout(m_toolSelectorContainer);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->setSizeConstraint(QLayout::SetFixedSize);

    m_selectionToolButton = new QToolButton(m_toolSelectorContainer);
    m_selectionToolButton->setIcon(QIcon(QStringLiteral(":/icons/icons/tools/selection-tool.svg")));
    m_selectionToolButton->setObjectName("SelectionToolButton");
    m_selectionToolButton->setCheckable(true);
    m_selectionToolButton->setChecked(true);
    m_selectionToolButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_selectionToolButton->setAutoRaise(false);
    m_selectionToolButton->setFocusPolicy(Qt::NoFocus);
    m_selectionToolButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    m_textToolButton = new QToolButton(m_toolSelectorContainer);
    m_textToolButton->setIcon(QIcon(QStringLiteral(":/icons/icons/tools/text-tool.svg")));
    m_textToolButton->setObjectName("TextToolButton");
    m_textToolButton->setCheckable(true);
    m_textToolButton->setChecked(false);
    m_textToolButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_textToolButton->setAutoRaise(false);
    m_textToolButton->setFocusPolicy(Qt::NoFocus);
    m_textToolButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    QFrame* divider = new QFrame(m_toolSelectorContainer);
    divider->setFrameShape(QFrame::VLine);
    divider->setFixedWidth(1);
    divider->setStyleSheet(QStringLiteral("background-color: %1;")
        .arg(AppColors::colorToCss(AppColors::gOverlayBorderColor)));

    const QString baseBg = AppColors::colorToCss(AppColors::gOverlayBackgroundColor);
    const QString activeBg = AppColors::colorToCss(AppColors::gOverlayActiveBackgroundColor);
    const QString borderColor = AppColors::colorToCss(AppColors::gOverlayBorderColor);
    const QString cornerRadiusPx = QString::number(gOverlayCornerRadiusPx) + QStringLiteral("px");

    const QString leftStyle = QStringLiteral(
        "QToolButton#SelectionToolButton {"
        " background-color: %1;"
        " border: 1px solid %2;"
        " border-top-left-radius: %3;"
        " border-bottom-left-radius: %3;"
        " border-top-right-radius: 0px;"
        " border-bottom-right-radius: 0px;"
        " border-right: none;"
        " padding: 0;"
        " margin: 0;"
        "}"
        "QToolButton#SelectionToolButton:hover:!disabled:!checked { background-color: %1; }"
        "QToolButton#SelectionToolButton:pressed { background-color: %4; }"
        "QToolButton#SelectionToolButton:checked { background-color: %4; }"
        "QToolButton#SelectionToolButton:checked:hover { background-color: %4; }"
    ).arg(baseBg, borderColor, cornerRadiusPx, activeBg);

    const QString rightStyle = QStringLiteral(
        "QToolButton#TextToolButton {"
        " background-color: %1;"
        " border: 1px solid %2;"
        " border-top-left-radius: 0px;"
        " border-bottom-left-radius: 0px;"
        " border-top-right-radius: %3;"
        " border-bottom-right-radius: %3;"
        " border-left: none;"
        " padding: 0;"
        " margin: 0;"
        "}"
        "QToolButton#TextToolButton:hover:!disabled:!checked { background-color: %1; }"
        "QToolButton#TextToolButton:pressed { background-color: %4; }"
        "QToolButton#TextToolButton:checked { background-color: %4; }"
        "QToolButton#TextToolButton:checked:hover { background-color: %4; }"
    ).arg(baseBg, borderColor, cornerRadiusPx, activeBg);

    m_selectionToolButton->setStyleSheet(leftStyle);
    m_textToolButton->setStyleSheet(rightStyle);

    layout->addWidget(m_selectionToolButton);
    layout->addWidget(divider);
    layout->addWidget(m_textToolButton);

    connect(m_selectionToolButton, &QToolButton::clicked, this, [this]() {
        emit toolSelected(ToolChoice::Selection);
    });
    connect(m_textToolButton, &QToolButton::clicked, this, [this]() {
        emit toolSelected(ToolChoice::Text);
    });

    m_toolSelectorContainer->show();
}

void CanvasGlobalOverlayHost::updateGeometry(int margin, int spacing, int buttonSize, int iconSize) {
    ensureSettingsToggleButton();
    ensureToolSelector();

    if (m_settingsToggleButton) {
        m_settingsToggleButton->setFixedSize(buttonSize, buttonSize);
        m_settingsToggleButton->setIconSize(QSize(iconSize, iconSize));
        m_settingsToggleButton->move(margin, margin);
        m_settingsToggleButton->raise();
        m_settingsToggleButton->show();
    }

    if (m_selectionToolButton && m_textToolButton && m_toolSelectorContainer && m_settingsToggleButton) {
        m_selectionToolButton->setFixedSize(buttonSize, buttonSize);
        m_selectionToolButton->setIconSize(QSize(iconSize, iconSize));
        m_textToolButton->setFixedSize(buttonSize, buttonSize);
        m_textToolButton->setIconSize(QSize(iconSize, iconSize));

        const int dividerWidth = 1;
        const int totalWidth = (buttonSize * 2) + dividerWidth;
        m_toolSelectorContainer->setFixedSize(totalWidth, buttonSize);

        int settingsButtonRight = m_settingsToggleButton->x() + m_settingsToggleButton->width();
        m_toolSelectorContainer->move(settingsButtonRight + spacing, margin);
        m_toolSelectorContainer->raise();
        m_toolSelectorContainer->show();
    }
}

void CanvasGlobalOverlayHost::setCurrentTool(ToolChoice tool) {
    m_currentTool = tool;
    if (m_selectionToolButton && m_textToolButton) {
        QSignalBlocker leftBlocker(m_selectionToolButton);
        QSignalBlocker rightBlocker(m_textToolButton);
        m_selectionToolButton->setChecked(tool == ToolChoice::Selection);
        m_textToolButton->setChecked(tool == ToolChoice::Text);
    }
}

bool CanvasGlobalOverlayHost::isSettingsChecked() const {
    return m_settingsToggleButton ? m_settingsToggleButton->isChecked() : false;
}

void CanvasGlobalOverlayHost::setSettingsChecked(bool checked, bool silent) {
    if (!m_settingsToggleButton) {
        return;
    }
    if (silent) {
        QSignalBlocker blocker(m_settingsToggleButton);
        m_settingsToggleButton->setChecked(checked);
        return;
    }
    m_settingsToggleButton->setChecked(checked);
}

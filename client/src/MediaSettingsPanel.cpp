#include "MediaSettingsPanel.h"
#include "Theme.h"
#include "AppColors.h"
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include "MediaItems.h" // for ResizableMediaBase

MediaSettingsPanel::MediaSettingsPanel(QWidget* parentWidget)
    : QObject(parentWidget)
{
    buildUi(parentWidget);
}

void MediaSettingsPanel::buildUi(QWidget* parentWidget) {
    m_widget = new QWidget(parentWidget);
    m_widget->setObjectName("MediaSettingsPanelWidget");

    // Enable styled background so the QWidget can paint the overlay chrome itself
    m_widget->setAttribute(Qt::WA_StyledBackground, true);
    // Ensure overlay blocks mouse events to canvas behind it (like media overlay)
    m_widget->setAttribute(Qt::WA_NoMousePropagation, true);
    // Apply the same background, border, and typography styling as the media list overlay
    const QString overlayTextCss = AppColors::colorToCss(AppColors::gOverlayTextColor);
    const QString overlayBorderCss = AppColors::colorToCss(AppColors::gOverlayBorderColor);
    const QString overlayTextStyle = QStringLiteral("color: %1;").arg(overlayTextCss);
    const QString widgetStyle = QStringLiteral(
        "#MediaSettingsPanelWidget {"
        " background-color: %1;"
        " border: 1px solid %2;"
        " border-radius: %3px;"
        " color: %4;"
        " font-size: 16px;"
        "}"
        " #MediaSettingsPanelWidget * {"
        " background-color: transparent;"
        "}"
    )
    .arg(AppColors::colorToCss(AppColors::gOverlayBackgroundColor))
    .arg(overlayBorderCss)
    .arg(gOverlayCornerRadiusPx)
    .arg(overlayTextCss);
    m_widget->setStyleSheet(widgetStyle);
    m_widget->setAutoFillBackground(true);

    // Root layout on widget
    m_rootLayout = new QVBoxLayout(m_widget);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);
    m_rootLayout->setSizeConstraint(QLayout::SetNoConstraint);

    // Container that holds the scrollable content
    m_scrollContainer = new QWidget(m_widget);
    m_scrollContainer->setObjectName("MediaSettingsScrollContainer");
    m_scrollContainer->setAttribute(Qt::WA_StyledBackground, true);
    m_scrollContainer->setAttribute(Qt::WA_NoMousePropagation, true);
    m_scrollContainer->setAutoFillBackground(false);
    m_scrollContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_scrollContainer->installEventFilter(this);
    auto* scrollContainerLayout = new QVBoxLayout(m_scrollContainer);
    scrollContainerLayout->setContentsMargins(0, 0, 0, 0);
    scrollContainerLayout->setSpacing(0);

    // Scroll area for inner content (mirror media overlay)
    m_scrollArea = new QScrollArea(m_scrollContainer);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setWidgetResizable(true);
    // Ensure scroll area blocks mouse propagation
    m_scrollArea->setAttribute(Qt::WA_NoMousePropagation, true);
    if (auto* hBar = m_scrollArea->horizontalScrollBar()) { hBar->setEnabled(false); hBar->hide(); }
    if (m_scrollArea->viewport()) {
        m_scrollArea->viewport()->setAutoFillBackground(false);
        // Block mouse propagation on viewport too
        m_scrollArea->viewport()->setAttribute(Qt::WA_NoMousePropagation, true);
    }
    if (auto* vBar = m_scrollArea->verticalScrollBar()) vBar->hide();
    m_scrollArea->setStyleSheet(
        "QAbstractScrollArea { background: transparent; border: none; }"
        " QAbstractScrollArea > QWidget#qt_scrollarea_viewport { background: transparent; }"
        " QAbstractScrollArea::corner { background: transparent; }"
        " QScrollArea QScrollBar:vertical { width: 0px; margin: 0; background: transparent; }"
    );
    scrollContainerLayout->addWidget(m_scrollArea);
    m_rootLayout->addWidget(m_scrollContainer);
    m_scrollContainer->setVisible(true);

    // Inner content widget inside scroll area (with margins like before)
    m_innerContent = new QWidget(m_scrollArea);
    m_innerContent->setAttribute(Qt::WA_StyledBackground, true);
    m_innerContent->setAttribute(Qt::WA_NoMousePropagation, true);
    m_innerContent->setStyleSheet(QStringLiteral("background-color: transparent; color: %1; font-size: 16px;")
        .arg(overlayTextCss));
    m_scrollArea->setWidget(m_innerContent);

    // Content layout with the previous margins/spacing
    m_contentLayout = new QVBoxLayout(m_innerContent);
    m_contentLayout->setContentsMargins(20, 16, 20, 16);
    m_contentLayout->setSpacing(10);
    m_contentLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);

    m_title = new QLabel("Scene options");
    QFont tf = m_title->font();
    tf.setBold(true); // keep bold, but do not change size beyond 16px baseline
    m_title->setFont(tf);
    m_title->setStyleSheet(overlayTextStyle);
    m_contentLayout->addWidget(m_title);

    // Helper to create a small value box label like [1]
    auto makeValueBox = [&](const QString& text = QStringLiteral("1")) {
        auto* box = new QLabel(text);
        box->setAlignment(Qt::AlignCenter);
        // Make clickable and install event filter for click handling
        box->setAttribute(Qt::WA_Hover, true);
        box->setFocusPolicy(Qt::ClickFocus); // allow focus for key events
        box->installEventFilter(this);
        setBoxActive(box, false); // set initial inactive style
        box->setMinimumWidth(28);
        return box;
    };

    // 0) Display automatically + Display delay as separate checkboxes
    {
        // Display automatically checkbox with proper alignment
        auto* autoRow = new QWidget(m_widget);
        auto* autoLayout = new QHBoxLayout(autoRow);
        autoLayout->setContentsMargins(0, 0, 0, 0);
        autoLayout->setSpacing(0);
        m_displayAfterCheck = new QCheckBox("Display automatically", autoRow);
        m_displayAfterCheck->setStyleSheet(overlayTextStyle);
        m_displayAfterCheck->installEventFilter(this);
        autoLayout->addWidget(m_displayAfterCheck);
        autoLayout->addStretch();
        m_contentLayout->addWidget(autoRow);
        
        // Display delay checkbox with input (separate checkbox)
        auto* delayRow = new QWidget(m_widget);
        auto* h = new QHBoxLayout(delayRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        m_displayDelayCheck = new QCheckBox("Display delay: ", delayRow);
        m_displayDelayCheck->setStyleSheet(overlayTextStyle);
        m_displayDelayCheck->installEventFilter(this);
    connect(m_displayDelayCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        m_displayAfterBox = makeValueBox();
        m_displayAfterSecondsLabel = new QLabel("s", delayRow);
        m_displayAfterSecondsLabel->setStyleSheet(overlayTextStyle);
        h->addWidget(m_displayDelayCheck);
        h->addWidget(m_displayAfterBox);
        h->addWidget(m_displayAfterSecondsLabel);
        h->addStretch();
        m_contentLayout->addWidget(delayRow);
    }

    // 1) Play automatically as separate widget (video only) - matching display layout
    {
        m_autoPlayRow = new QWidget(m_widget);
        auto* autoLayout = new QHBoxLayout(m_autoPlayRow);
        autoLayout->setContentsMargins(0, 0, 0, 0);
        autoLayout->setSpacing(0);
        m_autoPlayCheck = new QCheckBox("Play automatically", m_autoPlayRow);
        m_autoPlayCheck->setStyleSheet(overlayTextStyle);
        m_autoPlayCheck->installEventFilter(this);
        autoLayout->addWidget(m_autoPlayCheck);
        autoLayout->addStretch();
        m_contentLayout->addWidget(m_autoPlayRow);
    }
    
    // Play delay as a separate widget (video only) - matching display delay layout
    {
        m_playDelayRow = new QWidget(m_widget);
        auto* h = new QHBoxLayout(m_playDelayRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        m_playDelayCheck = new QCheckBox("Play delay: ", m_playDelayRow);
        m_playDelayCheck->setStyleSheet(overlayTextStyle);
        m_playDelayCheck->installEventFilter(this);
    connect(m_playDelayCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        m_autoPlayBox = makeValueBox();
        m_autoPlaySecondsLabel = new QLabel("s", m_playDelayRow);
        m_autoPlaySecondsLabel->setStyleSheet(overlayTextStyle);
        h->addWidget(m_playDelayCheck);
        h->addWidget(m_autoPlayBox);
        h->addWidget(m_autoPlaySecondsLabel);
        h->addStretch();
        m_contentLayout->addWidget(m_playDelayRow);
    }

    // 2) Repeat (video only) - keeping original single checkbox format
    {
        m_repeatRow = new QWidget(m_widget);
        auto* h = new QHBoxLayout(m_repeatRow);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(0);
        m_repeatCheck = new QCheckBox("Repeat ", m_repeatRow);
        m_repeatCheck->setStyleSheet(overlayTextStyle);
        m_repeatCheck->installEventFilter(this);
    connect(m_repeatCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        m_repeatBox = makeValueBox();
        auto* suffix = new QLabel(" times", m_repeatRow);
        suffix->setStyleSheet(overlayTextStyle);
        h->addWidget(m_repeatCheck);
        h->addWidget(m_repeatBox);
        h->addWidget(suffix);
        h->addStretch();
        m_contentLayout->addWidget(m_repeatRow);
    }

    // Element Properties section header
    m_elementPropertiesTitle = new QLabel("Element Properties");
    QFont epf = m_elementPropertiesTitle->font();
    epf.setBold(true);
    m_elementPropertiesTitle->setFont(epf);
    m_elementPropertiesTitle->setStyleSheet(overlayTextStyle);
    m_contentLayout->addWidget(m_elementPropertiesTitle);

    // 3) Fade in with checkbox format
    {
        auto* row = new QWidget(m_widget);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(0);
        m_fadeInCheck = new QCheckBox("Fade in: ", row);
        m_fadeInCheck->setStyleSheet(overlayTextStyle);
        m_fadeInCheck->installEventFilter(this);
    connect(m_fadeInCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        m_fadeInBox = makeValueBox();
        auto* suffix = new QLabel("s", row);
        suffix->setStyleSheet(overlayTextStyle);
        h->addWidget(m_fadeInCheck);
        h->addWidget(m_fadeInBox);
        h->addWidget(suffix);
        h->addStretch();
        m_contentLayout->addWidget(row);
    }

    // 4) Fade out with checkbox format
    {
        auto* row = new QWidget(m_widget);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(0);
        m_fadeOutCheck = new QCheckBox("Fade out: ", row);
        m_fadeOutCheck->setStyleSheet(overlayTextStyle);
        m_fadeOutCheck->installEventFilter(this);
    connect(m_fadeOutCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        m_fadeOutBox = makeValueBox();
        auto* suffix = new QLabel("s", row);
        suffix->setStyleSheet(overlayTextStyle);
        h->addWidget(m_fadeOutCheck);
        h->addWidget(m_fadeOutBox);
        h->addWidget(suffix);
        h->addStretch();
        m_contentLayout->addWidget(row);
    }

    // 5) Opacity with checkbox format
    {
        auto* row = new QWidget(m_widget);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(0);
        m_opacityCheck = new QCheckBox("Opacity: ", row);
        m_opacityCheck->setStyleSheet(overlayTextStyle);
        m_opacityCheck->installEventFilter(this);
        m_opacityBox = makeValueBox(QStringLiteral("100")); // Default to 100%
        auto* suffix = new QLabel("%", row);
        suffix->setStyleSheet(overlayTextStyle);
        h->addWidget(m_opacityCheck);
        h->addWidget(m_opacityBox);
        h->addWidget(suffix);
        h->addStretch();
        QObject::connect(m_opacityCheck, &QCheckBox::toggled, this, &MediaSettingsPanel::onOpacityToggled);
        m_contentLayout->addWidget(row);
    }
    // Widget is now directly parented to toolbar container, no proxy needed
    m_widget->setMouseTracking(true);
    m_widget->setFixedWidth(m_panelWidthPx);
    
    // Install event filter on the main widget and inner content to catch clicks/wheel
    m_widget->installEventFilter(this);
    if (m_innerContent) m_innerContent->installEventFilter(this);
    
    // Create floating overlay scrollbar synced with scroll area
    if (!m_overlayVScroll) {
        m_overlayVScroll = new QScrollBar(Qt::Vertical, m_widget);
        m_overlayVScroll->setObjectName("settingsOverlayVScroll");
        m_overlayVScroll->setAutoFillBackground(false);
        m_overlayVScroll->setAttribute(Qt::WA_TranslucentBackground, true);
        m_overlayVScroll->setCursor(Qt::ArrowCursor);
        // Hide scrollbar by default - only show on scroll interaction
        m_overlayVScroll->hide();
        m_overlayVScroll->setStyleSheet(
            "QScrollBar#settingsOverlayVScroll { background: transparent; border: none; width: 8px; margin: 0px; }"
            " QScrollBar#settingsOverlayVScroll::groove:vertical { background: transparent; border: none; margin: 0px; }"
            " QScrollBar#settingsOverlayVScroll::handle:vertical { background: rgba(255,255,255,0.35); min-height: 24px; border-radius: 4px; }"
            " QScrollBar#settingsOverlayVScroll::handle:vertical:hover { background: rgba(255,255,255,0.55); }"
            " QScrollBar#settingsOverlayVScroll::handle:vertical:pressed { background: rgba(255,255,255,0.7); }"
            " QScrollBar#settingsOverlayVScroll::add-line:vertical, QScrollBar#settingsOverlayVScroll::sub-line:vertical { height: 0px; width: 0px; background: transparent; border: none; }"
            " QScrollBar#settingsOverlayVScroll::add-page:vertical, QScrollBar#settingsOverlayVScroll::sub-page:vertical { background: transparent; }"
        );
        // Auto-hide timer
        if (!m_scrollbarHideTimer) {
            m_scrollbarHideTimer = new QTimer(this);
            m_scrollbarHideTimer->setSingleShot(true);
            m_scrollbarHideTimer->setInterval(500);
            connect(m_scrollbarHideTimer, &QTimer::timeout, this, [this]() {
                if (m_overlayVScroll) m_overlayVScroll->hide();
            });
        }
        // Sync with scroll area's vertical scrollbar
        QScrollBar* src = m_scrollArea->verticalScrollBar();
        connect(m_overlayVScroll, &QScrollBar::valueChanged, src, &QScrollBar::setValue);
        connect(src, &QScrollBar::rangeChanged, this, [this](int min, int max){
            if (m_overlayVScroll) {
                m_overlayVScroll->setRange(min, max);
                // Sync page step for proper thumb sizing
                m_overlayVScroll->setPageStep(m_scrollArea->verticalScrollBar()->pageStep());
            }
            updateScrollbarGeometry();
        });
        connect(src, &QScrollBar::valueChanged, this, [this](int v){ if (m_overlayVScroll) m_overlayVScroll->setValue(v); });
        auto showScrollbarAndRestartTimer = [this]() {
            if (m_overlayVScroll && m_scrollbarHideTimer) {
                m_overlayVScroll->show();
                m_scrollbarHideTimer->start();
            }
        };
        connect(m_overlayVScroll, &QScrollBar::valueChanged, this, showScrollbarAndRestartTimer);
        connect(src, &QScrollBar::valueChanged, this, showScrollbarAndRestartTimer);
        
        // Initialize current values immediately (including pageStep for proper thumb sizing)
        m_overlayVScroll->setRange(src->minimum(), src->maximum());
        m_overlayVScroll->setPageStep(src->pageStep());
        m_overlayVScroll->setValue(src->value());
    }

    // Connect display automatically checkbox to enable/disable display delay controls
    if (m_displayAfterCheck) {
        connect(m_displayAfterCheck, &QCheckBox::toggled, this, &MediaSettingsPanel::onDisplayAutomaticallyToggled);
        // Set initial state (display delay disabled by default since display automatically is unchecked)
        onDisplayAutomaticallyToggled(m_displayAfterCheck->isChecked());
    }
    
    // Connect play automatically checkbox to enable/disable play delay controls
    if (m_autoPlayCheck) {
        connect(m_autoPlayCheck, &QCheckBox::toggled, this, &MediaSettingsPanel::onPlayAutomaticallyToggled);
        // Set initial state (play delay disabled by default since play automatically is unchecked)
        onPlayAutomaticallyToggled(m_autoPlayCheck->isChecked());
    }
    updatePosition();
}

void MediaSettingsPanel::setVisible(bool visible) {
    if (!m_widget) return;
    m_widget->setVisible(visible);
    // Clear active box when hiding the panel
    if (!visible) {
        clearActiveBox();
    }
}

bool MediaSettingsPanel::isVisible() const {
    return m_widget && m_widget->isVisible();
}

void MediaSettingsPanel::setMediaType(bool isVideo) {
    // Show/hide video-only options
    if (m_autoPlayRow) {
        m_autoPlayRow->setVisible(isVideo);
    }
    if (m_playDelayRow) {
        m_playDelayRow->setVisible(isVideo);
    }
    if (m_repeatRow) {
        m_repeatRow->setVisible(isVideo);
    }
    
    // Clear active box if it belongs to a hidden video-only option
    if (!isVideo && m_activeBox && (m_activeBox == m_autoPlayBox || m_activeBox == m_repeatBox)) {
        clearActiveBox();
    }
    
    // Force layout update to recalculate size
    if (m_widget && m_contentLayout) {
        // Force layout to recalculate
        m_contentLayout->invalidate();
        m_contentLayout->activate();
        
        // Update widget geometry
        m_widget->updateGeometry();
        m_widget->adjustSize();
        
        // Update position to recalculate background rect size
        updatePosition();
    }
}

void MediaSettingsPanel::updatePosition() {
    if (!m_widget) return;
    
    QWidget* viewport = m_widget->parentWidget();
    if (!viewport) return;
    
    const int viewportHeight = viewport->height();
    const int availableHeight = std::max(50, viewportHeight - m_anchorTopMargin - m_anchorBottomMargin);

    updateAvailableHeight(availableHeight);

    m_widget->adjustSize();
    const int desiredHeight = m_widget->sizeHint().height();
    const int boundedHeight = std::min(availableHeight, desiredHeight);
    const QSize newSize(m_panelWidthPx, boundedHeight);
    if (m_widget->size() != newSize) {
        m_widget->resize(newSize);
    }

    m_widget->move(m_anchorLeftMargin, m_anchorTopMargin);

    updateScrollbarGeometry();
}

void MediaSettingsPanel::setAnchorMargins(int left, int top, int bottom) {
    m_anchorLeftMargin = std::max(0, left);
    m_anchorTopMargin = std::max(0, top);
    m_anchorBottomMargin = std::max(0, bottom);
    updatePosition();
}

void MediaSettingsPanel::setBoxActive(QLabel* box, bool active) {
    if (!box) return;
    
    if (active) {
        // Active state: use the same blue tint as enabled settings button (74,144,226)
        box->setStyleSheet(
            QString("QLabel {"
            "  background-color: %1;"
            "  border: 1px solid %1;"
            "  border-radius: 6px;"
            "  padding: 2px 10px;"
            "  margin-left: 4px;"
            "  margin-right: 0px;"
            "  color: white;"
            "}").arg(AppColors::gMediaPanelActiveBg.name())
        );
    } else {
        // Inactive state: subtle translucent box with rounded corners; white text to match panel
        box->setStyleSheet(
            QString("QLabel {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 6px;"
            "  padding: 2px 10px;"
            "  margin-left: 4px;"
            "  margin-right: 0px;"
            "  color: white;"
            "}").arg(AppColors::gMediaPanelInactiveBg.name()).arg(AppColors::gMediaPanelInactiveBorder.name())
        );
    }
}

void MediaSettingsPanel::clearActiveBox() {
    if (m_activeBox) {
        QLabel* was = m_activeBox;
        bool wasOpacityBox = (was == m_opacityBox);
        setBoxActive(m_activeBox, false);
        m_activeBox = nullptr;
        // Reset first-type-clears flag when deactivating
        m_clearOnFirstType = false;
        // If user just finished editing opacity and option is enabled, apply it now
        if (wasOpacityBox && m_opacityCheck && m_opacityCheck->isChecked()) {
            applyOpacityFromUi();
        }
    }
}

bool MediaSettingsPanel::eventFilter(QObject* obj, QEvent* event) {
    if ((obj == m_scrollContainer || obj == m_widget) && event->type() == QEvent::Resize) {
        updateScrollbarGeometry();
    }

    const bool withinPanelHierarchy = m_widget && obj && obj->isWidgetType() &&
        (obj == m_widget || m_widget->isAncestorOf(static_cast<QWidget*>(obj)));

    // Block all mouse interactions from reaching canvas when over settings panel
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonRelease ||
        event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonDblClick) {
        if (withinPanelHierarchy || obj == m_scrollArea || obj == m_scrollContainer) {
            if (event->type() == QEvent::MouseButtonPress) {
                clearActiveBox();
            }
            return false; // Allow widget to process; WA_NoMousePropagation stops propagation
        }
    }
    
    // Handle clicks on value boxes
    if (event->type() == QEvent::MouseButtonPress) {
        QLabel* box = qobject_cast<QLabel*>(obj);
        if (box && (box == m_displayAfterBox || box == m_autoPlayBox || box == m_repeatBox || 
                   box == m_fadeInBox || box == m_fadeOutBox || box == m_opacityBox)) {
            // Don't allow interaction with disabled boxes
            if (!box->isEnabled()) {
                return true; // consume the event but don't activate
            }
            
            // Clear previous active box
            clearActiveBox();
            // Set this box as active
            m_activeBox = box;
            setBoxActive(box, true);
            // Give focus to the box so it can receive key events
            box->setFocus();
            // Enable one-shot clear-on-type behavior
            m_clearOnFirstType = true;
            return true; // consume the event
        }
        // Handle clicks on checkboxes or elsewhere in the panel - clear active box
        else {
            clearActiveBox();
            return false; // don't consume, let other widgets handle
        }
    }
    // Handle key presses when a box is active
    else if (event->type() == QEvent::KeyPress && m_activeBox) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        
        // Enter key deactivates
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            clearActiveBox();
            return true;
        }
        // Backspace clears the box and shows "..."
        else if (keyEvent->key() == Qt::Key_Backspace) {
            m_activeBox->setText("...");
            if (!m_updatingFromMedia) {
                pushSettingsToMedia();
            }
            return true;
        }
        // 'i' key sets infinity symbol
        else if (keyEvent->key() == Qt::Key_I) {
            m_activeBox->setText("∞");
            if (!m_updatingFromMedia) {
                pushSettingsToMedia();
            }
            return true;
        }
        // Handle input based on which box is active
        else {
            QString text = keyEvent->text();
            if (!text.isEmpty() && isValidInputForBox(m_activeBox, text[0])) {
                QString currentText = m_activeBox->text();
                QString newText;
                
                // On first key after selection, replace entire content
                if (m_clearOnFirstType) {
                    newText = text;
                    m_clearOnFirstType = false;
                }
                else if (currentText == "..." || currentText == "∞") {
                    // If current text is cleared state or infinity symbol, replace it
                    newText = text;
                } else {
                    // Append to existing text
                    newText = currentText + text;
                }
                
                // Check if we have more than 5 digits - if so, show infinity symbol
                int digitCount = 0;
                for (QChar c : newText) {
                    if (c.isDigit()) digitCount++;
                }
                
                if (digitCount > 5) {
                    m_activeBox->setText("∞");
                } else {
                    m_activeBox->setText(newText);
                }
                if (!m_updatingFromMedia) {
                    pushSettingsToMedia();
                }
                return true;
            }
        }
    }
    // Consume wheel events over the settings panel so canvas doesn't zoom/scroll
    else if (event->type() == QEvent::Wheel) {
        // Only handle if event is within our widget tree
        if (withinPanelHierarchy || obj == m_scrollContainer || obj == m_scrollArea) {
            // Route to scroll area
            if (m_scrollArea) {
                QCoreApplication::sendEvent(m_scrollArea->viewport(), event);
                return true;
            }
        }
    }
    
    return QObject::eventFilter(obj, event);
}

bool MediaSettingsPanel::isValidInputForBox(QLabel* box, QChar character) {
    if (box == m_displayAfterBox) {
        // Display automatically: numbers, dots, commas
        return character.isDigit() || character == '.' || character == ',';
    }
    else if (box == m_autoPlayBox) {
        // Play automatically: numbers, dots, commas
        return character.isDigit() || character == '.' || character == ',';
    }
    else if (box == m_repeatBox) {
        // Repeat: numbers only (excluding 0)
        return character.isDigit() && character != '0';
    }
    else if (box == m_fadeInBox) {
        // Fade in: numbers, dots, commas
        return character.isDigit() || character == '.' || character == ',';
    }
    else if (box == m_fadeOutBox) {
        // Fade out: numbers, dots, commas
        return character.isDigit() || character == '.' || character == ',';
    }
    else if (box == m_opacityBox) {
        // Opacity: digits only
        return character.isDigit();
    }
    
    return false; // Unknown box, reject input
}

void MediaSettingsPanel::onOpacityToggled(bool checked) {
    Q_UNUSED(checked);
    applyOpacityFromUi();
    if (!m_updatingFromMedia) {
        pushSettingsToMedia();
    }
}

void MediaSettingsPanel::applyOpacityFromUi() {
    if (!m_mediaItem) return;

    if (m_updatingFromMedia) {
        const auto state = m_mediaItem->mediaSettingsState();
        if (state.opacityOverrideEnabled) {
            bool ok = false;
            int val = state.opacityText.trimmed().toInt(&ok);
            if (!ok) val = 100;
            val = std::clamp(val, 0, 100);
            m_mediaItem->setContentOpacity(static_cast<qreal>(val) / 100.0);
        } else {
            m_mediaItem->setContentOpacity(1.0);
        }
        return;
    }

    if (!m_opacityCheck || !m_opacityBox) return;
    pushSettingsToMedia();
}

double MediaSettingsPanel::fadeInSeconds() const {
    if (!m_fadeInCheck || !m_fadeInBox) return 0.0;
    if (!m_fadeInCheck->isChecked()) return 0.0;
    QString t = m_fadeInBox->text().trimmed();
    if (t == "∞" || t.isEmpty() || t == "...") return 0.0; // treat infinity / empty as instant for now
    // Replace comma with dot for parsing
    t.replace(',', '.');
    bool ok=false; double v = t.toDouble(&ok); if (!ok) return 0.0; return std::clamp(v, 0.0, 3600.0); // clamp to 1 hour max
}

bool MediaSettingsPanel::displayAutomaticallyEnabled() const {
    return m_displayAfterCheck && m_displayAfterCheck->isChecked();
}

int MediaSettingsPanel::displayDelayMillis() const {
    if (!m_displayDelayCheck || !m_displayAfterCheck) return 0;
    if (!m_displayDelayCheck->isChecked()) return 0;
    if (!m_displayAfterBox) return 0;
    bool ok=false; int v = m_displayAfterBox->text().trimmed().toInt(&ok);
    if (!ok || v < 0) return 0;
    return v * 1000;
}

bool MediaSettingsPanel::playAutomaticallyEnabled() const {
    return m_autoPlayCheck && m_autoPlayCheck->isChecked();
}

int MediaSettingsPanel::playDelayMillis() const {
    if (!m_playDelayCheck || !m_autoPlayCheck) return 0;
    if (!m_playDelayCheck->isChecked()) return 0;
    if (!m_autoPlayBox) return 0;
    bool ok=false; int v = m_autoPlayBox->text().trimmed().toInt(&ok);
    if (!ok || v < 0) return 0;
    return v * 1000;
}

double MediaSettingsPanel::fadeOutSeconds() const {
    if (!m_fadeOutCheck || !m_fadeOutBox) return 0.0;
    if (!m_fadeOutCheck->isChecked()) return 0.0;
    QString t = m_fadeOutBox->text().trimmed();
    if (t == "∞" || t.isEmpty() || t == "...") return 0.0;
    t.replace(',', '.');
    bool ok=false; double v = t.toDouble(&ok); if (!ok) return 0.0; return std::clamp(v, 0.0, 3600.0);
}

void MediaSettingsPanel::updateScrollbarGeometry() {
    if (!m_overlayVScroll || !m_widget || !m_scrollArea || !m_scrollContainer) return;

    // Hide scrollbar entirely when collapsed or no scrollable content is visible
    if (!m_scrollContainer->isVisible()) {
        m_overlayVScroll->hide();
        return;
    }

    QScrollBar* sourceScrollBar = m_scrollArea->verticalScrollBar();
    if (!sourceScrollBar) {
        m_overlayVScroll->hide();
        return;
    }

    const bool needed = sourceScrollBar->maximum() > sourceScrollBar->minimum();
    if (!needed) {
        m_overlayVScroll->hide();
        return;
    }

    // Align overlay scrollbar with the scroll container so it doesn't overlap the header button
    const QRect scrollRect = m_scrollContainer->geometry();
    const int margin = 6; // inset from right edge for visual parity with other overlays
    const int topMargin = 6;
    const int bottomMargin = 6;
    const int width = 8;
    const int height = qMax(0, scrollRect.height() - topMargin - bottomMargin);
    if (height <= 0) {
        m_overlayVScroll->hide();
        return;
    }
    const int x = scrollRect.x() + scrollRect.width() - width - margin;
    const int y = scrollRect.y() + topMargin;

    m_overlayVScroll->setRange(sourceScrollBar->minimum(), sourceScrollBar->maximum());
    m_overlayVScroll->setPageStep(sourceScrollBar->pageStep());
    m_overlayVScroll->setValue(sourceScrollBar->value());
    m_overlayVScroll->setGeometry(x, y, width, height);

    // Respect auto-hide timing; only force visibility if timer is already keeping it shown
    if (!m_scrollbarHideTimer || m_scrollbarHideTimer->isActive()) {
        m_overlayVScroll->show();
    }
}

void MediaSettingsPanel::onDisplayAutomaticallyToggled(bool checked) {
    // Enable/disable display delay checkbox and input box based on display automatically state
    const QString activeTextStyle = QStringLiteral("color: %1;")
        .arg(AppColors::colorToCss(AppColors::gOverlayTextColor));
    const QString disabledTextStyle = QStringLiteral("color: #808080;");
    
    if (m_displayDelayCheck) {
        m_displayDelayCheck->setEnabled(checked);
        
        // Update visual styling for disabled state
        if (checked) {
            m_displayDelayCheck->setStyleSheet(activeTextStyle);
        } else {
            m_displayDelayCheck->setStyleSheet(disabledTextStyle); // Gray color for disabled
            // Also uncheck the display delay checkbox when disabled
            m_displayDelayCheck->setChecked(false);
        }
    }
    
    if (m_displayAfterBox) {
        m_displayAfterBox->setEnabled(checked);
        
        // Update visual styling for the input box
        if (checked) {
            // Reset to normal styling when enabled
            setBoxActive(m_displayAfterBox, m_activeBox == m_displayAfterBox);
        } else {
            // Apply disabled styling
            m_displayAfterBox->setStyleSheet(
                "QLabel {"
                "  background-color: #404040;"
                "  border: 1px solid #606060;"
                "  border-radius: 6px;"
                "  padding: 2px 10px;"
                "  margin-left: 4px;"
                "  margin-right: 0px;"
                "  color: #808080;"
                "}"
            );
            
            // Clear active state if this box was active
            if (m_activeBox == m_displayAfterBox) {
                clearActiveBox();
            }
        }
    }
    
    // Also update the "seconds" label styling
    if (m_displayAfterSecondsLabel) {
        if (checked) {
            m_displayAfterSecondsLabel->setStyleSheet(activeTextStyle);
        } else {
            m_displayAfterSecondsLabel->setStyleSheet(disabledTextStyle); // Gray color for disabled
        }
    }

    if (!m_updatingFromMedia) {
        pushSettingsToMedia();
    }
}

void MediaSettingsPanel::onPlayAutomaticallyToggled(bool checked) {
    // Enable/disable play delay checkbox and input box based on play automatically state
    const QString activeTextStyle = QStringLiteral("color: %1;")
        .arg(AppColors::colorToCss(AppColors::gOverlayTextColor));
    const QString disabledTextStyle = QStringLiteral("color: #808080;");

    if (m_playDelayCheck) {
        m_playDelayCheck->setEnabled(checked);
        
        // Update visual styling for disabled state
        if (checked) {
            m_playDelayCheck->setStyleSheet(activeTextStyle);
        } else {
            m_playDelayCheck->setStyleSheet(disabledTextStyle); // Gray color for disabled
            // Also uncheck the play delay checkbox when disabled
            m_playDelayCheck->setChecked(false);
        }
    }
    
    if (m_autoPlayBox) {
        m_autoPlayBox->setEnabled(checked);
        
        // Update visual styling for the input box
        if (checked) {
            // Reset to normal styling when enabled
            setBoxActive(m_autoPlayBox, m_activeBox == m_autoPlayBox);
        } else {
            // Apply disabled styling
            m_autoPlayBox->setStyleSheet(
                "QLabel {"
                "  background-color: #404040;"
                "  border: 1px solid #606060;"
                "  border-radius: 6px;"
                "  padding: 2px 10px;"
                "  margin-left: 4px;"
                "  margin-right: 0px;"
                "  color: #808080;"
                "}"
            );
            
            // Clear active state if this box was active
            if (m_activeBox == m_autoPlayBox) {
                clearActiveBox();
            }
        }
    }
    
    // Also update the "seconds" label styling
    if (m_autoPlaySecondsLabel) {
        if (checked) {
            m_autoPlaySecondsLabel->setStyleSheet(activeTextStyle);
        } else {
            m_autoPlaySecondsLabel->setStyleSheet(disabledTextStyle); // Gray color for disabled
        }
    }

    if (!m_updatingFromMedia) {
        pushSettingsToMedia();
    }
}

void MediaSettingsPanel::updateAvailableHeight(int maxHeightPx) {
    if (!m_widget) return;
    if (maxHeightPx <= 0) {
        m_widget->setMaximumHeight(QWIDGETSIZE_MAX);
        if (m_scrollArea) {
            m_scrollArea->setMaximumHeight(QWIDGETSIZE_MAX);
            m_scrollArea->setMinimumHeight(0);
        }
        updateScrollbarGeometry();
        return;
    }

    const int clamped = std::max(50, maxHeightPx);
    m_widget->setMaximumHeight(clamped);
    m_widget->setMinimumHeight(0);

    if (m_scrollArea) {
        m_scrollArea->setMaximumHeight(clamped);
        m_scrollArea->setMinimumHeight(0);
    }

    m_widget->updateGeometry();
    if (m_scrollArea) {
        m_scrollArea->updateGeometry();
    }
    updateScrollbarGeometry();
}

void MediaSettingsPanel::setMediaItem(ResizableMediaBase* item) {
    clearActiveBox();
    if (m_mediaItem == item) {
        if (m_mediaItem) {
            pullSettingsFromMedia();
        }
        return;
    }
    m_mediaItem = item;
    pullSettingsFromMedia();
}

void MediaSettingsPanel::pullSettingsFromMedia() {
    m_updatingFromMedia = true;
    if (!m_mediaItem) {
        m_updatingFromMedia = false;
        return;
    }

    const auto state = m_mediaItem->mediaSettingsState();

    auto applyCheckState = [](QCheckBox* box, bool checked) {
        if (!box) return;
        const bool prev = box->blockSignals(true);
        box->setChecked(checked);
        box->blockSignals(prev);
    };

    auto applyBoxText = [](QLabel* label, const QString& text, const QString& fallback) {
        if (!label) return;
        const QString value = text.isEmpty() ? fallback : text;
        label->setText(value);
    };

    applyCheckState(m_displayAfterCheck, state.displayAutomatically);
    applyCheckState(m_displayDelayCheck, state.displayDelayEnabled);
    applyCheckState(m_autoPlayCheck, state.playAutomatically);
    applyCheckState(m_playDelayCheck, state.playDelayEnabled);
    applyCheckState(m_repeatCheck, state.repeatEnabled);
    applyCheckState(m_fadeInCheck, state.fadeInEnabled);
    applyCheckState(m_fadeOutCheck, state.fadeOutEnabled);
    applyCheckState(m_opacityCheck, state.opacityOverrideEnabled);

    applyBoxText(m_displayAfterBox, state.displayDelayText, QStringLiteral("1"));
    applyBoxText(m_autoPlayBox, state.playDelayText, QStringLiteral("1"));
    applyBoxText(m_repeatBox, state.repeatCountText, QStringLiteral("1"));
    applyBoxText(m_fadeInBox, state.fadeInText, QStringLiteral("1"));
    applyBoxText(m_fadeOutBox, state.fadeOutText, QStringLiteral("1"));
    applyBoxText(m_opacityBox, state.opacityText, QStringLiteral("100"));

    // Re-run UI interlock logic without persisting back to the media item
    onDisplayAutomaticallyToggled(m_displayAfterCheck ? m_displayAfterCheck->isChecked() : false);
    onPlayAutomaticallyToggled(m_autoPlayCheck ? m_autoPlayCheck->isChecked() : false);
    onOpacityToggled(m_opacityCheck ? m_opacityCheck->isChecked() : false);

    m_updatingFromMedia = false;

    // Ensure opacity is immediately applied (uses stored state when guard is false)
    applyOpacityFromUi();
}

void MediaSettingsPanel::pushSettingsToMedia() {
    if (m_updatingFromMedia) return;
    if (!m_mediaItem) return;

    auto trimmedText = [](QLabel* label, const QString& fallback) {
        if (!label) return fallback;
        const QString text = label->text().trimmed();
        return text.isEmpty() ? fallback : text;
    };

    ResizableMediaBase::MediaSettingsState state = m_mediaItem->mediaSettingsState();
    state.displayAutomatically = m_displayAfterCheck && m_displayAfterCheck->isChecked();
    state.displayDelayEnabled = m_displayDelayCheck && m_displayDelayCheck->isChecked();
    state.displayDelayText = trimmedText(m_displayAfterBox, state.displayDelayText);
    state.playAutomatically = m_autoPlayCheck && m_autoPlayCheck->isChecked();
    state.playDelayEnabled = m_playDelayCheck && m_playDelayCheck->isChecked();
    state.playDelayText = trimmedText(m_autoPlayBox, state.playDelayText);
    state.repeatEnabled = m_repeatCheck && m_repeatCheck->isChecked();
    state.repeatCountText = trimmedText(m_repeatBox, state.repeatCountText);
    state.fadeInEnabled = m_fadeInCheck && m_fadeInCheck->isChecked();
    state.fadeInText = trimmedText(m_fadeInBox, state.fadeInText);
    state.fadeOutEnabled = m_fadeOutCheck && m_fadeOutCheck->isChecked();
    state.fadeOutText = trimmedText(m_fadeOutBox, state.fadeOutText);
    state.opacityOverrideEnabled = m_opacityCheck && m_opacityCheck->isChecked();
    state.opacityText = trimmedText(m_opacityBox, state.opacityText);

    m_mediaItem->setMediaSettingsState(state);
}

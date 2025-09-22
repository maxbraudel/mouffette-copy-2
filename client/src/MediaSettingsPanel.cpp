#include "MediaSettingsPanel.h"
#include "Theme.h"
#include "AppColors.h"
#include "OverlayPanels.h"
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsProxyWidget>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QPalette>
#include <QGuiApplication>
#include "Theme.h"
#include "OverlayPanels.h"

MediaSettingsPanel::MediaSettingsPanel(QObject* parent)
    : QObject(parent)
{
    buildUi();
}

MediaSettingsPanel::~MediaSettingsPanel() {
    if (m_proxy && m_proxy->scene()) m_proxy->scene()->removeItem(m_proxy);
    delete m_proxy;
}

void MediaSettingsPanel::buildUi() {
    m_widget = new QWidget();
    m_widget->setObjectName("MediaSettingsPanelWidget");

    // Make the QWidget visually transparent; we'll draw an exact rounded background in the scene
    m_widget->setAttribute(Qt::WA_StyledBackground, true);
    // Apply unified font size to match media filename overlay (OverlayTextElement uses 16px)
    m_widget->setStyleSheet("background-color: transparent; color: white; font-size: 16px;");
    m_widget->setAutoFillBackground(false);

    m_layout = new QVBoxLayout(m_widget);
    m_layout->setContentsMargins(20, 30, 20, 35); // Extra bottom margin to prevent clipping
    m_layout->setSpacing(12); // Slightly more spacing
    // Use NoConstraint to let the layout expand freely
    m_layout->setSizeConstraint(QLayout::SetNoConstraint);
    
    // Start with no size constraints - let updatePosition handle width constraints
    m_widget->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    m_widget->setMinimumSize(0, 0);
    // Set size policy for natural expansion
    m_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);

    m_title = new QLabel("Scene options");
    QFont tf = m_title->font();
    tf.setBold(true); // keep bold, but do not change size beyond 16px baseline
    m_title->setFont(tf);
    m_title->setStyleSheet("color: white;");
    m_layout->addWidget(m_title);
    
    // Add extra spacing after the title
    m_layout->addSpacing(15);

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
        // Display automatically checkbox with separate label below
        auto* autoRow = new QWidget(m_widget);
        auto* autoLayout = new QVBoxLayout(autoRow); // Use vertical layout for checkbox + label
        autoLayout->setContentsMargins(0, 0, 0, 0);
        autoLayout->setSpacing(4); // Small spacing between checkbox and label
        
        m_displayAfterCheck = new QCheckBox(autoRow); // Empty checkbox text
        m_displayAfterCheck->installEventFilter(this);
        
        auto* displayLabel = new QLabel("Display automatically with very Display automatically with veryDisplay automatically with veryDisplay automatically with veryDisplay automatically with veryDisplay automatically with veryDisplay automatically with veryDisplay automatically with veryDisplay automatically with very ", autoRow);
        displayLabel->setStyleSheet("color: white; padding: 4px 0px; line-height: 1.3em;"); // Better line height for readability
        displayLabel->setWordWrap(true); // Enable word wrapping on the label
        displayLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft); // Ensure proper alignment
        // Critical: Use Preferred for both width and height to allow natural sizing
        displayLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        // Remove fixed height constraints to allow dynamic height based on content
        displayLabel->setMinimumHeight(0);
        displayLabel->setMaximumHeight(QWIDGETSIZE_MAX);
        displayLabel->setContentsMargins(0, 2, 0, 2); // Additional margins inside the label
        
        autoLayout->addWidget(m_displayAfterCheck);
        autoLayout->addWidget(displayLabel);
        
        m_layout->addWidget(autoRow);
        
        // Display delay checkbox with input (separate checkbox)
        auto* delayRow = new QWidget(m_widget);
        auto* delayMainLayout = new QVBoxLayout(delayRow);
        delayMainLayout->setContentsMargins(0, 0, 0, 0);
        delayMainLayout->setSpacing(4);
        
        // Checkbox with no text
        auto* delayCheck = new QCheckBox(delayRow);
        delayCheck->installEventFilter(this);
        
        // Label + input box layout
        auto* delayInputRow = new QWidget(delayRow);
        auto* h = new QHBoxLayout(delayInputRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        
        auto* delayLabel = new QLabel("Display delay:", delayInputRow);
        delayLabel->setStyleSheet("color: white;");
        delayLabel->setWordWrap(true);
        delayLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred); // Allow natural sizing
        
        m_displayAfterBox = makeValueBox();
        auto* suffix = new QLabel(" seconds", delayInputRow);
        suffix->setStyleSheet("color: white;");
        
        h->addWidget(delayLabel);
        h->addWidget(m_displayAfterBox);
        h->addWidget(suffix);
        h->addStretch();
        
        delayMainLayout->addWidget(delayCheck);
        delayMainLayout->addWidget(delayInputRow);
        
        m_layout->addWidget(delayRow);
    }

    // 1) Play automatically + Play delay as separate checkboxes (video only)
    {
        m_autoPlayRow = new QWidget(m_widget);
        auto* vLayout = new QVBoxLayout(m_autoPlayRow);
        vLayout->setContentsMargins(0, 0, 0, 0);
        vLayout->setSpacing(5);
        
        // Play automatically checkbox with separate label
        auto* autoRow = new QWidget(m_autoPlayRow);
        auto* autoLayout = new QVBoxLayout(autoRow);
        autoLayout->setContentsMargins(0, 0, 0, 0);
        autoLayout->setSpacing(4);
        
        m_autoPlayCheck = new QCheckBox(autoRow); // Empty checkbox text
        m_autoPlayCheck->installEventFilter(this);
        
        auto* playLabel = new QLabel("Play automatically", autoRow);
        playLabel->setStyleSheet("color: white;");
        playLabel->setWordWrap(true);
        playLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred); // Allow natural sizing
        
        autoLayout->addWidget(m_autoPlayCheck);
        autoLayout->addWidget(playLabel);
        
        vLayout->addWidget(autoRow);
        
        // Play delay checkbox with input
        auto* delayRow = new QWidget(m_autoPlayRow);
        auto* delayMainLayout = new QVBoxLayout(delayRow);
        delayMainLayout->setContentsMargins(0, 0, 0, 0);
        delayMainLayout->setSpacing(4);
        
        // Checkbox with no text
        m_playDelayCheck = new QCheckBox(delayRow);
        m_playDelayCheck->installEventFilter(this);
        
        // Label + input box layout
        auto* delayInputRow = new QWidget(delayRow);
        auto* h = new QHBoxLayout(delayInputRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        
        auto* playDelayLabel = new QLabel("Play delay:", delayInputRow);
        playDelayLabel->setStyleSheet("color: white;");
        playDelayLabel->setWordWrap(true);
        playDelayLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred); // Allow natural sizing
        
        m_autoPlayBox = makeValueBox();
        auto* suffix = new QLabel(" seconds", delayInputRow);
        suffix->setStyleSheet("color: white;");
        
        h->addWidget(playDelayLabel);
        h->addWidget(m_autoPlayBox);
        h->addWidget(suffix);
        h->addStretch();
        
        delayMainLayout->addWidget(m_playDelayCheck);
        delayMainLayout->addWidget(delayInputRow);
        
        vLayout->addWidget(delayRow);
        
        m_layout->addWidget(m_autoPlayRow);
    }

    // 2) Repeat (video only) - using separate checkbox and label
    {
        m_repeatRow = new QWidget(m_widget);
        auto* mainLayout = new QVBoxLayout(m_repeatRow);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(4);
        
        // Checkbox with no text
        m_repeatCheck = new QCheckBox(m_repeatRow);
        m_repeatCheck->installEventFilter(this);
        
        // Label + input box layout
        auto* inputRow = new QWidget(m_repeatRow);
        auto* h = new QHBoxLayout(inputRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        
        auto* repeatLabel = new QLabel("Repeat", inputRow);
        repeatLabel->setStyleSheet("color: white;");
        repeatLabel->setWordWrap(true);
        repeatLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred); // Allow natural sizing
        
        m_repeatBox = makeValueBox();
        auto* suffix = new QLabel(" times", inputRow);
        suffix->setStyleSheet("color: white;");
        
        h->addWidget(repeatLabel);
        h->addWidget(m_repeatBox);
        h->addWidget(suffix);
        h->addStretch();
        
        mainLayout->addWidget(m_repeatCheck);
        mainLayout->addWidget(inputRow);
        
        m_layout->addWidget(m_repeatRow);
    }

    // 3) Fade in with separate checkbox and label
    {
        auto* row = new QWidget(m_widget);
        auto* mainLayout = new QVBoxLayout(row);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(4);
        
        // Checkbox with no text
        m_fadeInCheck = new QCheckBox(row);
        m_fadeInCheck->installEventFilter(this);
        
        // Label + input box layout
        auto* inputRow = new QWidget(row);
        auto* h = new QHBoxLayout(inputRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        
        auto* fadeInLabel = new QLabel("Fade in:", inputRow);
        fadeInLabel->setStyleSheet("color: white;");
        fadeInLabel->setWordWrap(true);
        fadeInLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred); // Allow natural sizing
        
        m_fadeInBox = makeValueBox();
        auto* suffix = new QLabel(" seconds", inputRow);
        suffix->setStyleSheet("color: white;");
        
        h->addWidget(fadeInLabel);
        h->addWidget(m_fadeInBox);
        h->addWidget(suffix);
        h->addStretch();
        
        mainLayout->addWidget(m_fadeInCheck);
        mainLayout->addWidget(inputRow);
        
        m_layout->addWidget(row);
    }

    // 4) Fade out with separate checkbox and label
    {
        auto* row = new QWidget(m_widget);
        auto* mainLayout = new QVBoxLayout(row);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(4);
        
        // Checkbox with no text
        m_fadeOutCheck = new QCheckBox(row);
        m_fadeOutCheck->installEventFilter(this);
        
        // Label + input box layout
        auto* inputRow = new QWidget(row);
        auto* h = new QHBoxLayout(inputRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        
        auto* fadeOutLabel = new QLabel("Fade out:", inputRow);
        fadeOutLabel->setStyleSheet("color: white;");
        fadeOutLabel->setWordWrap(true);
        fadeOutLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred); // Allow natural sizing
        
        m_fadeOutBox = makeValueBox();
        auto* suffix = new QLabel(" seconds", inputRow);
        suffix->setStyleSheet("color: white;");
        
        h->addWidget(fadeOutLabel);
        h->addWidget(m_fadeOutBox);
        h->addWidget(suffix);
        h->addStretch();
        
        mainLayout->addWidget(m_fadeOutCheck);
        mainLayout->addWidget(inputRow);
        
        m_layout->addWidget(row);
    }

    // 5) Opacity with separate checkbox and label
    {
        auto* row = new QWidget(m_widget);
        auto* mainLayout = new QVBoxLayout(row);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(4);
        
        // Checkbox with no text
        m_opacityCheck = new QCheckBox(row);
        m_opacityCheck->installEventFilter(this);
        
        // Label + input box layout
        auto* inputRow = new QWidget(row);
        auto* h = new QHBoxLayout(inputRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        
        auto* opacityLabel = new QLabel("Opacity:", inputRow);
        opacityLabel->setStyleSheet("color: white;");
        opacityLabel->setWordWrap(true);
        opacityLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred); // Allow natural sizing
        
        m_opacityBox = makeValueBox(QStringLiteral("100")); // Default to 100%
        auto* suffix = new QLabel("%", inputRow);
        suffix->setStyleSheet("color: white;");
        
        h->addWidget(opacityLabel);
        h->addWidget(m_opacityBox);
        h->addWidget(suffix);
        h->addStretch();
        
        mainLayout->addWidget(m_opacityCheck);
        mainLayout->addWidget(inputRow);
        
        m_layout->addWidget(row);
    }

    // Scene-drawn rounded background behind the widget, matching overlay style
    m_bgRect = new MouseBlockingRoundedRectItem();
    m_bgRect->setRadius(gOverlayCornerRadiusPx);
    applyOverlayBorder(m_bgRect);
    m_bgRect->setBrush(QBrush(AppColors::gOverlayBackgroundColor));
    m_bgRect->setZValue(12009.5); // just below proxy
    m_bgRect->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_bgRect->setData(0, QStringLiteral("overlay"));

    m_proxy = new QGraphicsProxyWidget();
    m_proxy->setWidget(m_widget);
    m_proxy->setZValue(12010.0); // above overlays
    m_proxy->setOpacity(1.0);
    // Ignore view scaling (keep absolute pixel size)
    m_proxy->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    // Prevent clipping of content
    m_proxy->setFlag(QGraphicsItem::ItemClipsToShape, false);
    m_proxy->setFlag(QGraphicsItem::ItemClipsChildrenToShape, false);
    // Ensure the panel receives mouse events and is treated as an overlay by the canvas
    m_proxy->setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    m_proxy->setAcceptHoverEvents(true);
    m_proxy->setData(0, QStringLiteral("overlay"));
    
    // Remove any size constraints from proxy - let it follow widget size naturally
    m_proxy->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    m_proxy->setMinimumSize(0, 0);
    
    // Also make underlying widget track mouse (not strictly required for click blocking)
    m_widget->setMouseTracking(true);
    
    // Install event filter on the main widget to catch clicks elsewhere
    m_widget->installEventFilter(this);
    
    // Initial geometry update to ensure proper sizing
    updatePanelGeometry();
}

void MediaSettingsPanel::ensureInScene(QGraphicsScene* scene) {
    if (!scene || !m_proxy) return;
    if (m_bgRect && !m_bgRect->scene()) scene->addItem(m_bgRect);
    if (!m_proxy->scene()) scene->addItem(m_proxy);
}

void MediaSettingsPanel::setVisible(bool visible) {
    if (!m_proxy) return;
    m_proxy->setVisible(visible);
    if (m_bgRect) m_bgRect->setVisible(visible);
    
    // Clear active box when hiding the panel
    if (!visible) {
        clearActiveBox();
    }
}

bool MediaSettingsPanel::isVisible() const {
    return m_proxy && m_proxy->isVisible();
}

void MediaSettingsPanel::setMediaType(bool isVideo) {
    // Show/hide video-only options
    if (m_autoPlayRow) {
        m_autoPlayRow->setVisible(isVideo);
    }
    if (m_repeatRow) {
        m_repeatRow->setVisible(isVideo);
    }
    
    // Clear active box if it belongs to a hidden video-only option
    if (!isVideo && m_activeBox && (m_activeBox == m_autoPlayBox || m_activeBox == m_repeatBox)) {
        clearActiveBox();
    }
    
    // Force panel geometry update to recalculate size
    updatePanelGeometry();
}

void MediaSettingsPanel::updatePosition(QGraphicsView* view) {
    if (!view || !m_proxy) return;
    
    const int margin = 16;
    
    // Calculate maximum allowed width (50% of viewport)
    const int maxOverlayW = (view->viewport()->width() - margin*2) / 2;
    
    // Always set width constraints - either the 50% limit or unlimited
    if (maxOverlayW > 0 && maxOverlayW < QWIDGETSIZE_MAX) {
        m_widget->setMaximumWidth(maxOverlayW);
    } else {
        // No effective constraint - let it expand freely
        m_widget->setMaximumWidth(QWIDGETSIZE_MAX);
    }
    m_widget->setMinimumWidth(200); // Reasonable minimum
    
    // Critical: Remove ALL height constraints
    m_widget->setMaximumHeight(QWIDGETSIZE_MAX);
    m_widget->setMinimumHeight(0);
    
    // Set size policy to expand vertically as needed
    m_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
    
    // Force a geometry update after changing width constraints
    updatePanelGeometry();
    
    // Position at left edge of viewport
    QPointF topLeftVp(margin, margin);
    QPointF topLeftScene = view->viewportTransform().inverted().map(topLeftVp);
    m_proxy->setPos(topLeftScene);
    
    // Let the background rect follow the proxy's natural size
    if (m_bgRect) {
        m_bgRect->setPos(topLeftScene);
        // The background will be updated when the proxy resizes naturally
    }
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
        setBoxActive(m_activeBox, false);
        m_activeBox = nullptr;
        // Reset first-type-clears flag when deactivating
        m_clearOnFirstType = false;
    }
}

bool MediaSettingsPanel::eventFilter(QObject* obj, QEvent* event) {
    // Handle widget resize events to update proxy and background
    if (obj == m_widget && event->type() == QEvent::Resize) {
        if (m_proxy && m_widget) {
            QSize widgetSize = m_widget->size();
            m_proxy->resize(widgetSize);
            // Update background rect to match
            if (m_bgRect) {
                m_bgRect->setRect(0, 0, widgetSize.width(), widgetSize.height());
            }
        }
        return false; // Continue normal processing
    }
    
    // Handle clicks on value boxes
    if (event->type() == QEvent::MouseButtonPress) {
        QLabel* box = qobject_cast<QLabel*>(obj);
        if (box && (box == m_displayAfterBox || box == m_autoPlayBox || box == m_repeatBox || 
                   box == m_fadeInBox || box == m_fadeOutBox || box == m_opacityBox)) {
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
            updatePanelGeometry();
            return true;
        }
        // 'i' key sets infinity symbol
        else if (keyEvent->key() == Qt::Key_I) {
            m_activeBox->setText("∞");
            updatePanelGeometry();
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
                
                // Update panel geometry when text content changes
                updatePanelGeometry();
                
                return true;
            }
        }
    }
    
    return QObject::eventFilter(obj, event);
}

void MediaSettingsPanel::updatePanelGeometry() {
    if (!m_widget || !m_layout) return;
    
    // Force the layout to recalculate based on new content
    m_layout->invalidate();
    m_layout->activate();
    
    // Get the current width constraint
    int currentWidth = m_widget->maximumWidth();
    if (currentWidth == QWIDGETSIZE_MAX) {
        // No width constraint - let widget choose its preferred width
        m_widget->adjustSize();
    } else {
        // Width is constrained - calculate height for this specific width
        m_widget->resize(currentWidth, 0); // Set width, let height calculate
        
        // Use heightForWidth if the layout supports it
        int heightForWidth = m_layout->heightForWidth(currentWidth);
        if (heightForWidth > 0) {
            m_widget->resize(currentWidth, heightForWidth);
        } else {
            // Fallback to adjustSize
            m_widget->adjustSize();
        }
    }
    
    // Directly update proxy and background to match widget size
    if (m_proxy && m_widget) {
        QSize widgetSize = m_widget->size();
        m_proxy->resize(widgetSize);
        
        // Update background rect to match
        if (m_bgRect) {
            m_bgRect->setRect(0, 0, widgetSize.width(), widgetSize.height());
        }
    }
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

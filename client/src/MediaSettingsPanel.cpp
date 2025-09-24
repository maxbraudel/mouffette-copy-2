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
    m_layout->setContentsMargins(20, 16, 20, 16);
    m_layout->setSpacing(10);
    // Prevent layout from stretching when items are hidden
    m_layout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    
    // Set a minimum width for the settings panel to make it wider
    m_widget->setMinimumWidth(380);

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
        // Display automatically checkbox with proper alignment
        auto* autoRow = new QWidget(m_widget);
        auto* autoLayout = new QHBoxLayout(autoRow);
        autoLayout->setContentsMargins(0, 0, 0, 0);
        autoLayout->setSpacing(0);
        m_displayAfterCheck = new QCheckBox("Display automatically", autoRow);
        m_displayAfterCheck->setStyleSheet("color: white;");
        m_displayAfterCheck->installEventFilter(this);
        autoLayout->addWidget(m_displayAfterCheck);
        autoLayout->addStretch();
        m_layout->addWidget(autoRow);
        
        // Display delay checkbox with input (separate checkbox)
        auto* delayRow = new QWidget(m_widget);
        auto* h = new QHBoxLayout(delayRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        auto* delayCheck = new QCheckBox("Display delay: ", delayRow);
        delayCheck->setStyleSheet("color: white;");
        delayCheck->installEventFilter(this);
        m_displayAfterBox = makeValueBox();
        auto* suffix = new QLabel(" seconds", delayRow);
        suffix->setStyleSheet("color: white;");
        h->addWidget(delayCheck);
        h->addWidget(m_displayAfterBox);
        h->addWidget(suffix);
        h->addStretch();
        m_layout->addWidget(delayRow);
    }

    // 1) Play automatically + Play delay as separate checkboxes (video only)
    {
        m_autoPlayRow = new QWidget(m_widget);
        auto* vLayout = new QVBoxLayout(m_autoPlayRow);
        vLayout->setContentsMargins(0, 0, 0, 0);
        vLayout->setSpacing(5);
        
        // Play automatically checkbox
        auto* autoRow = new QWidget(m_autoPlayRow);
        auto* autoLayout = new QHBoxLayout(autoRow);
        autoLayout->setContentsMargins(0, 0, 0, 0);
        autoLayout->setSpacing(0);
        m_autoPlayCheck = new QCheckBox("Play automatically", autoRow);
        m_autoPlayCheck->setStyleSheet("color: white;");
        m_autoPlayCheck->installEventFilter(this);
        autoLayout->addWidget(m_autoPlayCheck);
        autoLayout->addStretch();
        vLayout->addWidget(autoRow);
        
        // Play delay checkbox with input
        auto* delayRow = new QWidget(m_autoPlayRow);
        auto* h = new QHBoxLayout(delayRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        m_playDelayCheck = new QCheckBox("Play delay: ", delayRow);
        m_playDelayCheck->setStyleSheet("color: white;");
        m_playDelayCheck->installEventFilter(this);
        m_autoPlayBox = makeValueBox();
        auto* suffix = new QLabel(" seconds", delayRow);
        suffix->setStyleSheet("color: white;");
        h->addWidget(m_playDelayCheck);
        h->addWidget(m_autoPlayBox);
        h->addWidget(suffix);
        h->addStretch();
        vLayout->addWidget(delayRow);
        
        m_layout->addWidget(m_autoPlayRow);
    }

    // 2) Repeat (video only) - keeping original single checkbox format
    {
        m_repeatRow = new QWidget(m_widget);
        auto* h = new QHBoxLayout(m_repeatRow);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(0);
        m_repeatCheck = new QCheckBox("Repeat ", m_repeatRow);
        m_repeatCheck->setStyleSheet("color: white;");
        m_repeatCheck->installEventFilter(this);
        m_repeatBox = makeValueBox();
        auto* suffix = new QLabel(" times", m_repeatRow);
        suffix->setStyleSheet("color: white;");
        h->addWidget(m_repeatCheck);
        h->addWidget(m_repeatBox);
        h->addWidget(suffix);
        h->addStretch();
        m_layout->addWidget(m_repeatRow);
    }

    // 3) Fade in with checkbox format
    {
        auto* row = new QWidget(m_widget);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(0);
        m_fadeInCheck = new QCheckBox("Fade in: ", row);
        m_fadeInCheck->setStyleSheet("color: white;");
        m_fadeInCheck->installEventFilter(this);
        m_fadeInBox = makeValueBox();
        auto* suffix = new QLabel(" seconds", row);
        suffix->setStyleSheet("color: white;");
        h->addWidget(m_fadeInCheck);
        h->addWidget(m_fadeInBox);
        h->addWidget(suffix);
        h->addStretch();
        m_layout->addWidget(row);
    }

    // 4) Fade out with checkbox format
    {
        auto* row = new QWidget(m_widget);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(0);
        m_fadeOutCheck = new QCheckBox("Fade out: ", row);
        m_fadeOutCheck->setStyleSheet("color: white;");
        m_fadeOutCheck->installEventFilter(this);
        m_fadeOutBox = makeValueBox();
        auto* suffix = new QLabel(" seconds", row);
        suffix->setStyleSheet("color: white;");
        h->addWidget(m_fadeOutCheck);
        h->addWidget(m_fadeOutBox);
        h->addWidget(suffix);
        h->addStretch();
        m_layout->addWidget(row);
    }

    // 5) Opacity with checkbox format
    {
        auto* row = new QWidget(m_widget);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(0);
        m_opacityCheck = new QCheckBox("Opacity: ", row);
        m_opacityCheck->setStyleSheet("color: white;");
        m_opacityCheck->installEventFilter(this);
        m_opacityBox = makeValueBox(QStringLiteral("100")); // Default to 100%
        auto* suffix = new QLabel("%", row);
        suffix->setStyleSheet("color: white;");
        h->addWidget(m_opacityCheck);
        h->addWidget(m_opacityBox);
        h->addWidget(suffix);
        h->addStretch();
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
    // Ensure the panel receives mouse events and is treated as an overlay by the canvas
    m_proxy->setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    m_proxy->setAcceptHoverEvents(true);
    m_proxy->setData(0, QStringLiteral("overlay"));
    // Also make underlying widget track mouse (not strictly required for click blocking)
    m_widget->setMouseTracking(true);
    
    // Install event filter on the main widget to catch clicks elsewhere
    m_widget->installEventFilter(this);
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
    
    // Force layout update to recalculate size
    if (m_widget && m_layout) {
        // Force layout to recalculate
        m_layout->invalidate();
        m_layout->activate();
        
        // Update widget geometry
        m_widget->updateGeometry();
        m_widget->adjustSize();
        
        // Update proxy widget size to match widget's preferred size
        if (m_proxy) {
            QSize preferredSize = m_widget->sizeHint();
            m_proxy->resize(preferredSize);
            
            // Update background rect to match proxy size
            if (m_bgRect) {
                m_bgRect->setRect(0, 0, preferredSize.width(), preferredSize.height());
            }
        }
    }
}

void MediaSettingsPanel::updatePosition(QGraphicsView* view) {
    if (!view || !m_proxy) return;
    const int margin = 16;
    // Position at left edge of viewport, vertically near top with margin
    QPointF topLeftVp(margin, margin);
    QPointF topLeftScene = view->viewportTransform().inverted().map(topLeftVp);
    m_proxy->setPos(topLeftScene);
    // Match background rect to proxy widget geometry
    if (m_bgRect) {
        m_bgRect->setPos(topLeftScene);
        const QSizeF s = m_proxy->size();
        m_bgRect->setRect(0, 0, s.width(), s.height());
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
            return true;
        }
        // 'i' key sets infinity symbol
        else if (keyEvent->key() == Qt::Key_I) {
            m_activeBox->setText("∞");
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

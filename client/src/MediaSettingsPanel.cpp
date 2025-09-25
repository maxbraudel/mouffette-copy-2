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
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
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
    // Ensure overlay blocks mouse events to canvas behind it (like media overlay)
    m_widget->setAttribute(Qt::WA_NoMousePropagation, true);
    // Apply unified font size to match media filename overlay (OverlayTextElement uses 16px)
    m_widget->setStyleSheet("background-color: transparent; color: white; font-size: 16px;");
    m_widget->setAutoFillBackground(false);

    // Root layout on widget
    m_rootLayout = new QVBoxLayout(m_widget);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);
    m_rootLayout->setSizeConstraint(QLayout::SetNoConstraint);

    // Set fixed width of the overlay panel
    m_widget->setFixedWidth(221);

    // Scroll area for inner content (mirror media overlay)
    m_scrollArea = new QScrollArea(m_widget);
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
    m_rootLayout->addWidget(m_scrollArea);

    // Inner content widget inside scroll area (with margins like before)
    m_innerContent = new QWidget(m_scrollArea);
    m_innerContent->setAttribute(Qt::WA_StyledBackground, true);
    m_innerContent->setAttribute(Qt::WA_NoMousePropagation, true);
    m_innerContent->setStyleSheet("background-color: transparent; color: white; font-size: 16px;");
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
    m_title->setStyleSheet("color: white;");
    m_contentLayout->addWidget(m_title);
    
    // Add extra spacing after the title
    m_contentLayout->addSpacing(15);

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
    m_contentLayout->addWidget(autoRow);
        
        // Display delay checkbox with input (separate checkbox)
        auto* delayRow = new QWidget(m_widget);
        auto* h = new QHBoxLayout(delayRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        m_displayDelayCheck = new QCheckBox("Display delay: ", delayRow);
        m_displayDelayCheck->setStyleSheet("color: white;");
        m_displayDelayCheck->installEventFilter(this);
        m_displayAfterBox = makeValueBox();
        m_displayAfterSecondsLabel = new QLabel("s", delayRow);
        m_displayAfterSecondsLabel->setStyleSheet("color: white;");
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
        m_autoPlayCheck->setStyleSheet("color: white;");
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
        m_playDelayCheck->setStyleSheet("color: white;");
        m_playDelayCheck->installEventFilter(this);
        m_autoPlayBox = makeValueBox();
        m_autoPlaySecondsLabel = new QLabel("s", m_playDelayRow);
        m_autoPlaySecondsLabel->setStyleSheet("color: white;");
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
        m_repeatCheck->setStyleSheet("color: white;");
        m_repeatCheck->installEventFilter(this);
        m_repeatBox = makeValueBox();
        auto* suffix = new QLabel(" times", m_repeatRow);
        suffix->setStyleSheet("color: white;");
        h->addWidget(m_repeatCheck);
        h->addWidget(m_repeatBox);
        h->addWidget(suffix);
        h->addStretch();
    m_contentLayout->addWidget(m_repeatRow);
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
        auto* suffix = new QLabel("s", row);
        suffix->setStyleSheet("color: white;");
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
        m_fadeOutCheck->setStyleSheet("color: white;");
        m_fadeOutCheck->installEventFilter(this);
        m_fadeOutBox = makeValueBox();
        auto* suffix = new QLabel("s", row);
        suffix->setStyleSheet("color: white;");
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
        m_opacityCheck->setStyleSheet("color: white;");
        m_opacityCheck->installEventFilter(this);
        m_opacityBox = makeValueBox(QStringLiteral("100")); // Default to 100%
        auto* suffix = new QLabel("%", row);
        suffix->setStyleSheet("color: white;");
        h->addWidget(m_opacityCheck);
        h->addWidget(m_opacityBox);
        h->addWidget(suffix);
        h->addStretch();
    m_contentLayout->addWidget(row);
    }

    // Scene-drawn rounded background behind the widget, matching overlay style
    m_bgRect = new MouseBlockingRoundedRectItem();
    m_bgRect->setRadius(gOverlayCornerRadiusPx);
    applyOverlayBorder(m_bgRect);
    m_bgRect->setBrush(QBrush(AppColors::gOverlayBackgroundColor));
    m_bgRect->setZValue(12009.5); // just below proxy
    m_bgRect->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_bgRect->setData(0, QStringLiteral("blocking-overlay"));
    
    // Set initial size for background rect
    QSize initialSize = m_widget->sizeHint();
    initialSize.setWidth(221);
    m_bgRect->setRect(0, 0, initialSize.width(), initialSize.height());

    m_proxy = new QGraphicsProxyWidget();
    m_proxy->setWidget(m_widget);
    m_proxy->setZValue(12010.0); // above overlays
    m_proxy->setOpacity(1.0);
    // Ignore view scaling (keep absolute pixel size)
    m_proxy->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    // Ensure the panel receives mouse events and is treated as a blocking overlay by the canvas
    m_proxy->setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    m_proxy->setAcceptHoverEvents(true);
    m_proxy->setData(0, QStringLiteral("blocking-overlay"));
    
    // Force the proxy to use our fixed width (height will be set in updatePosition)
    QSize fixedSize = m_widget->sizeHint();
    fixedSize.setWidth(221);
    m_proxy->resize(fixedSize);
    // Also make underlying widget track mouse (not strictly required for click blocking)
    m_widget->setMouseTracking(true);
    
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
        
        // Update proxy widget size to match widget's preferred size but enforce fixed width
        if (m_proxy) {
            QSize preferredSize = m_widget->sizeHint();
            preferredSize.setWidth(221); // Enforce our fixed width
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

    // Clamp panel height to viewport height; enable overflow scroll when needed
    if (view) {
        const int margin = 16;
        int maxHeight = view->viewport()->height() - 2*margin;
        maxHeight = qMax(50, maxHeight); // safety
        // Compute desired content height based on inner content hint
        int contentH = m_innerContent ? m_innerContent->sizeHint().height() + 0 : 0;
        int panelH = qMin(maxHeight, contentH);
        QSize s = m_proxy->size().toSize();
        s.setHeight(panelH);
        m_proxy->resize(s);
        if (m_bgRect) m_bgRect->setRect(0, 0, s.width(), s.height());
        updateScrollbarGeometry();
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
    // Block all mouse interactions from reaching canvas when over settings panel
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonRelease ||
        event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonDblClick) {
        // Accept and consume these events to prevent canvas panning/interaction
        if (obj == m_widget || obj == m_innerContent || obj == m_scrollArea ||
            (obj->isWidgetType() && static_cast<QWidget*>(obj)->isAncestorOf(m_widget))) {
            // Let normal widget handling occur but prevent propagation to canvas
            return false; // Allow widget to handle, but WA_NoMousePropagation will block further propagation
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
    // Consume wheel events over the settings panel so canvas doesn't zoom/scroll
    else if (event->type() == QEvent::Wheel) {
        // Only handle if event is within our widget tree
        if (obj == m_widget || (m_innerContent && obj->isWidgetType())) {
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

void MediaSettingsPanel::updateScrollbarGeometry() {
    if (!m_overlayVScroll || !m_proxy) return;
    const QRectF r = m_proxy->boundingRect();
    const int margin = 6; // small inset from edge
    const int width = 8;
    const int top = 6;
    const int bottom = 6;
    const int height = qMax(0, (int)r.height() - top - bottom);
    const int x = (int)r.width() - width - margin;
    const int y = top;
    m_overlayVScroll->setGeometry(x, y, width, height);
    // Update range visibility depending on content - but don't auto-show, only show on interaction
    if (auto* v = m_scrollArea ? m_scrollArea->verticalScrollBar() : nullptr) {
        const bool needed = v->maximum() > v->minimum();
        // Only hide if not needed, but don't auto-show when needed (wait for user interaction)
        if (!needed) {
            m_overlayVScroll->hide();
        }
        // Ensure pageStep is synchronized for proper thumb sizing
        if (m_overlayVScroll) {
            m_overlayVScroll->setPageStep(v->pageStep());
        }
    }
}

void MediaSettingsPanel::onDisplayAutomaticallyToggled(bool checked) {
    // Enable/disable display delay checkbox and input box based on display automatically state
    if (m_displayDelayCheck) {
        m_displayDelayCheck->setEnabled(checked);
        
        // Update visual styling for disabled state
        if (checked) {
            m_displayDelayCheck->setStyleSheet("color: white;");
        } else {
            m_displayDelayCheck->setStyleSheet("color: #808080;"); // Gray color for disabled
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
            m_displayAfterSecondsLabel->setStyleSheet("color: white;");
        } else {
            m_displayAfterSecondsLabel->setStyleSheet("color: #808080;"); // Gray color for disabled
        }
    }
}

void MediaSettingsPanel::onPlayAutomaticallyToggled(bool checked) {
    // Enable/disable play delay checkbox and input box based on play automatically state
    if (m_playDelayCheck) {
        m_playDelayCheck->setEnabled(checked);
        
        // Update visual styling for disabled state
        if (checked) {
            m_playDelayCheck->setStyleSheet("color: white;");
        } else {
            m_playDelayCheck->setStyleSheet("color: #808080;"); // Gray color for disabled
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
            m_autoPlaySecondsLabel->setStyleSheet("color: white;");
        } else {
            m_autoPlaySecondsLabel->setStyleSheet("color: #808080;"); // Gray color for disabled
        }
    }
}

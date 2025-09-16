#include "MediaSettingsPanel.h"

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
    
    // Set a minimum width for the settings panel to make it wider
    m_widget->setMinimumWidth(380);

    m_title = new QLabel("Scene options");
    QFont tf = m_title->font();
    tf.setBold(true); // keep bold, but do not change size beyond 16px baseline
    m_title->setFont(tf);
    m_title->setStyleSheet("color: white;");
    m_layout->addWidget(m_title);

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

    // 1) Play automatically after [1] seconds
    {
        auto* row = new QWidget(m_widget);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(0);
        m_autoPlayCheck = new QCheckBox("Play automatically after ", row);
        m_autoPlayCheck->setStyleSheet("color: white;");
        m_autoPlayCheck->installEventFilter(this);
        m_autoPlayBox = makeValueBox();
        auto* suffix = new QLabel(" seconds", row);
        suffix->setStyleSheet("color: white;");
        h->addWidget(m_autoPlayCheck);
        h->addWidget(m_autoPlayBox);
        h->addWidget(suffix);
        h->addStretch();
        m_layout->addWidget(row);
    }

    // 2) Repeat [1] time
    {
        auto* row = new QWidget(m_widget);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(0);
        m_repeatCheck = new QCheckBox("Repeat ", row);
        m_repeatCheck->setStyleSheet("color: white;");
        m_repeatCheck->installEventFilter(this);
        m_repeatBox = makeValueBox();
        auto* suffix = new QLabel(" time", row);
        suffix->setStyleSheet("color: white;");
        h->addWidget(m_repeatCheck);
        h->addWidget(m_repeatBox);
        h->addWidget(suffix);
        h->addStretch();
        m_layout->addWidget(row);
    }

    // 3) Fade in during [1] seconds
    {
        auto* row = new QWidget(m_widget);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(0);
        m_fadeInCheck = new QCheckBox("Fade in during ", row);
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

    // 4) Fade out during [1] seconds
    {
        auto* row = new QWidget(m_widget);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(0);
        m_fadeOutCheck = new QCheckBox("Fade out during ", row);
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

    // Scene-drawn rounded background behind the widget, matching overlay style
    m_bgRect = new MouseBlockingRoundedRectItem();
    m_bgRect->setRadius(gOverlayCornerRadiusPx);
    m_bgRect->setPen(Qt::NoPen);
    m_bgRect->setBrush(QBrush(gOverlayBackgroundColor));
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
            "QLabel {"
            "  background-color: rgb(74,144,226);"
            "  border: 1px solid rgb(200,200,200);"
            "  border-radius: 6px;"
            "  padding: 2px 10px;"
            "  margin-left: 4px;"
            "  margin-right: 0px;"
            "  color: white;"
            "}"
        );
    } else {
        // Inactive state: subtle translucent box with rounded corners; white text to match panel
        box->setStyleSheet(
            "QLabel {"
            "  background-color: rgb(60,60,60);"
            "  border: 1px solid rgb(200,200,200);"
            "  border-radius: 6px;"
            "  padding: 2px 10px;"
            "  margin-left: 4px;"
            "  margin-right: 0px;"
            "  color: white;"
            "}"
        );
    }
}

void MediaSettingsPanel::clearActiveBox() {
    if (m_activeBox) {
        setBoxActive(m_activeBox, false);
        m_activeBox = nullptr;
    }
}

bool MediaSettingsPanel::eventFilter(QObject* obj, QEvent* event) {
    // Handle clicks on value boxes
    if (event->type() == QEvent::MouseButtonPress) {
        QLabel* box = qobject_cast<QLabel*>(obj);
        if (box && (box == m_autoPlayBox || box == m_repeatBox || 
                   box == m_fadeInBox || box == m_fadeOutBox)) {
            // Clear previous active box
            clearActiveBox();
            // Set this box as active
            m_activeBox = box;
            setBoxActive(box, true);
            // Give focus to the box so it can receive key events
            box->setFocus();
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
                
                // If current text is "..." (cleared state) or infinity symbol, replace it
                if (currentText == "..." || currentText == "∞") {
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
    if (box == m_autoPlayBox) {
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
    
    return false; // Unknown box, reject input
}

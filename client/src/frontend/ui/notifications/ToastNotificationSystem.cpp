#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include <QPainter>
#include <QApplication>

// Static instance for singleton pattern
ToastNotificationSystem* ToastNotificationSystem::s_instance = nullptr;

// ToastNotification Implementation
ToastNotification::ToastNotification(const QString& text, Type type, QWidget* parent)
    : QWidget(parent)
    , m_text(text)
    , m_type(type)
    , m_duration(DEFAULT_DURATION)
    , m_textLabel(nullptr)
    , m_timer(new QTimer(this))
    , m_fadeAnimation(nullptr)
    , m_slideAnimation(nullptr)
    , m_opacityEffect(nullptr)
    , m_isShowing(false)
    , m_isHiding(false)
{
    // Set up the widget
    setWindowFlags(Qt::Widget);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    
    // Get default style for the notification type
    m_style = getDefaultStyleForType(type);
    
    // Create the text label
    m_textLabel = new QLabel(text, this);
    m_textLabel->setWordWrap(true);
    m_textLabel->setStyleSheet(QString(
        "QLabel { color: %1; background: transparent; padding: 10px 14px; font-size: 13px; font-weight: bold; }"
    ).arg(m_style.textColor.name()));
    
    // Layout
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_textLabel);
    
    // Set up opacity effect
    m_opacityEffect = new QGraphicsOpacityEffect(this);
    setGraphicsEffect(m_opacityEffect);
    m_opacityEffect->setOpacity(0.0);
    
    // Set up timer
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &ToastNotification::onTimerExpired);
    
    // Calculate size
    adjustSize();
    
    setupAnimation();
}

ToastNotification::~ToastNotification()
{
    if (m_fadeAnimation) {
        m_fadeAnimation->stop();
    }
    if (m_slideAnimation) {
        m_slideAnimation->stop();
    }
}

void ToastNotification::setDuration(int milliseconds)
{
    m_duration = milliseconds;
}

void ToastNotification::setStyle(const Style& style)
{
    m_style = style;
    if (m_textLabel) {
        m_textLabel->setStyleSheet(QString(
            "QLabel { color: %1; background: transparent; padding: 10px 14px; font-size: 13px; font-weight: bold; }"
        ).arg(style.textColor.name()));
        m_textLabel->adjustSize();
    }
    adjustSize();
    update();
}

void ToastNotification::show()
{
    if (m_isShowing || m_isHiding) return;
    
    // Don't call QWidget::show() yet - we'll show after positioning
    startFadeIn();
}

void ToastNotification::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Inset rect by half border width for crisp stroke
    const int bw = qMax(1, m_style.borderWidth);
    QRectF r = rect();
    const qreal half = bw / 2.0;
    r.adjust(half, half, -half, -half);

    // Fill base with window background color, then overlay tinted background
    p.setPen(Qt::NoPen);
    const QColor base = AppColors::getCurrentColor(AppColors::gWindowBackgroundColorSource);
    p.setBrush(base);
    p.drawRoundedRect(r, m_style.borderRadius, m_style.borderRadius);

    p.setBrush(m_style.backgroundColor);
    p.drawRoundedRect(r, m_style.borderRadius, m_style.borderRadius);

    // Border
    QPen pen(m_style.borderColor, bw);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, m_style.borderRadius, m_style.borderRadius);
}

void ToastNotification::setupAnimation()
{
    // Fade animation
    m_fadeAnimation = new QPropertyAnimation(m_opacityEffect, "opacity", this);
    m_fadeAnimation->setDuration(FADE_DURATION);
    m_fadeAnimation->setEasingCurve(QEasingCurve::OutQuad);
    
    // Slide animation
    m_slideAnimation = new QPropertyAnimation(this, "pos", this);
    m_slideAnimation->setDuration(FADE_DURATION);
    m_slideAnimation->setEasingCurve(QEasingCurve::OutQuad);
}

void ToastNotification::startFadeIn()
{
    if (m_isShowing) return;
    m_isShowing = true;
    
    // Get the final position from parent (should be set by ToastNotificationSystem)
    QPoint finalPos = pos();
    
    // Set initial position (slightly lower for upward slide)
    QPoint startPos = finalPos + QPoint(0, SLIDE_DISTANCE);
    move(startPos);
    
    // Now show the widget
    QWidget::show();
    
    // Animate opacity
    m_fadeAnimation->setStartValue(0.0);
    m_fadeAnimation->setEndValue(1.0);
    connect(m_fadeAnimation, &QPropertyAnimation::finished, 
            this, &ToastNotification::onFadeInFinished, Qt::UniqueConnection);
    m_fadeAnimation->start();
    
    // Animate position (slide up)
    m_slideAnimation->setStartValue(startPos);
    m_slideAnimation->setEndValue(finalPos);
    m_slideAnimation->start();
}

void ToastNotification::startFadeOut()
{
    if (m_isHiding) return;
    m_isHiding = true;
    
    // Stop the display timer
    m_timer->stop();
    
    // Animate opacity
    m_fadeAnimation->disconnect(); // Disconnect previous connections
    m_fadeAnimation->setStartValue(1.0);
    m_fadeAnimation->setEndValue(0.0);
    connect(m_fadeAnimation, &QPropertyAnimation::finished, 
            this, &ToastNotification::onFadeOutFinished, Qt::UniqueConnection);
    m_fadeAnimation->start();
    
    // Animate position (slide down slightly)
    QPoint currentPos = pos();
    QPoint endPos = currentPos + QPoint(0, SLIDE_DISTANCE / 2);
    m_slideAnimation->setStartValue(currentPos);
    m_slideAnimation->setEndValue(endPos);
    m_slideAnimation->start();
}

void ToastNotification::onFadeInFinished()
{
    m_isShowing = false;
    
    // Start the display timer
    if (m_duration > 0) {
        m_timer->start(m_duration);
    }
}

void ToastNotification::onTimerExpired()
{
    startFadeOut();
}

void ToastNotification::onFadeOutFinished()
{
    m_isHiding = false;
    emit finished();
    close();
}

ToastNotification::Style ToastNotification::getDefaultStyleForType(Type type) const
{
    Style style;
    // Default style will be overridden by ToastNotificationSystem::Config when shown
    // Keep a neutral translucent background and white text as a fallback
    style.backgroundColor = QColor(0, 0, 0, 64);
    style.textColor = Qt::white;
    
    return style;
}

// ToastNotificationSystem Implementation
ToastNotificationSystem::ToastNotificationSystem(QWidget* parentWindow, QObject* parent)
    : QObject(parent)
    , m_parentWindow(parentWindow)
{
    if (m_parentWindow) {
        m_parentWindow->installEventFilter(this);
    }
}

ToastNotificationSystem::~ToastNotificationSystem()
{
    clearAll();
}

void ToastNotificationSystem::setConfig(const Config& config)
{
    m_config = config;
    repositionNotifications();
}

void ToastNotificationSystem::showSuccess(const QString& message, int duration)
{
    showNotification(message, ToastNotification::Type::Success, duration);
}

void ToastNotificationSystem::showError(const QString& message, int duration)
{
    showNotification(message, ToastNotification::Type::Error, duration);
}

void ToastNotificationSystem::showWarning(const QString& message, int duration)
{
    showNotification(message, ToastNotification::Type::Warning, duration);
}

void ToastNotificationSystem::showInfo(const QString& message, int duration)
{
    showNotification(message, ToastNotification::Type::Info, duration);
}

void ToastNotificationSystem::showLoading(const QString& message, int duration)
{
    showNotification(message, ToastNotification::Type::Loading, duration);
}

void ToastNotificationSystem::showNotification(const QString& message, ToastNotification::Type type, int duration)
{
    if (!m_parentWindow) return;
    
    auto* notification = new ToastNotification(message, type, m_parentWindow);
    
    if (duration > 0) {
        notification->setDuration(duration);
    }
    
    // Apply custom style if configured
    notification->setStyle(getStyleForType(type));
    
    connect(notification, &ToastNotification::finished, 
            this, &ToastNotificationSystem::onNotificationFinished);
    
    showNotificationInternal(notification);
}

void ToastNotificationSystem::showNotificationInternal(ToastNotification* notification)
{
    // Remove oldest notification if we've reached the limit
    removeOldestNotificationIfNeeded();
    
    // Add to active notifications
    m_activeNotifications.append(notification);
    
    // Position the notification BEFORE showing it
    int index = m_activeNotifications.size() - 1;
    QPoint position = calculateNotificationPosition(index);
    notification->move(position);
    
    // Reposition existing notifications to make room
    repositionNotifications();
    
    // Show the notification (this will trigger the animation)
    notification->show();
}

void ToastNotificationSystem::repositionNotifications()
{
    for (int i = 0; i < m_activeNotifications.size(); ++i) {
        QPoint newPos = calculateNotificationPosition(i);
        ToastNotification* notification = m_activeNotifications[i];
        
        // Animate to new position if notification is already visible
        if (notification->isVisible()) {
            auto* posAnimation = new QPropertyAnimation(notification, "pos", this);
            posAnimation->setDuration(m_config.animationDuration);
            posAnimation->setEasingCurve(m_config.easingCurve);
            posAnimation->setStartValue(notification->pos());
            posAnimation->setEndValue(newPos);
            posAnimation->start(QAbstractAnimation::DeleteWhenStopped);
        } else {
            notification->move(newPos);
        }
    }
}

QPoint ToastNotificationSystem::calculateNotificationPosition(int index) const {
    if (!m_parentWindow) return QPoint(0, 0);

    // Use full window size (local coords)
    QRect parentRect = m_parentWindow->geometry();
    parentRect = QRect(0, 0, parentRect.width(), parentRect.height());

    // Bounds check
    if (index < 0 || index >= m_activeNotifications.size()) return QPoint(0, 0);

    // Current toast size
    const int curW = m_activeNotifications[index]->width();
    const int curH = m_activeNotifications[index]->height();

    // X coordinate
    int x = 0;
    switch (m_config.position) {
        case ToastNotification::Position::TopLeft:
        case ToastNotification::Position::BottomLeft:
            x = m_config.marginLeft; // specific left margin for left-anchored toasts
            break;
        case ToastNotification::Position::TopRight:
        case ToastNotification::Position::BottomRight:
            x = parentRect.width() - curW - m_config.marginFromEdge;
            break;
        case ToastNotification::Position::TopCenter:
        case ToastNotification::Position::BottomCenter:
            x = (parentRect.width() - curW) / 2;
            break;
    }

    // Y coordinate
    int y = 0;
    switch (m_config.position) {
        case ToastNotification::Position::TopLeft:
        case ToastNotification::Position::TopRight:
        case ToastNotification::Position::TopCenter:
            // Top positions: stack downward (index 0 at top, new toasts below)
            {
                int prevHeightSum = 0;
                for (int i = 0; i < index && i < m_activeNotifications.size(); ++i) {
                    prevHeightSum += m_activeNotifications[i]->height();
                }
                prevHeightSum += index * m_config.spacing;
                y = m_config.marginFromEdge + prevHeightSum;
            }
            break;
        case ToastNotification::Position::BottomLeft:
        case ToastNotification::Position::BottomRight:
        case ToastNotification::Position::BottomCenter:
            // Bottom positions: stack upward (newest toast always at bottom, older ones pushed up)
            {
                int nextHeightSum = 0;
                for (int i = index + 1; i < m_activeNotifications.size(); ++i) {
                    nextHeightSum += m_activeNotifications[i]->height();
                }
                nextHeightSum += (m_activeNotifications.size() - index - 1) * m_config.spacing;
                y = parentRect.height() - m_config.marginBottom - curH - nextHeightSum;
            }
            break;
    }

    return QPoint(x, y);
}

void ToastNotificationSystem::removeOldestNotificationIfNeeded()
{
    while (m_activeNotifications.size() >= m_config.maxVisibleToasts) {
        if (!m_activeNotifications.isEmpty()) {
            ToastNotification* oldest = m_activeNotifications.takeFirst();
            oldest->close();
            oldest->deleteLater();
        }
    }
}

ToastNotification::Style ToastNotificationSystem::getStyleForType(ToastNotification::Type type) const
{
    switch (type) {
        case ToastNotification::Type::Success:
            return m_config.successStyle;
        case ToastNotification::Type::Error:
            return m_config.errorStyle;
        case ToastNotification::Type::Warning:
            return m_config.warningStyle;
        case ToastNotification::Type::Info:
            return m_config.infoStyle;
        case ToastNotification::Type::Loading:
            return m_config.loadingStyle;
    }
    return ToastNotification::Style();
}

void ToastNotificationSystem::clearAll()
{
    for (auto* notification : m_activeNotifications) {
        notification->close();
        notification->deleteLater();
    }
    m_activeNotifications.clear();
    
    while (!m_pendingNotifications.isEmpty()) {
        auto* notification = m_pendingNotifications.dequeue();
        notification->deleteLater();
    }
}

void ToastNotificationSystem::onNotificationFinished()
{
    auto* notification = qobject_cast<ToastNotification*>(sender());
    if (notification) {
        m_activeNotifications.removeAll(notification);
        repositionNotifications();
        
        // Show pending notifications if any
        if (!m_pendingNotifications.isEmpty() && 
            m_activeNotifications.size() < m_config.maxVisibleToasts) {
            auto* pending = m_pendingNotifications.dequeue();
            showNotificationInternal(pending);
        }
    }
}

void ToastNotificationSystem::onParentResized()
{
    repositionNotifications();
}

bool ToastNotificationSystem::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_parentWindow && event->type() == QEvent::Resize) {
        onParentResized();
    }
    return QObject::eventFilter(obj, event);
}

// Singleton methods
ToastNotificationSystem* ToastNotificationSystem::instance()
{
    return s_instance;
}

void ToastNotificationSystem::setInstance(ToastNotificationSystem* instance)
{
    s_instance = instance;
}
#ifndef TOASTNOTIFICATIONSYSTEM_H
#define TOASTNOTIFICATIONSYSTEM_H

#include <QWidget>
#include <QLabel>
#include <QTimer>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QColor>
#include <QQueue>
#include <QEasingCurve>
#include "AppColors.h"

class ToastNotification : public QWidget
{
    Q_OBJECT

public:
    enum class Type {
        Success,
        Error,
        Warning,
        Info,
        Loading
    };

    enum class Position {
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
        TopCenter,
        BottomCenter
    };

    struct Style {
        QColor textColor = Qt::white;
        QColor backgroundColor = QColor(50, 50, 50, 230);
        QColor borderColor = QColor(0, 0, 0, 80);
        int borderRadius = 8;
        int borderWidth = 1;
        QFont font;
        
        Style() {
            font.setPointSize(11);
            font.setWeight(QFont::Medium);
        }
    };

    explicit ToastNotification(const QString& text, Type type, QWidget* parent = nullptr);
    ~ToastNotification() override;

    void setDuration(int milliseconds);
    void setStyle(const Style& style);
    void show();

signals:
    void finished();

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onFadeInFinished();
    void onTimerExpired();
    void onFadeOutFinished();

private:
    void setupAnimation();
    void startFadeIn();
    void startFadeOut();
    Style getDefaultStyleForType(Type type) const;

    QString m_text;
    Type m_type;
    Style m_style;
    int m_duration;
    
    QLabel* m_textLabel;
    QTimer* m_timer;
    QPropertyAnimation* m_fadeAnimation;
    QPropertyAnimation* m_slideAnimation;
    QGraphicsOpacityEffect* m_opacityEffect;
    
    bool m_isShowing;
    bool m_isHiding;
    
    // Animation settings
    static constexpr int DEFAULT_DURATION = 4000; // 4 seconds
    static constexpr int FADE_DURATION = 300;     // 300ms
    static constexpr int SLIDE_DISTANCE = 20;     // 20px upward slide
};

class ToastNotificationSystem : public QObject
{
    Q_OBJECT

public:
    struct Config {
        ToastNotification::Position position = ToastNotification::Position::BottomLeft;
        int maxVisibleToasts = 3;
        int spacing = 10;
        int marginFromEdge = 20; // generic fallback for edges not covered below
        int marginLeft = 30;     // specific left margin for container
        int marginBottom = 30;   // specific bottom margin for container
        int animationDuration = 300;
        QEasingCurve::Type easingCurve = QEasingCurve::OutQuad;
        
        // Default styles for each notification type
        ToastNotification::Style successStyle;
        ToastNotification::Style errorStyle;
        ToastNotification::Style warningStyle;
        ToastNotification::Style infoStyle;
        ToastNotification::Style loadingStyle;
        
        Config() {
            // Match global network indicator styling: translucent background + colored text
            successStyle.backgroundColor = AppColors::gStatusConnectedBg;
            successStyle.textColor = AppColors::gStatusConnectedText;
            successStyle.borderColor = AppColors::gStatusConnectedText;

            warningStyle.backgroundColor = AppColors::gStatusWarningBg;
            warningStyle.textColor = AppColors::gStatusWarningText;
            warningStyle.borderColor = AppColors::gStatusWarningText;

            errorStyle.backgroundColor = AppColors::gStatusErrorBg;
            errorStyle.textColor = AppColors::gStatusErrorText;
            errorStyle.borderColor = AppColors::gStatusErrorText;

            // Info: use brand blue scheme
            infoStyle.backgroundColor = AppColors::gBrandBlueLight;
            infoStyle.textColor = AppColors::gBrandBlue;
            infoStyle.borderColor = AppColors::gBrandBlue;

            // Loading: align to info scheme for consistency
            loadingStyle.backgroundColor = AppColors::gBrandBlueLight;
            loadingStyle.textColor = AppColors::gBrandBlue;
            loadingStyle.borderColor = AppColors::gBrandBlue;
        }
    };

    explicit ToastNotificationSystem(QWidget* parentWindow, QObject* parent = nullptr);
    ~ToastNotificationSystem() override;

    // Configuration
    void setConfig(const Config& config);
    Config getConfig() const { return m_config; }
    
    // Show notifications
    void showSuccess(const QString& message, int duration = -1);
    void showError(const QString& message, int duration = -1);
    void showWarning(const QString& message, int duration = -1);
    void showInfo(const QString& message, int duration = -1);
    void showLoading(const QString& message, int duration = -1);
    
    // Generic show method
    void showNotification(const QString& message, ToastNotification::Type type, int duration = -1);
    
    // Clear all notifications
    void clearAll();
    
    // Singleton access (optional convenience method)
    static ToastNotificationSystem* instance();
    static void setInstance(ToastNotificationSystem* instance);

private slots:
    void onNotificationFinished();
    void onParentResized();
    
    // QObject override
    bool eventFilter(QObject* obj, QEvent* event) override;
    
private:
    void showNotificationInternal(ToastNotification* notification);
    void repositionNotifications();
    QPoint calculateNotificationPosition(int index) const;
    void removeOldestNotificationIfNeeded();
    ToastNotification::Style getStyleForType(ToastNotification::Type type) const;

    QWidget* m_parentWindow;
    Config m_config;
    QList<ToastNotification*> m_activeNotifications;
    QQueue<ToastNotification*> m_pendingNotifications;
    
    static ToastNotificationSystem* s_instance;
};

// Convenience macros for easy usage with optional duration
#define TOAST_SUCCESS(...) do { \
    if (auto* system = ToastNotificationSystem::instance()) { \
        system->showSuccess(__VA_ARGS__); \
    } \
} while(0)

#define TOAST_ERROR(...) do { \
    if (auto* system = ToastNotificationSystem::instance()) { \
        system->showError(__VA_ARGS__); \
    } \
} while(0)

#define TOAST_WARNING(...) do { \
    if (auto* system = ToastNotificationSystem::instance()) { \
        system->showWarning(__VA_ARGS__); \
    } \
} while(0)

#define TOAST_INFO(...) do { \
    if (auto* system = ToastNotificationSystem::instance()) { \
        system->showInfo(__VA_ARGS__); \
    } \
} while(0)

#define TOAST_LOADING(...) do { \
    if (auto* system = ToastNotificationSystem::instance()) { \
        system->showLoading(__VA_ARGS__); \
    } \
} while(0)

#endif // TOASTNOTIFICATIONSYSTEM_H
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
        int borderRadius = 8;
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
        int marginFromEdge = 20;
        int animationDuration = 300;
        QEasingCurve::Type easingCurve = QEasingCurve::OutQuad;
        
        // Default styles for each notification type
        ToastNotification::Style successStyle;
        ToastNotification::Style errorStyle;
        ToastNotification::Style warningStyle;
        ToastNotification::Style infoStyle;
        ToastNotification::Style loadingStyle;
        
        Config() {
            // Success: Green theme
            successStyle.backgroundColor = QColor(46, 125, 50, 230);
            successStyle.textColor = Qt::white;
            
            // Error: Red theme
            errorStyle.backgroundColor = QColor(211, 47, 47, 230);
            errorStyle.textColor = Qt::white;
            
            // Warning: Orange theme
            warningStyle.backgroundColor = QColor(245, 124, 0, 230);
            warningStyle.textColor = Qt::white;
            
            // Info: Blue theme
            infoStyle.backgroundColor = QColor(25, 118, 210, 230);
            infoStyle.textColor = Qt::white;
            
            // Loading: Gray theme
            loadingStyle.backgroundColor = QColor(97, 97, 97, 230);
            loadingStyle.textColor = Qt::white;
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
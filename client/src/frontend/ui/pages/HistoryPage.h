#ifndef HISTORYPAGE_H
#define HISTORYPAGE_H

#include <QWidget>

class QLabel;
class NotificationCenter;
struct NotificationEntry;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

/** Displays the durable notification history, newest entry first. */
class HistoryPage final : public QWidget {
    Q_OBJECT

public:
    explicit HistoryPage(NotificationCenter* notificationCenter = nullptr,
                         QWidget* parent = nullptr);
    ~HistoryPage() override = default;

    void setNotificationCenter(NotificationCenter* notificationCenter);
    NotificationCenter* notificationCenter() const { return m_notificationCenter; }

private slots:
    void refresh();
    void confirmClearHistory();

private:
    void setupUi();
    QWidget* createEntryCard(const NotificationEntry& entry) const;

    NotificationCenter* m_notificationCenter = nullptr; // Not owned.
    QStackedWidget* m_contentStack = nullptr;
    QWidget* m_emptyPage = nullptr;
    QLabel* m_emptyLabel = nullptr;
    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_entriesHost = nullptr;
    QVBoxLayout* m_entriesLayout = nullptr;
    QPushButton* m_clearButton = nullptr;
};

#endif // HISTORYPAGE_H

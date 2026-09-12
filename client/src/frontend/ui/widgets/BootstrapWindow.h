#ifndef BOOTSTRAPWINDOW_H
#define BOOTSTRAPWINDOW_H

#include "backend/runtime/RuntimeStorageBootstrap.h"

#include <QDialog>

class QLabel;
class QPushButton;
class QProgressBar;

class BootstrapWindow final : public QDialog {
    Q_OBJECT

public:
    explicit BootstrapWindow(QWidget* parent = nullptr);

    void setStage(RuntimeStorageBootstrap::Stage stage);
    // Returns true for Retry and false for Quit.
    bool waitForRetry(const RuntimeStorageBootstrap::Result& result);
    // Blocks until the user acknowledges a destructive reset summary.
    bool acknowledgeReset(const RuntimeStorageBootstrap::Result& result);

private:
    void setDecisionMode(const QString& title,
                         const QString& detail,
                         const QString& primaryText);

    QLabel* m_title = nullptr;
    QLabel* m_detail = nullptr;
    QProgressBar* m_progress = nullptr;
    QPushButton* m_primary = nullptr;
    QPushButton* m_quit = nullptr;
};

#endif // BOOTSTRAPWINDOW_H

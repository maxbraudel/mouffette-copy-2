#include "frontend/ui/widgets/BootstrapWindow.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QVBoxLayout>

BootstrapWindow::BootstrapWindow(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Mouffette"));
    setModal(true);
    setMinimumWidth(460);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(14);
    m_title = new QLabel(QStringLiteral("Preparing Mouffette"), this);
    QFont titleFont = m_title->font();
    titleFont.setPointSize(titleFont.pointSize() + 3);
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_detail = new QLabel(QStringLiteral("Preparing runtime"), this);
    m_detail->setWordWrap(true);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->setTextVisible(false);
    layout->addWidget(m_title);
    layout->addWidget(m_detail);
    layout->addWidget(m_progress);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    m_quit = new QPushButton(QStringLiteral("Quit"), this);
    m_primary = new QPushButton(QStringLiteral("Retry"), this);
    m_primary->setDefault(true);
    buttons->addWidget(m_quit);
    buttons->addWidget(m_primary);
    layout->addLayout(buttons);
    m_quit->hide();
    m_primary->hide();

    connect(m_primary, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_quit, &QPushButton::clicked, this, &QDialog::reject);
}

void BootstrapWindow::setStage(RuntimeStorageBootstrap::Stage stage)
{
    m_title->setText(QStringLiteral("Checking local storage"));
    m_detail->setText(RuntimeStorageBootstrap::stageLabel(stage) + QStringLiteral("…"));
    m_primary->hide();
    m_quit->hide();
    m_progress->show();
    if (!isVisible()) show();
    QCoreApplication::processEvents();
}

void BootstrapWindow::setDecisionMode(const QString& title,
                                      const QString& detail,
                                      const QString& primaryText)
{
    m_title->setText(title);
    m_detail->setText(detail);
    m_primary->setText(primaryText);
    m_progress->hide();
    m_primary->show();
    m_quit->show();
}

bool BootstrapWindow::waitForRetry(const RuntimeStorageBootstrap::Result& result)
{
    setDecisionMode(QStringLiteral("Storage check failed"),
                    result.cause + QStringLiteral(
                        "\n\nMouffette has not opened a network connection. "
                        "Retry after resolving the local storage problem."),
                    QStringLiteral("Retry"));
    return exec() == QDialog::Accepted;
}

bool BootstrapWindow::acknowledgeReset(
    const RuntimeStorageBootstrap::Result& result)
{
    QString detail = result.cause;
    if (!result.foundVersion.isEmpty()) {
        detail += QStringLiteral("\n\nStorage version: found %1; expected %2.")
                      .arg(result.foundVersion)
                      .arg(result.expectedVersion);
    }
    detail += QStringLiteral("\nReset categories: %1.")
                  .arg(result.resetCategories.join(QStringLiteral(", ")));
    detail += QStringLiteral(
        "\n\nExternal source media files were not modified.");
    setDecisionMode(QStringLiteral("Local storage was repaired"), detail,
                    QStringLiteral("Continue"));
    return exec() == QDialog::Accepted;
}

#include "MediaSettingsPanel.h"

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsProxyWidget>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QLineEdit>
#include <QPalette>
#include <QGuiApplication>

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

    // Styling: match overlay grey background (approx 160 alpha black over white -> ~#000000 with alpha)
    // We'll use a semi-opaque dark background and white text, rounded corners handled by proxy bounds.
    QPalette pal = m_widget->palette();
    pal.setColor(QPalette::Window, QColor(0,0,0,160));
    pal.setColor(QPalette::WindowText, Qt::white);
    m_widget->setPalette(pal);
    m_widget->setAutoFillBackground(true);

    m_layout = new QVBoxLayout(m_widget);
    m_layout->setContentsMargins(16, 16, 16, 16);
    m_layout->setSpacing(10);

    m_title = new QLabel("Scene options");
    QFont tf = m_title->font();
    tf.setBold(true);
    tf.setPointSize(tf.pointSize() + 1);
    m_title->setFont(tf);
    m_title->setStyleSheet("color: white;");
    m_layout->addWidget(m_title);

    // 1) Play automatically after x seconds
    {
        auto* row = new QWidget(m_widget);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(8);
        m_autoPlayCheck = new QCheckBox("Play automatically after", row);
        m_autoPlayCheck->setStyleSheet("color: white;");
        m_autoPlaySeconds = new QLineEdit(row);
        m_autoPlaySeconds->setPlaceholderText("seconds");
        m_autoPlaySeconds->setFixedWidth(80);
        h->addWidget(m_autoPlayCheck);
        h->addWidget(m_autoPlaySeconds);
        h->addStretch();
        m_layout->addWidget(row);
    }

    // 2) Repeat X times
    {
        auto* row = new QWidget(m_widget);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(8);
        m_repeatCheck = new QCheckBox("Repeat", row);
        m_repeatCheck->setStyleSheet("color: white;");
        m_repeatTimes = new QLineEdit(row);
        m_repeatTimes->setPlaceholderText("times");
        m_repeatTimes->setFixedWidth(80);
        h->addWidget(m_repeatCheck);
        h->addWidget(m_repeatTimes);
        h->addStretch();
        m_layout->addWidget(row);
    }

    // 3) Appear in X seconds fade in
    {
        auto* row = new QWidget(m_widget);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(8);
        m_fadeInCheck = new QCheckBox("Appear in", row);
        m_fadeInCheck->setStyleSheet("color: white;");
        m_fadeInSeconds = new QLineEdit(row);
        m_fadeInSeconds->setPlaceholderText("seconds fade in");
        m_fadeInSeconds->setFixedWidth(120);
        h->addWidget(m_fadeInCheck);
        h->addWidget(m_fadeInSeconds);
        h->addStretch();
        m_layout->addWidget(row);
    }

    // 4) Disappear in X seconds fade out
    {
        auto* row = new QWidget(m_widget);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(8);
        m_fadeOutCheck = new QCheckBox("Disappear in", row);
        m_fadeOutCheck->setStyleSheet("color: white;");
        m_fadeOutSeconds = new QLineEdit(row);
        m_fadeOutSeconds->setPlaceholderText("seconds fade out");
        m_fadeOutSeconds->setFixedWidth(140);
        h->addWidget(m_fadeOutCheck);
        h->addWidget(m_fadeOutSeconds);
        h->addStretch();
        m_layout->addWidget(row);
    }

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
}

void MediaSettingsPanel::ensureInScene(QGraphicsScene* scene) {
    if (!scene || !m_proxy) return;
    if (!m_proxy->scene()) scene->addItem(m_proxy);
}

void MediaSettingsPanel::setVisible(bool visible) {
    if (!m_proxy) return;
    m_proxy->setVisible(visible);
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
}

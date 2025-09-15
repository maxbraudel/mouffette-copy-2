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
    m_widget->setStyleSheet("background-color: transparent; color: white;");
    m_widget->setAutoFillBackground(false);

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

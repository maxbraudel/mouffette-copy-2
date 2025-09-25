// Single, deduplicated implementation of ScreenNavigationManager
#include "ScreenNavigationManager.h"
#include "ClientInfo.h"
#include "SpinnerWidget.h"
#include "ScreenCanvas.h"
#include <QStackedWidget>
#include <QPushButton>
#include <QTimer>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QDebug>

ScreenNavigationManager::ScreenNavigationManager(QObject* parent) : QObject(parent) {}

void ScreenNavigationManager::setWidgets(const Widgets& w) { m_w = w; }

void ScreenNavigationManager::setDurations(int loaderDelayMs, int loaderFadeMs, int canvasFadeMs) {
    m_loaderDelayMs = qMax(0, loaderDelayMs);
    m_loaderFadeDurationMs = qMax(0, loaderFadeMs);
    m_canvasFadeDurationMs = qMax(0, canvasFadeMs);
}

bool ScreenNavigationManager::isOnScreenView() const {
    return m_w.stack && m_w.screenViewPage && m_w.stack->currentWidget() == m_w.screenViewPage;
}

void ScreenNavigationManager::showScreenView(const ClientInfo& client) {
    if (!m_w.stack || !m_w.screenViewPage) return;
    const QString id = client.getId();
    m_currentClientId = id;

    // Switch UI early
    m_w.stack->setCurrentWidget(m_w.screenViewPage);
    if (m_w.backButton) m_w.backButton->show();

    // Reset spinner/canvas states
    if (m_w.canvasStack) m_w.canvasStack->setCurrentIndex(0); // assume 0 = spinner container
    if (m_w.loadingSpinner) {
        m_w.loadingSpinner->stop();
        if (m_w.spinnerFade) m_w.spinnerFade->stop();
        if (m_w.spinnerOpacity) m_w.spinnerOpacity->setOpacity(0.0);
    }
    if (m_w.volumeFade) m_w.volumeFade->stop();
    if (m_w.volumeOpacity) m_w.volumeOpacity->setOpacity(0.0);
    if (m_w.canvasFade) m_w.canvasFade->stop();
    if (m_w.canvasOpacity) m_w.canvasOpacity->setOpacity(0.0);

    startSpinnerDelayed();

    if (!id.isEmpty()) {
        emit requestScreens(id);
        emit watchTargetRequested(id);
    }
    emit screenViewEntered(id);
}

void ScreenNavigationManager::showClientList() {
    if (!m_w.stack || !m_w.clientListPage) return;
    if (m_w.backButton) m_w.backButton->hide();
    if (m_loaderDelayTimer) m_loaderDelayTimer->stop();
    stopSpinner();
    m_currentClientId.clear();
    m_w.stack->setCurrentWidget(m_w.clientListPage);
    emit clientListEntered();
}

void ScreenNavigationManager::revealCanvas() {
    if (!isOnScreenView()) return; // Only if we're still on screen view
    stopSpinner();
    if (m_w.canvasStack) m_w.canvasStack->setCurrentIndex(1); // show canvas
    
    // Show preserved content after reconnection
    if (m_w.screenCanvas) {
        m_w.screenCanvas->showContentAfterReconnect();
    }
    
    fadeInCanvas();
}

void ScreenNavigationManager::enterLoadingStateImmediate() {
    if (!isOnScreenView()) return;
    // Stop any pending loader delay and fades
    if (m_loaderDelayTimer && m_loaderDelayTimer->isActive()) m_loaderDelayTimer->stop();
    if (m_w.canvasFade) m_w.canvasFade->stop();
    if (m_w.volumeFade) m_w.volumeFade->stop();
    if (m_w.spinnerFade) m_w.spinnerFade->stop();

    // Hide canvas content but preserve viewport state (do not clear the screen items)
    if (m_w.screenCanvas) {
        m_w.screenCanvas->hideContentPreservingState();
    }
    if (m_w.canvasOpacity) m_w.canvasOpacity->setOpacity(0.0);
    if (m_w.canvasStack) m_w.canvasStack->setCurrentIndex(0); // spinner page

    // Hide volume overlay immediately
    if (m_w.volumeOpacity) m_w.volumeOpacity->setOpacity(0.0);

    // Show spinner immediately (no delay)
    if (m_w.loadingSpinner) m_w.loadingSpinner->start();
    if (m_w.spinnerOpacity) m_w.spinnerOpacity->setOpacity(1.0);
}

void ScreenNavigationManager::ensureLoaderTimer() {
    if (!m_loaderDelayTimer) {
        m_loaderDelayTimer = new QTimer(this);
        m_loaderDelayTimer->setSingleShot(true);
        connect(m_loaderDelayTimer, &QTimer::timeout, this, &ScreenNavigationManager::onLoaderDelayTimeout);
    }
}

void ScreenNavigationManager::startSpinnerDelayed() {
    if (!m_w.loadingSpinner || !m_w.spinnerFade || !m_w.spinnerOpacity) return;
    ensureLoaderTimer();
    if (m_loaderDelayTimer->isActive()) m_loaderDelayTimer->stop();
    m_loaderDelayTimer->start(m_loaderDelayMs);
}

void ScreenNavigationManager::stopSpinner() {
    if (m_loaderDelayTimer && m_loaderDelayTimer->isActive()) m_loaderDelayTimer->stop();
    if (m_w.loadingSpinner) m_w.loadingSpinner->stop();
    if (m_w.spinnerFade) m_w.spinnerFade->stop();
    if (m_w.spinnerOpacity) m_w.spinnerOpacity->setOpacity(0.0);
}

void ScreenNavigationManager::fadeInCanvas() {
    if (!m_w.canvasFade || !m_w.canvasOpacity) return;
    m_w.canvasFade->stop();
    m_w.canvasFade->setDuration(m_canvasFadeDurationMs);
    m_w.canvasFade->setStartValue(0.0);
    m_w.canvasFade->setEndValue(1.0);
    m_w.canvasFade->start();
}

void ScreenNavigationManager::onLoaderDelayTimeout() {
    if (!m_w.loadingSpinner || !m_w.spinnerFade || !m_w.spinnerOpacity) return;
    if (!isOnScreenView()) return; // navigated away early
    m_w.loadingSpinner->start();
    m_w.spinnerFade->stop();
    m_w.spinnerFade->setDuration(m_loaderFadeDurationMs);
    m_w.spinnerFade->setStartValue(0.0);
    m_w.spinnerFade->setEndValue(1.0);
    m_w.spinnerFade->start();
}

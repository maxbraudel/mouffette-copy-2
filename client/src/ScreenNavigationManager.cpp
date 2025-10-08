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

void ScreenNavigationManager::setActiveCanvas(ScreenCanvas* canvas) {
    m_w.screenCanvas = canvas;
}

bool ScreenNavigationManager::isOnScreenView() const {
    return m_w.stack && m_w.screenViewPage && m_w.stack->currentWidget() == m_w.screenViewPage;
}

void ScreenNavigationManager::showScreenView(const ClientInfo& client, bool hasCachedContent) {
    if (!m_w.stack || !m_w.screenViewPage) return;
    const QString id = client.getId();
    const bool isOnline = client.isOnline();
    const bool useCachedContent = hasCachedContent;
    m_currentClientId = id;

    // Switch UI early
    m_w.stack->setCurrentWidget(m_w.screenViewPage);
    if (m_w.backButton) m_w.backButton->show();

    // Stop any pending spinner state before we decide how to present content
    stopSpinner();
    if (m_w.loadingSpinner) m_w.loadingSpinner->stop();
    if (m_w.spinnerFade) m_w.spinnerFade->stop();
    if (m_w.inlineSpinner) {
        m_w.inlineSpinner->stop();
        m_w.inlineSpinner->hide();
    }
    if (m_w.volumeFade) m_w.volumeFade->stop();
    if (m_w.canvasFade) m_w.canvasFade->stop();

    if (useCachedContent) {
        if (m_w.canvasStack) m_w.canvasStack->setCurrentIndex(1);
        if (m_w.canvasOpacity) m_w.canvasOpacity->setOpacity(1.0);
        if (m_w.screenCanvas) {
            m_w.screenCanvas->showContentAfterReconnect();
        }
        if (m_w.canvasContentEverLoaded) {
            *m_w.canvasContentEverLoaded = true;
        }
        // Show inline spinner while waiting for fresh data from an online client
        if (isOnline && !id.isEmpty() && m_w.inlineSpinner) {
            m_w.inlineSpinner->show();
            m_w.inlineSpinner->start();
        }
    } else {
        // No cached content: fall back to full-screen loader behavior
        if (m_w.canvasOpacity) m_w.canvasOpacity->setOpacity(0.0);
        if (m_w.canvasStack) m_w.canvasStack->setCurrentIndex(0); // spinner page
        if (m_w.volumeOpacity) m_w.volumeOpacity->setOpacity(0.0);

        if (isOnline && !id.isEmpty()) {
            startSpinnerDelayed();
        } else {
            // Offline or unknown id: show whatever cached scene remains immediately
            if (m_w.canvasStack) m_w.canvasStack->setCurrentIndex(1);
            if (m_w.canvasOpacity) m_w.canvasOpacity->setOpacity(1.0);
        }
    }

    if (isOnline && !id.isEmpty()) {
        emit requestScreens(id);
        emit watchTargetRequested(id);
    }
    emit screenViewEntered(id);
}

void ScreenNavigationManager::refreshActiveClientPreservingCanvas(const ClientInfo& client) {
    if (!m_w.stack || !m_w.screenViewPage) return;
    const QString id = client.getId();
    const bool isOnline = client.isOnline();

    // If we're not already on the screen view, fall back to the full transition
    if (!isOnScreenView()) {
        showScreenView(client);
        return;
    }

    m_currentClientId = id;

    // Ensure full-screen spinner is stopped and canvas remains visible
    stopSpinner();
    if (m_w.canvasStack) m_w.canvasStack->setCurrentIndex(1);
    if (m_w.canvasOpacity) m_w.canvasOpacity->setOpacity(1.0);

    if (isOnline && !id.isEmpty()) {
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
    
    // Hide inline spinner (in case it was used during reconnection)
    if (m_w.inlineSpinner && m_w.inlineSpinner->isSpinning()) {
        m_w.inlineSpinner->stop();
        m_w.inlineSpinner->hide();
    }
    
    if (m_w.canvasStack) m_w.canvasStack->setCurrentIndex(1); // show canvas
    
    // Show preserved content after reconnection (only if it was actually hidden)
    // This prevents unnecessary overlay refreshes that cause flicker
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

    // Decide between full-screen loader (initial load) or inline loader (reconnection)
    const bool useInlineLoader = m_w.canvasContentEverLoaded && *m_w.canvasContentEverLoaded;
    m_usingInlineLoader = useInlineLoader;
    
    if (useInlineLoader) {
        // Content already loaded - use inline spinner and keep canvas visible
        // Hide full-screen spinner if it's showing
        if (m_w.loadingSpinner) m_w.loadingSpinner->stop();
        if (m_w.spinnerOpacity) m_w.spinnerOpacity->setOpacity(0.0);
        
        // Keep canvas visible - do NOT hide content to avoid flicker
        if (m_w.canvasStack) m_w.canvasStack->setCurrentIndex(1); // stay on canvas page
        if (m_w.canvasOpacity) m_w.canvasOpacity->setOpacity(1.0); // keep canvas fully visible
        
        // Show inline spinner in client info container
        if (m_w.inlineSpinner) {
            m_w.inlineSpinner->show();
            m_w.inlineSpinner->start();
        }
        
        // Keep volume overlay visible
        // (volumeOpacity stays as is)
        // Do NOT call hideContentPreservingState() - keep everything visible
    } else {
        // Initial load - use full-screen blocking loader
        // Hide canvas content but preserve viewport state (do not clear the screen items)
        if (m_w.screenCanvas) {
            m_w.screenCanvas->hideContentPreservingState();
        }
        if (m_w.canvasOpacity) m_w.canvasOpacity->setOpacity(0.0);
        if (m_w.canvasStack) m_w.canvasStack->setCurrentIndex(0); // spinner page

        // Hide volume overlay immediately
        if (m_w.volumeOpacity) m_w.volumeOpacity->setOpacity(0.0);

        // Show full-screen spinner immediately (no delay)
        if (m_w.loadingSpinner) m_w.loadingSpinner->start();
        if (m_w.spinnerOpacity) m_w.spinnerOpacity->setOpacity(1.0);
        
        // Hide inline spinner if it was showing
        if (m_w.inlineSpinner && m_w.inlineSpinner->isSpinning()) {
            m_w.inlineSpinner->stop();
            m_w.inlineSpinner->hide();
        }
    }
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
    const qreal currentOpacity = m_w.canvasOpacity->opacity();
    // Always skip animation if using inline loader OR already visible
    // This prevents flicker during reconnection
    if (m_usingInlineLoader || currentOpacity >= 0.95) {
        m_w.canvasOpacity->setOpacity(1.0);
        m_usingInlineLoader = false;
        return;
    }
    // Only animate from 0 on initial load
    if (currentOpacity < 0.1) {
        m_w.canvasFade->setDuration(m_canvasFadeDurationMs);
        m_w.canvasFade->setStartValue(currentOpacity);
        m_w.canvasFade->setEndValue(1.0);
        m_w.canvasFade->start();
    } else {
        // Already mostly visible, just snap to 1.0
        m_w.canvasOpacity->setOpacity(1.0);
    }
    m_usingInlineLoader = false;
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

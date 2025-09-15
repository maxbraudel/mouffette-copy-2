// MediaItems.cpp - Implementation of media item hierarchy extracted from MainWindow.cpp

#include "MediaItems.h"
#include <QPainter>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QStyleOptionGraphicsItem>
#include <QVariantAnimation>
#include <QtSvgWidgets/QGraphicsSvgItem>
#include <QtSvg/QSvgRenderer>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoSink>
#include <QMediaMetaData>
#include <QThreadPool>
#include <QRunnable>
#include <QApplication>
#include <QDateTime>
#include <QTimer>
#include <QMutexLocker>
#include <QDebug>
#include "MediaSettingsPanel.h"
#include <cmath>

// ---------------- ResizableMediaBase -----------------

int ResizableMediaBase::heightOfMediaOverlays = -1; // default auto
int ResizableMediaBase::cornerRadiusOfMediaOverlays = 6;

ResizableMediaBase::~ResizableMediaBase() = default;

ResizableMediaBase::ResizableMediaBase(const QSize& baseSizePx, int visualSizePx, int selectionSizePx, const QString& filename)
{
    m_visualSize = qMax(4, visualSizePx);
    m_selectionSize = qMax(m_visualSize, selectionSizePx);
    setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);
    m_baseSize = baseSizePx;
    setScale(1.0);
    setZValue(1.0);
    m_filename = filename;
    initializeOverlays();
}

void ResizableMediaBase::setHeightOfMediaOverlaysPx(int px) { heightOfMediaOverlays = px; }
int  ResizableMediaBase::getHeightOfMediaOverlaysPx() { return heightOfMediaOverlays; }
void ResizableMediaBase::setCornerRadiusOfMediaOverlaysPx(int px) { cornerRadiusOfMediaOverlays = std::max(0, px); }
int  ResizableMediaBase::getCornerRadiusOfMediaOverlaysPx() { return cornerRadiusOfMediaOverlays; }

void ResizableMediaBase::requestLabelRelayout() { updateOverlayLayout(); }

bool ResizableMediaBase::isOnHandleAtItemPos(const QPointF& itemPos) const { return hitTestHandle(itemPos) != None; }

bool ResizableMediaBase::beginResizeAtScenePos(const QPointF& scenePos) {
    Handle h = hitTestHandle(mapFromScene(scenePos));
    if (h == None) return false;
    m_activeHandle = h;
    m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
    m_fixedScenePoint = mapToScene(m_fixedItemPoint);
    m_initialScale = scale();
    const qreal d = std::hypot(scenePos.x() - m_fixedScenePoint.x(), scenePos.y() - m_fixedScenePoint.y());
    m_initialGrabDist = (d > 1e-6) ? d : 1e-6;
    grabMouse();
    return true;
}

Qt::CursorShape ResizableMediaBase::cursorForScenePos(const QPointF& scenePos) const {
    switch (hitTestHandle(mapFromScene(scenePos))) {
        case TopLeft:
        case BottomRight:
            return Qt::SizeFDiagCursor;
        case TopRight:
        case BottomLeft:
            return Qt::SizeBDiagCursor;
        default:
            return Qt::ArrowCursor;
    }
}

void ResizableMediaBase::setHandleVisualSize(int px) {
    int newVisual = qMax(4, px);
    int newSelection = qMax(m_selectionSize, newVisual);
    if (newSelection != m_selectionSize) {
        prepareGeometryChange();
        m_selectionSize = newSelection;
    }
    m_visualSize = newVisual;
    update();
}

void ResizableMediaBase::setHandleSelectionSize(int px) {
    int newSel = qMax(4, px);
    if (newSel != m_selectionSize) {
        prepareGeometryChange();
        m_selectionSize = newSel;
        update();
    }
}

QRectF ResizableMediaBase::boundingRect() const {
    QRectF br(0, 0, m_baseSize.width(), m_baseSize.height());
    if (isSelected()) {
        qreal pad = toItemLengthFromPixels(m_selectionSize) / 2.0;
        return br.adjusted(-pad, -pad, pad, pad);
    }
    return br;
}

QPainterPath ResizableMediaBase::shape() const {
    QPainterPath path; QRectF br(0,0, m_baseSize.width(), m_baseSize.height()); path.addRect(br);
    if (isSelected()) {
        const qreal s = toItemLengthFromPixels(m_selectionSize);
        path.addRect(QRectF(br.topLeft() - QPointF(s/2,s/2), QSizeF(s,s)));
        path.addRect(QRectF(QPointF(br.right(), br.top()) - QPointF(s/2,s/2), QSizeF(s,s)));
        path.addRect(QRectF(QPointF(br.left(), br.bottom()) - QPointF(s/2,s/2), QSizeF(s,s)));
        path.addRect(QRectF(br.bottomRight() - QPointF(s/2,s/2), QSizeF(s,s)));
    }
    return path;
}

QVariant ResizableMediaBase::itemChange(GraphicsItemChange change, const QVariant &value) {
    if (change == ItemSelectedChange) {
        prepareGeometryChange();
    }
    if (change == ItemSelectedHasChanged) {
        updateOverlayLayout();
        updateOverlayVisibility();
    }
    if (change == ItemTransformHasChanged || change == ItemPositionHasChanged) {
        updateOverlayLayout();
    }
    return QGraphicsItem::itemChange(change, value);
}

void ResizableMediaBase::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    m_activeHandle = hitTestHandle(event->pos());
    if (m_activeHandle != None) {
        m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
        m_fixedScenePoint = mapToScene(m_fixedItemPoint);
        m_initialScale = scale();
        const qreal d = std::hypot(event->scenePos().x() - m_fixedScenePoint.x(), event->scenePos().y() - m_fixedScenePoint.y());
        m_initialGrabDist = (d > 1e-6) ? d : 1e-6;
        event->accept();
        return;
    }
    QGraphicsItem::mousePressEvent(event);
}

void ResizableMediaBase::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    if (m_activeHandle != None) {
        const QPointF v = event->scenePos() - m_fixedScenePoint;
        const qreal currDist = std::hypot(v.x(), v.y());
        qreal newScale = m_initialScale * (currDist / (m_initialGrabDist > 0 ? m_initialGrabDist : 1e-6));
        newScale = std::clamp<qreal>(newScale, 0.05, 100.0);
        setScale(newScale);
        setPos(m_fixedScenePoint - newScale * m_fixedItemPoint);
        onInteractiveGeometryChanged();
        event->accept();
        return;
    }
    QGraphicsItem::mouseMoveEvent(event);
}

void ResizableMediaBase::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    if (m_activeHandle != None) {
        m_activeHandle = None;
        ungrabMouse();
        onInteractiveGeometryChanged();
        event->accept();
        return;
    }
    QGraphicsItem::mouseReleaseEvent(event);
}

void ResizableMediaBase::prepareForDeletion() {
    if (m_beingDeleted) return;
    m_beingDeleted = true;
    // Cancel any active resize interaction.
    if (m_activeHandle != None) { m_activeHandle = None; ungrabMouse(); }
    // Fully detach overlay panels: remove their background graphics items from the scene so no further relayout occurs.
    auto detachPanel = [&](std::unique_ptr<OverlayPanel>& panel){
        if (!panel) return;
        panel->setVisible(false);
        // We rely on panel destructor later; clear elements now to drop graphics.
        panel->clearElements();
    };
    detachPanel(m_topPanel);
    detachPanel(m_bottomPanel);
    // Hide and drop settings panel (proxy will be removed from scene by destructor)
    if (m_settingsPanel) {
        m_settingsPanel->setVisible(false);
        m_settingsPanel.reset();
    }
}

void ResizableMediaBase::hoverMoveEvent(QGraphicsSceneHoverEvent* event) { QGraphicsItem::hoverMoveEvent(event); }
void ResizableMediaBase::hoverLeaveEvent(QGraphicsSceneHoverEvent* event) { QGraphicsItem::hoverLeaveEvent(event); }

void ResizableMediaBase::paintSelectionAndLabel(QPainter* painter) {
    if (!isSelected()) return;
    QRectF br(0, 0, m_baseSize.width(), m_baseSize.height());
    painter->save();
    painter->setBrush(Qt::NoBrush);
    QPen whitePen(QColor(255,255,255)); whitePen.setCosmetic(true); whitePen.setWidth(1); whitePen.setStyle(Qt::DashLine); whitePen.setDashPattern({4,4}); whitePen.setCapStyle(Qt::FlatCap); whitePen.setJoinStyle(Qt::MiterJoin);
    painter->setPen(whitePen); painter->drawRect(br);
    QPen bluePen(QColor(74,144,226)); bluePen.setCosmetic(true); bluePen.setWidth(1); bluePen.setStyle(Qt::DashLine); bluePen.setDashPattern({4,4}); bluePen.setDashOffset(4); bluePen.setCapStyle(Qt::FlatCap); bluePen.setJoinStyle(Qt::MiterJoin);
    painter->setPen(bluePen); painter->drawRect(br);
    painter->restore();
    const qreal s = toItemLengthFromPixels(m_visualSize);
    painter->setPen(QPen(QColor(74,144,226), 0)); painter->setBrush(QBrush(Qt::white));
    painter->drawRect(QRectF(br.topLeft() - QPointF(s/2,s/2), QSizeF(s,s)));
    painter->drawRect(QRectF(QPointF(br.right(), br.top()) - QPointF(s/2,s/2), QSizeF(s,s)));
    painter->drawRect(QRectF(QPointF(br.left(), br.bottom()) - QPointF(s/2,s/2), QSizeF(s,s)));
    painter->drawRect(QRectF(br.bottomRight() - QPointF(s/2,s/2), QSizeF(s,s)));
}

ResizableMediaBase::Handle ResizableMediaBase::hitTestHandle(const QPointF& p) const {
    if (!isSelected()) return None;
    const qreal s = toItemLengthFromPixels(m_selectionSize);
    QRectF br(0, 0, m_baseSize.width(), m_baseSize.height());
    if (QRectF(br.topLeft() - QPointF(s/2,s/2), QSizeF(s,s)).contains(p)) return TopLeft;
    if (QRectF(QPointF(br.right(), br.top()) - QPointF(s/2,s/2), QSizeF(s,s)).contains(p)) return TopRight;
    if (QRectF(QPointF(br.left(), br.bottom()) - QPointF(s/2,s/2), QSizeF(s,s)).contains(p)) return BottomLeft;
    if (QRectF(br.bottomRight() - QPointF(s/2,s/2), QSizeF(s,s)).contains(p)) return BottomRight;
    return None;
}

ResizableMediaBase::Handle ResizableMediaBase::opposite(Handle h) const {
    switch (h) {
        case TopLeft: return BottomRight; case TopRight: return BottomLeft; case BottomLeft: return TopRight; case BottomRight: return TopLeft; default: return None;
    }
}

QPointF ResizableMediaBase::handlePoint(Handle h) const {
    switch (h) { case TopLeft: return QPointF(0,0); case TopRight: return QPointF(m_baseSize.width(),0); case BottomLeft: return QPointF(0,m_baseSize.height()); case BottomRight: return QPointF(m_baseSize.width(), m_baseSize.height()); default: return QPointF(0,0);} }

qreal ResizableMediaBase::toItemLengthFromPixels(int px) const {
    if (!scene() || scene()->views().isEmpty()) return px;
    QGraphicsView* v = scene()->views().first();
    QTransform itemToViewport = v->viewportTransform() * sceneTransform();
    qreal sx = std::hypot(itemToViewport.m11(), itemToViewport.m21());
    if (sx <= 1e-6) return px;
    return px / sx;
}

void ResizableMediaBase::initializeOverlays() {
    m_overlayStyle.cornerRadius = getCornerRadiusOfMediaOverlaysPx();
    if (getHeightOfMediaOverlaysPx() > 0) m_overlayStyle.defaultHeight = getHeightOfMediaOverlaysPx();
    m_topPanel = std::make_unique<OverlayPanel>(OverlayPanel::Top); m_topPanel->setStyle(m_overlayStyle);
    if (!m_filename.isEmpty()) {
        auto filenameElement = std::make_shared<OverlayTextElement>(m_filename, "filename");
        m_topPanel->addElement(filenameElement);
        // Add settings toggle button to the right of filename
        auto settingsBtn = std::make_shared<OverlayButtonElement>(QString(), "settings_toggle");
        // Resource path follows the same pattern as other media control icons (":/icons/icons/<name>.svg")
        settingsBtn->setSvgIcon(":/icons/icons/settings.svg");
        settingsBtn->setToggleOnly(true);
        // Initial state normal
        settingsBtn->setState(OverlayElement::Normal);
        // Toggle logic: cycles Normal <-> Toggled
        settingsBtn->setOnClicked([this, btnRef=settingsBtn]() {
            if (!btnRef) return;
            const bool enabling = (btnRef->state() != OverlayElement::Toggled);
            btnRef->setState(enabling ? OverlayElement::Toggled : OverlayElement::Normal);
            // Lazy-create settings panel
            if (!m_settingsPanel) m_settingsPanel = std::make_unique<MediaSettingsPanel>();
            // Ensure panel is in the scene
            if (scene()) m_settingsPanel->ensureInScene(scene());
            // Apply background color similar to overlay style
            // (Handled inside MediaSettingsPanel using palette/stylesheet defaults)
            if (scene() && !scene()->views().isEmpty()) {
                m_settingsPanel->updatePosition(scene()->views().first());
            }
            m_settingsPanel->setVisible(enabling);
        });
        m_topPanel->addElement(settingsBtn);
    }
    m_bottomPanel = std::make_unique<OverlayPanel>(OverlayPanel::Bottom); m_bottomPanel->setStyle(m_overlayStyle);
}

void ResizableMediaBase::updateOverlayVisibility() {
    // Show top overlay (filename + settings button) only when the item is selected,
    // matching bottom overlay behavior.
    bool shouldShowTop = isSelected() && !m_filename.isEmpty();
    if (m_topPanel) m_topPanel->setVisible(shouldShowTop);
    // bottom panel managed by video subclass
}

void ResizableMediaBase::updateOverlayLayout() {
    if (!scene() || scene()->views().isEmpty()) return;
    QGraphicsView* view = scene()->views().first();
    if (m_topPanel && !m_topPanel->scene()) m_topPanel->setScene(scene());
    if (m_bottomPanel && !m_bottomPanel->scene()) m_bottomPanel->setScene(scene());
    QRectF itemRect(0,0,m_baseSize.width(), m_baseSize.height());
    QPointF topAnchorScene = mapToScene(QPointF(itemRect.center().x(), itemRect.top()));
    QPointF bottomAnchorScene = mapToScene(QPointF(itemRect.center().x(), itemRect.bottom()));
    if (m_topPanel) m_topPanel->updateLayoutWithAnchor(topAnchorScene, view);
    if (m_bottomPanel) m_bottomPanel->updateLayoutWithAnchor(bottomAnchorScene, view);
    // Keep settings panel docked
    if (m_settingsPanel && m_settingsPanel->isVisible()) m_settingsPanel->updatePosition(view);
}

// ---------------- ResizablePixmapItem -----------------

ResizablePixmapItem::ResizablePixmapItem(const QPixmap& pm, int visualSizePx, int selectionSizePx, const QString& filename)
    : ResizableMediaBase(pm.size(), visualSizePx, selectionSizePx, filename), m_pix(pm) {}

void ResizablePixmapItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
    Q_UNUSED(option); Q_UNUSED(widget);
    if (!m_pix.isNull()) painter->drawPixmap(QPointF(0,0), m_pix);
    paintSelectionAndLabel(painter);
}

// ---------------- FrameConversionWorker -----------------

class ResizableVideoItem; // already declared in header but keep for clarity

QSet<uintptr_t> FrameConversionWorker::s_activeItems;

FrameConversionWorker::FrameConversionWorker(ResizableVideoItem* item, QVideoFrame frame, quint64 serial)
    : m_itemPtr(reinterpret_cast<uintptr_t>(item)), m_frame(frame), m_serial(serial) { setAutoDelete(true); }

void FrameConversionWorker::registerItem(ResizableVideoItem* item) { s_activeItems.insert(reinterpret_cast<uintptr_t>(item)); }
void FrameConversionWorker::unregisterItem(ResizableVideoItem* item) { s_activeItems.remove(reinterpret_cast<uintptr_t>(item)); }

void FrameConversionWorker::run() {
    if (!m_frame.isValid()) return;
    QImage img = m_frame.toImage(); if (img.isNull()) return;
    QImage converted = img.convertToFormat(QImage::Format_ARGB32_Premultiplied); if (converted.isNull()) return;
    uintptr_t itemPtr = m_itemPtr; quint64 serial = m_serial;
    QMetaObject::invokeMethod(qApp, [itemPtr, converted = std::move(converted), serial]() mutable {
        if (FrameConversionWorker::s_activeItems.contains(itemPtr)) {
            auto* item = reinterpret_cast<ResizableVideoItem*>(itemPtr);
            item->onFrameConversionComplete(std::move(converted), serial);
        }
    }, Qt::QueuedConnection);
}

// ---------------- ResizableVideoItem -----------------

ResizableVideoItem::ResizableVideoItem(const QString& filePath, int visualSizePx, int selectionSizePx, const QString& filename, int controlsFadeMs)
    : ResizableMediaBase(QSize(640,360), visualSizePx, selectionSizePx, filename)
{
    FrameConversionWorker::registerItem(this);
    m_controlsFadeMs = std::max(0, controlsFadeMs);
    m_player = new QMediaPlayer();
    m_audio = new QAudioOutput();
    m_sink = new QVideoSink();
    m_player->setAudioOutput(m_audio);
    m_player->setVideoSink(m_sink);
    m_player->setSource(QUrl::fromLocalFile(filePath));

    if (m_bottomPanel) {
        auto playBtn = m_bottomPanel->addButton("▶", "play"); if (playBtn) playBtn->setOnClicked([this]() { togglePlayPause(); });
        auto stopBtn = m_bottomPanel->addButton("■", "stop"); if (stopBtn) stopBtn->setOnClicked([this]() { stopToBeginning(); });
        auto repeatBtn = m_bottomPanel->addButton("R", "repeat"); if (repeatBtn) { repeatBtn->setOnClicked([this]() { toggleRepeat(); }); repeatBtn->setState(m_repeatEnabled ? OverlayElement::Toggled : OverlayElement::Normal); }
        auto muteBtn = m_bottomPanel->addButton("M", "mute"); if (muteBtn) { muteBtn->setOnClicked([this]() { toggleMute(); }); bool muted = m_audio && m_audio->isMuted(); muteBtn->setState(muted ? OverlayElement::Toggled : OverlayElement::Normal); }
        m_bottomPanel->setVisible(isSelected());
    }

    QObject::connect(m_sink, &QVideoSink::videoFrameChanged, m_player, [this](const QVideoFrame& f){
        ++m_framesReceived;
        if (!m_holdLastFrameAtEnd && f.isValid()) {
            if (!isVisibleInAnyView()) { ++m_framesSkipped; logFrameStats(); return; }
            {
                QMutexLocker locker(&m_frameMutex);
                m_pendingFrame = f;
            }
            if (!m_conversionBusy.exchange(true)) {
                quint64 serial = ++m_frameSerial;
                auto* worker = new FrameConversionWorker(this, f, serial);
                QThreadPool::globalInstance()->start(worker);
                ++m_conversionsStarted; ++m_framesProcessed;
            } else {
                ++m_framesSkipped;
            }
            m_lastFrame = f; maybeAdoptFrameSize(f);
            if (m_primingFirstFrame && !m_firstFramePrimed) {
                m_firstFramePrimed = true; m_primingFirstFrame = false;
                if (m_player) { m_player->pause(); m_player->setPosition(0); }
                if (m_audio) m_audio->setMuted(m_savedMuted);
                m_controlsLockedUntilReady = false; m_controlsDidInitialFade = false;
                if (isSelected()) { setControlsVisible(true); updateControlsLayout(); m_lastRepaintMs = 0; update(); return; }
            }
            logFrameStats();
        }
        if (shouldRepaint()) { m_lastRepaintMs = QDateTime::currentMSecsSinceEpoch(); update(); }
    });

    // Floating controls (absolute px background + children) created after sink connection
    m_controlsBg = new QGraphicsRectItem();
    m_controlsBg->setPen(Qt::NoPen); m_controlsBg->setBrush(Qt::NoBrush); m_controlsBg->setZValue(12000.0);
    m_controlsBg->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_controlsBg->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_controlsBg->setAcceptedMouseButtons(Qt::NoButton); m_controlsBg->setOpacity(0.0);
    if (scene()) scene()->addItem(m_controlsBg);

    auto makeSvg = [&](const char* path, QGraphicsItem* parent){ auto* svg = new QGraphicsSvgItem(path, parent); svg->setCacheMode(QGraphicsItem::DeviceCoordinateCache); svg->setZValue(12002.0); svg->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); svg->setAcceptedMouseButtons(Qt::NoButton); return svg; };
    m_playBtnRectItem = new RoundedRectItem(m_controlsBg); m_playBtnRectItem->setPen(Qt::NoPen); m_playBtnRectItem->setBrush(m_overlayStyle.backgroundBrush()); m_playBtnRectItem->setZValue(12001.0); m_playBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_playBtnRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_playIcon = makeSvg(":/icons/icons/play.svg", m_playBtnRectItem);
    m_pauseIcon = makeSvg(":/icons/icons/pause.svg", m_playBtnRectItem); m_pauseIcon->setVisible(false);
    m_stopBtnRectItem = new RoundedRectItem(m_controlsBg); m_stopBtnRectItem->setPen(Qt::NoPen); m_stopBtnRectItem->setBrush(m_overlayStyle.backgroundBrush()); m_stopBtnRectItem->setZValue(12001.0); m_stopBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_stopBtnRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_stopIcon = makeSvg(":/icons/icons/stop.svg", m_stopBtnRectItem);
    m_repeatBtnRectItem = new RoundedRectItem(m_controlsBg); m_repeatBtnRectItem->setPen(Qt::NoPen); m_repeatBtnRectItem->setBrush(m_overlayStyle.backgroundBrush()); m_repeatBtnRectItem->setZValue(12001.0); m_repeatBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_repeatBtnRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_repeatIcon = makeSvg(":/icons/icons/loop.svg", m_repeatBtnRectItem);
    m_muteBtnRectItem = new RoundedRectItem(m_controlsBg); m_muteBtnRectItem->setPen(Qt::NoPen); m_muteBtnRectItem->setBrush(m_overlayStyle.backgroundBrush()); m_muteBtnRectItem->setZValue(12001.0); m_muteBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_muteBtnRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_muteIcon = makeSvg(":/icons/icons/volume-on.svg", m_muteBtnRectItem);
    m_muteSlashIcon = makeSvg(":/icons/icons/volume-off.svg", m_muteBtnRectItem); m_muteSlashIcon->setVisible(false);
    m_volumeBgRectItem = new QGraphicsRectItem(m_controlsBg); m_volumeBgRectItem->setPen(Qt::NoPen); m_volumeBgRectItem->setBrush(m_overlayStyle.backgroundBrush()); m_volumeBgRectItem->setZValue(12001.0); m_volumeBgRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_volumeBgRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_volumeFillRectItem = new QGraphicsRectItem(m_volumeBgRectItem); m_volumeFillRectItem->setPen(Qt::NoPen); m_volumeFillRectItem->setBrush(QColor(74,144,226)); m_volumeFillRectItem->setZValue(12002.0); m_volumeFillRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_volumeFillRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_progressBgRectItem = new QGraphicsRectItem(m_controlsBg); m_progressBgRectItem->setPen(Qt::NoPen); m_progressBgRectItem->setBrush(m_overlayStyle.backgroundBrush()); m_progressBgRectItem->setZValue(12001.0); m_progressBgRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_progressBgRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_progressFillRectItem = new QGraphicsRectItem(m_progressBgRectItem); m_progressFillRectItem->setPen(Qt::NoPen); m_progressFillRectItem->setBrush(QColor(74,144,226)); m_progressFillRectItem->setZValue(12002.0); m_progressFillRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_progressFillRectItem->setAcceptedMouseButtons(Qt::NoButton);

    m_progressTimer = new QTimer(); m_progressTimer->setInterval(33);
    QObject::connect(m_progressTimer, &QTimer::timeout, [this]() {
        if (m_player && m_player->playbackState() == QMediaPlayer::PlayingState && !m_draggingProgress && !m_holdLastFrameAtEnd && !m_seeking && m_durationMs > 0) {
            qint64 currentPos = m_player->position(); qreal newRatio = static_cast<qreal>(currentPos) / m_durationMs; m_smoothProgressRatio = std::clamp<qreal>(newRatio, 0.0, 1.0); updateProgressBar(); update();
        }
    });
    m_controlsLockedUntilReady = true; m_controlsDidInitialFade = false;
    auto hide = [&](QGraphicsItem* it){ if (it) it->setVisible(false); };
    hide(m_controlsBg); hide(m_playBtnRectItem); hide(m_playIcon); hide(m_pauseIcon); hide(m_stopBtnRectItem); hide(m_stopIcon); hide(m_repeatBtnRectItem); hide(m_repeatIcon); hide(m_muteBtnRectItem); hide(m_muteIcon); hide(m_muteSlashIcon); hide(m_volumeBgRectItem); hide(m_volumeFillRectItem); hide(m_progressBgRectItem); hide(m_progressFillRectItem);

    QObject::connect(m_player, &QMediaPlayer::mediaStatusChanged, m_player, [this](QMediaPlayer::MediaStatus s){
        if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
            if (!m_adoptedSize) {
                const QMediaMetaData md = m_player->metaData();
                const QVariant v = md.value(QMediaMetaData::Resolution);
                const QSize sz = v.toSize(); if (!sz.isEmpty()) adoptBaseSize(sz);
                const QVariant thumbVar = md.value(QMediaMetaData::ThumbnailImage);
                if (!m_posterImageSet && thumbVar.isValid()) {
                    if (thumbVar.canConvert<QImage>()) { m_posterImage = thumbVar.value<QImage>(); m_posterImageSet = !m_posterImage.isNull(); }
                    else if (thumbVar.canConvert<QPixmap>()) { m_posterImage = thumbVar.value<QPixmap>().toImage(); m_posterImageSet = !m_posterImage.isNull(); }
                    if (!m_posterImageSet) {
                        const QVariant coverVar = md.value(QMediaMetaData::CoverArtImage);
                        if (coverVar.canConvert<QImage>()) { m_posterImage = coverVar.value<QImage>(); m_posterImageSet = !m_posterImage.isNull(); }
                        else if (coverVar.canConvert<QPixmap>()) { m_posterImage = coverVar.value<QPixmap>().toImage(); m_posterImageSet = !m_posterImage.isNull(); }
                    }
                    if (m_posterImageSet) update();
                }
            }
            if (!m_firstFramePrimed && !m_primingFirstFrame) {
                m_holdLastFrameAtEnd = false; m_savedMuted = m_audio ? m_audio->isMuted() : false; if (m_audio) m_audio->setMuted(true); m_primingFirstFrame = true; if (m_player) m_player->play();
            }
        }
        if (s == QMediaPlayer::EndOfMedia) {
            if (m_repeatEnabled) {
                if (m_progressTimer) m_progressTimer->stop();
                m_smoothProgressRatio = 0.0; updateProgressBar(); m_player->setPosition(0); m_player->play();
                QTimer::singleShot(10, [this]() { if (m_progressTimer && m_player && m_player->playbackState() == QMediaPlayer::PlayingState) m_progressTimer->start(); });
            } else {
                m_holdLastFrameAtEnd = true; if (m_durationMs > 0) m_positionMs = m_durationMs; m_smoothProgressRatio = 1.0; updateProgressBar(); if (m_progressTimer) m_progressTimer->stop(); updateControlsLayout(); update(); if (m_player) m_player->pause();
            }
        }
    });
    QObject::connect(m_player, &QMediaPlayer::durationChanged, m_player, [this](qint64 d){ m_durationMs = d; update(); });
    QObject::connect(m_player, &QMediaPlayer::positionChanged, m_player, [this](qint64 p){ if (m_holdLastFrameAtEnd) return; m_positionMs = p; });
}

ResizableVideoItem::~ResizableVideoItem() {
    FrameConversionWorker::unregisterItem(this);
    if (m_player) QObject::disconnect(m_player, nullptr, nullptr, nullptr);
    if (m_sink) QObject::disconnect(m_sink, nullptr, nullptr, nullptr);
    delete m_player; delete m_audio; delete m_sink; delete m_controlsFadeAnim;
    if (m_controlsBg && m_controlsBg->parentItem() == nullptr) delete m_controlsBg;
}

void ResizableVideoItem::togglePlayPause() {
    if (!m_player) return;
    if (m_player->playbackState() == QMediaPlayer::PlayingState) { m_player->pause(); if (m_progressTimer) m_progressTimer->stop(); }
    else { if (m_holdLastFrameAtEnd) { m_holdLastFrameAtEnd = false; m_positionMs = 0; m_player->setPosition(0); m_smoothProgressRatio = 0.0; updateProgressBar(); } m_player->play(); if (m_progressTimer) m_progressTimer->start(); }
    updateControlsLayout(); update();
}

void ResizableVideoItem::toggleRepeat() { m_repeatEnabled = !m_repeatEnabled; if (m_bottomPanel) { auto btn = std::dynamic_pointer_cast<OverlayButtonElement>(m_bottomPanel->findElement("repeat")); if (btn) btn->setState(m_repeatEnabled ? OverlayElement::Toggled : OverlayElement::Normal); } updateControlsLayout(); update(); }

void ResizableVideoItem::toggleMute() { if (!m_audio) return; m_audio->setMuted(!m_audio->isMuted()); bool muted = m_audio->isMuted(); if (m_bottomPanel) { auto btn = std::dynamic_pointer_cast<OverlayButtonElement>(m_bottomPanel->findElement("mute")); if (btn) btn->setState(muted ? OverlayElement::Toggled : OverlayElement::Normal); } updateControlsLayout(); update(); }

void ResizableVideoItem::stopToBeginning() { if (!m_player) return; m_holdLastFrameAtEnd = false; m_player->pause(); m_player->setPosition(0); m_positionMs = 0; m_smoothProgressRatio = 0.0; updateProgressBar(); if (m_progressTimer) m_progressTimer->stop(); updateControlsLayout(); update(); }

void ResizableVideoItem::seekToRatio(qreal r) {
    if (!m_player || m_durationMs <= 0) return; r = std::clamp<qreal>(r, 0.0, 1.0); m_holdLastFrameAtEnd = false; m_seeking = true; if (m_progressTimer) m_progressTimer->stop(); m_smoothProgressRatio = r; m_positionMs = static_cast<qint64>(r * m_durationMs); updateProgressBar(); updateControlsLayout(); update(); const qint64 pos = m_positionMs; m_player->setPosition(pos); QTimer::singleShot(30, [this]() { m_seeking = false; if (m_progressTimer && m_player && m_player->playbackState() == QMediaPlayer::PlayingState) m_progressTimer->start(); }); }

void ResizableVideoItem::setExternalPosterImage(const QImage& img) { if (!img.isNull()) { m_posterImage = img; m_posterImageSet = true; if (!m_adoptedSize) adoptBaseSize(img.size()); update(); } }

void ResizableVideoItem::updateDragWithScenePos(const QPointF& scenePos) {
    QPointF p = mapFromScene(scenePos);
    if (m_draggingProgress) { qreal r = (p.x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width(); r = std::clamp<qreal>(r, 0.0, 1.0); m_holdLastFrameAtEnd = false; seekToRatio(r); if (m_durationMs > 0) m_positionMs = static_cast<qint64>(r * m_durationMs); updateControlsLayout(); update(); }
    else if (m_draggingVolume) { qreal r = (p.x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width(); r = std::clamp<qreal>(r, 0.0, 1.0); if (m_audio) m_audio->setVolume(r); updateControlsLayout(); update(); }
}

void ResizableVideoItem::endDrag() { if (m_draggingProgress || m_draggingVolume) { m_draggingProgress = false; m_draggingVolume = false; ungrabMouse(); updateControlsLayout(); update(); } }

void ResizableVideoItem::getFrameStats(int& received, int& processed, int& skipped) const { received = m_framesReceived; processed = m_framesProcessed; skipped = m_framesSkipped; }

void ResizableVideoItem::getFrameStatsExtended(int& received, int& processed, int& skipped, int& dropped, int& conversionsStarted, int& conversionsCompleted) const { received = m_framesReceived; processed = m_framesProcessed; skipped = m_framesSkipped; dropped = m_framesDropped; conversionsStarted = m_conversionsStarted; conversionsCompleted = m_conversionsCompleted; }

void ResizableVideoItem::resetFrameStats() { m_framesReceived = m_framesProcessed = m_framesSkipped = 0; m_framesDropped = m_conversionsStarted = m_conversionsCompleted = 0; m_frameSerial = m_lastProcessedSerial = 0; }

void ResizableVideoItem::onFrameConversionComplete(QImage convertedImage, quint64 serial) {
    if (m_beingDeleted) return; // ignore late callbacks after deletion begun
    if (serial <= m_lastProcessedSerial) { ++m_framesDropped; return; }
    m_lastProcessedSerial = serial; m_lastFrameImage = std::move(convertedImage); ++m_conversionsCompleted; m_conversionBusy.store(false);
    if (m_conversionsCompleted % 30 == 0) qDebug() << "Frame conversion completed serial" << serial;
    { QMutexLocker locker(&m_frameMutex); if (m_pendingFrame.isValid() && !m_conversionBusy.exchange(true)) { quint64 newSerial = ++m_frameSerial; auto* worker = new FrameConversionWorker(this, m_pendingFrame, newSerial); QThreadPool::globalInstance()->start(worker); ++m_conversionsStarted; m_pendingFrame = QVideoFrame(); } }
    if (shouldRepaint()) { m_lastRepaintMs = QDateTime::currentMSecsSinceEpoch(); update(); }
}

bool ResizableVideoItem::handleControlsPressAtItemPos(const QPointF& itemPos) {
    if (!isSelected() || m_controlsLockedUntilReady) return false;
    if (m_playBtnRectItemCoords.contains(itemPos)) { m_holdLastFrameAtEnd = false; togglePlayPause(); return true; }
    if (m_stopBtnRectItemCoords.contains(itemPos)) { m_holdLastFrameAtEnd = false; stopToBeginning(); return true; }
    if (m_repeatBtnRectItemCoords.contains(itemPos)) { toggleRepeat(); return true; }
    if (m_muteBtnRectItemCoords.contains(itemPos)) { toggleMute(); return true; }
    if (m_progRectItemCoords.contains(itemPos)) { qreal r = (itemPos.x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width(); m_holdLastFrameAtEnd = false; seekToRatio(r); m_draggingProgress = true; grabMouse(); return true; }
    if (m_volumeRectItemCoords.contains(itemPos)) { qreal r = (itemPos.x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width(); r = std::clamp<qreal>(r, 0.0, 1.0); if (m_audio) m_audio->setVolume(r); updateControlsLayout(); m_draggingVolume = true; grabMouse(); return true; }
    return false;
}

void ResizableVideoItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
    Q_UNUSED(option); Q_UNUSED(widget);
    QRectF br(0,0, baseWidth(), baseHeight());
    auto fitRect = [](const QRectF& bounds, const QSize& imgSz) -> QRectF {
        if (bounds.isEmpty() || imgSz.isEmpty()) return bounds; qreal brW = bounds.width(); qreal brH = bounds.height(); qreal imgW = imgSz.width(); qreal imgH = imgSz.height(); if (imgW <= 0 || imgH <= 0) return bounds; qreal brAR = brW / brH; qreal imgAR = imgW / imgH; if (imgAR > brAR) { qreal h = brW / imgAR; return QRectF(bounds.left(), bounds.top() + (brH - h)/2.0, brW, h);} else { qreal w = brH * imgAR; return QRectF(bounds.left() + (brW - w)/2.0, bounds.top(), w, brH);} };
    if (!m_lastFrameImage.isNull()) { QRectF dst = fitRect(br, m_lastFrameImage.size()); painter->drawImage(dst, m_lastFrameImage); }
    else if (m_lastFrame.isValid()) { QImage img = m_lastFrame.toImage(); if (!img.isNull()) { QRectF dst = fitRect(br, img.size()); painter->drawImage(dst, img); } else if (m_posterImageSet && !m_posterImage.isNull()) { QRectF dst = fitRect(br, m_posterImage.size()); painter->drawImage(dst, m_posterImage); } }
    else if (m_posterImageSet && !m_posterImage.isNull()) { QRectF dst = fitRect(br, m_posterImage.size()); painter->drawImage(dst, m_posterImage); }
    paintSelectionAndLabel(painter);
}

QRectF ResizableVideoItem::boundingRect() const {
    QRectF br(0,0, baseWidth(), baseHeight());
    if (isSelected() && !m_controlsLockedUntilReady) {
        const int overrideH = ResizableMediaBase::getHeightOfMediaOverlaysPx();
        const int fallbackH = m_overlayStyle.defaultHeight > 0 ? m_overlayStyle.defaultHeight : 24;
        const int rowH = (overrideH > 0) ? overrideH : fallbackH; const int gapPx = 8;
        qreal extra = toItemLengthFromPixels(rowH * 2 + 2 * gapPx); br.setHeight(br.height() + extra);
    }
    if (isSelected()) { qreal pad = toItemLengthFromPixels(m_selectionSize) / 2.0; br = br.adjusted(-pad, -pad, pad, pad); }
    return br;
}

QPainterPath ResizableVideoItem::shape() const {
    QPainterPath p; p.addRect(QRectF(0,0, baseWidth(), baseHeight()));
    if (isSelected() && !m_controlsLockedUntilReady) {
        if (!m_playBtnRectItemCoords.isNull()) p.addRect(m_playBtnRectItemCoords);
        if (!m_stopBtnRectItemCoords.isNull()) p.addRect(m_stopBtnRectItemCoords);
        if (!m_repeatBtnRectItemCoords.isNull()) p.addRect(m_repeatBtnRectItemCoords);
        if (!m_muteBtnRectItemCoords.isNull()) p.addRect(m_muteBtnRectItemCoords);
        if (!m_progRectItemCoords.isNull()) p.addRect(m_progRectItemCoords);
        if (!m_volumeRectItemCoords.isNull()) p.addRect(m_volumeRectItemCoords);
    }
    if (isSelected()) { const qreal s = toItemLengthFromPixels(m_selectionSize); QRectF br(0,0, baseWidth(), baseHeight()); p.addRect(QRectF(br.topLeft() - QPointF(s/2,s/2), QSizeF(s,s))); p.addRect(QRectF(QPointF(br.right(), br.top()) - QPointF(s/2,s/2), QSizeF(s,s))); p.addRect(QRectF(QPointF(br.left(), br.bottom()) - QPointF(s/2,s/2), QSizeF(s,s))); p.addRect(QRectF(br.bottomRight() - QPointF(s/2,s/2), QSizeF(s,s))); }
    return p;
}

void ResizableVideoItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    m_activeHandle = hitTestHandle(event->pos());
    if (m_activeHandle != None) {
        m_fixedItemPoint = handlePoint(opposite(m_activeHandle)); m_fixedScenePoint = mapToScene(m_fixedItemPoint); m_initialScale = scale(); const qreal d = std::hypot(event->scenePos().x() - m_fixedScenePoint.x(), event->scenePos().y() - m_fixedScenePoint.y()); m_initialGrabDist = (d > 1e-6) ? d : 1e-6; event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_playBtnRectItemCoords.contains(event->pos())) { togglePlayPause(); event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_stopBtnRectItemCoords.contains(event->pos())) { stopToBeginning(); event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_repeatBtnRectItemCoords.contains(event->pos())) { toggleRepeat(); event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_muteBtnRectItemCoords.contains(event->pos())) { toggleMute(); event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_progRectItemCoords.contains(event->pos())) { qreal r = (event->pos().x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width(); m_seeking = true; if (m_progressTimer) m_progressTimer->stop(); seekToRatio(r); m_draggingProgress = true; grabMouse(); event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_volumeRectItemCoords.contains(event->pos())) { qreal r = (event->pos().x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width(); r = std::clamp<qreal>(r, 0.0, 1.0); if (m_audio) m_audio->setVolume(r); updateControlsLayout(); m_draggingVolume = true; grabMouse(); event->accept(); return; }
    ResizableMediaBase::mousePressEvent(event);
}

void ResizableVideoItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) {
    if (isSelected() && !m_controlsLockedUntilReady) {
        if (m_playBtnRectItemCoords.contains(event->pos())) { togglePlayPause(); event->accept(); return; }
        if (m_stopBtnRectItemCoords.contains(event->pos())) { stopToBeginning(); event->accept(); return; }
        if (m_repeatBtnRectItemCoords.contains(event->pos())) { toggleRepeat(); event->accept(); return; }
        if (m_muteBtnRectItemCoords.contains(event->pos())) { toggleMute(); event->accept(); return; }
        if (m_progRectItemCoords.contains(event->pos())) { qreal r = (event->pos().x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width(); seekToRatio(r); event->accept(); return; }
        if (m_volumeRectItemCoords.contains(event->pos())) { qreal r = (event->pos().x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width(); r = std::clamp<qreal>(r, 0.0, 1.0); if (m_audio) m_audio->setVolume(r); event->accept(); return; }
    }
    ResizableMediaBase::mouseDoubleClickEvent(event);
}

QVariant ResizableVideoItem::itemChange(GraphicsItemChange change, const QVariant &value) {
    if (change == ItemSceneChange) { if (value.value<QGraphicsScene*>() == nullptr) { if (m_controlsBg) m_controlsBg->setVisible(false); } }
    if (change == ItemSceneHasChanged) { if (scene() && m_controlsBg && m_controlsBg->scene() != scene()) { if (m_controlsBg->scene()) m_controlsBg->scene()->removeItem(m_controlsBg); scene()->addItem(m_controlsBg); } }
    if (change == ItemSelectedChange) { const bool willBeSelected = value.toBool(); prepareGeometryChange(); setControlsVisible(willBeSelected); }
    if (change == ItemSelectedHasChanged) { updateControlsLayout(); }
    if (change == ItemPositionHasChanged || change == ItemTransformHasChanged) { updateControlsLayout(); }
    return ResizableMediaBase::itemChange(change, value);
}

void ResizableVideoItem::onInteractiveGeometryChanged() { updateControlsLayout(); updateOverlayLayout(); update(); }

void ResizableVideoItem::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    if (m_activeHandle != None) { ResizableMediaBase::mouseMoveEvent(event); return; }
    if (event->buttons() & Qt::LeftButton) {
        if (m_draggingProgress) { qreal r = (event->pos().x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width(); r = std::clamp<qreal>(r, 0.0, 1.0); seekToRatio(r); updateControlsLayout(); update(); event->accept(); return; }
        if (m_draggingVolume) { qreal r = (event->pos().x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width(); r = std::clamp<qreal>(r, 0.0, 1.0); if (m_audio) m_audio->setVolume(r); updateControlsLayout(); update(); event->accept(); return; }
    }
    ResizableMediaBase::mouseMoveEvent(event);
}

void ResizableVideoItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    if (m_draggingProgress || m_draggingVolume) { m_draggingProgress = false; m_draggingVolume = false; ungrabMouse(); QTimer::singleShot(30, [this]() { m_seeking = false; if (m_progressTimer && m_player && m_player->playbackState() == QMediaPlayer::PlayingState) m_progressTimer->start(); }); event->accept(); return; }
    ResizableMediaBase::mouseReleaseEvent(event);
}

void ResizableVideoItem::prepareForDeletion() {
    ResizableMediaBase::prepareForDeletion();
    // Stop timers / animations immediately.
    if (m_progressTimer) m_progressTimer->stop();
    if (m_controlsFadeAnim) m_controlsFadeAnim->stop();
    m_draggingProgress = m_draggingVolume = false; m_seeking = false;
    // Hide controls background & children (safer than deleting here; destructor will clean up if needed).
    if (m_controlsBg) {
        if (m_controlsBg->scene()) m_controlsBg->scene()->removeItem(m_controlsBg);
        m_controlsBg->setVisible(false);
    }
}

void ResizableVideoItem::maybeAdoptFrameSize(const QVideoFrame& f) { if (m_adoptedSize) return; if (!f.isValid()) return; QImage img = f.toImage(); if (img.isNull()) return; const QSize sz = img.size(); if (sz.isEmpty()) return; adoptBaseSize(sz); }

void ResizableVideoItem::adoptBaseSize(const QSize& sz) { if (m_adoptedSize) return; if (sz.isEmpty()) return; m_adoptedSize = true; const QRectF oldRect(0,0, baseWidth(), baseHeight()); const QPointF oldCenterScene = mapToScene(oldRect.center()); prepareGeometryChange(); m_baseSize = sz; setScale(m_initialScaleFactor); const QPointF newTopLeftScene = oldCenterScene - QPointF(sz.width()*m_initialScaleFactor/2.0, sz.height()*m_initialScaleFactor/2.0); setPos(newTopLeftScene); update(); }

void ResizableVideoItem::setControlsVisible(bool show) {
    if (!m_controlsBg) return; bool allow = show && !m_controlsLockedUntilReady;
    auto setChildrenVisible = [&](bool vis){ if (m_playBtnRectItem) m_playBtnRectItem->setVisible(vis); if (m_playIcon) m_playIcon->setVisible(vis && !(m_player && m_player->playbackState() == QMediaPlayer::PlayingState)); if (m_pauseIcon) m_pauseIcon->setVisible(vis && (m_player && m_player->playbackState() == QMediaPlayer::PlayingState)); if (m_stopBtnRectItem) m_stopBtnRectItem->setVisible(vis); if (m_stopIcon) m_stopIcon->setVisible(vis); if (m_repeatBtnRectItem) m_repeatBtnRectItem->setVisible(vis); if (m_repeatIcon) m_repeatIcon->setVisible(vis); if (m_muteBtnRectItem) m_muteBtnRectItem->setVisible(vis); if (m_muteIcon) m_muteIcon->setVisible(vis); if (m_muteSlashIcon) m_muteSlashIcon->setVisible(vis && m_audio && m_audio->isMuted()); if (m_volumeBgRectItem) m_volumeBgRectItem->setVisible(vis); if (m_volumeFillRectItem) m_volumeFillRectItem->setVisible(vis); if (m_progressBgRectItem) m_progressBgRectItem->setVisible(vis); if (m_progressFillRectItem) m_progressFillRectItem->setVisible(vis); };
    if (allow) { m_controlsBg->setVisible(true); m_controlsBg->setZValue(12000.0); setChildrenVisible(true); if (!m_controlsDidInitialFade) { if (!m_controlsFadeAnim) { m_controlsFadeAnim = new QVariantAnimation(); m_controlsFadeAnim->setEasingCurve(QEasingCurve::OutCubic); QObject::connect(m_controlsFadeAnim, &QVariantAnimation::valueChanged, [this](const QVariant& v){ if (m_controlsBg) m_controlsBg->setOpacity(v.toReal()); }); } m_controlsFadeAnim->stop(); m_controlsFadeAnim->setDuration(m_controlsFadeMs); m_controlsFadeAnim->setStartValue(0.0); m_controlsFadeAnim->setEndValue(1.0); m_controlsDidInitialFade = true; m_controlsFadeAnim->start(); } else { if (m_controlsFadeAnim) m_controlsFadeAnim->stop(); m_controlsBg->setOpacity(1.0); } }
    else { if (m_controlsFadeAnim) m_controlsFadeAnim->stop(); m_controlsBg->setOpacity(0.0); m_controlsBg->setVisible(false); setChildrenVisible(false); }
}

void ResizableVideoItem::updateControlsLayout() {
    if (!scene() || scene()->views().isEmpty()) return; if (!isSelected()) return; if (m_controlsLockedUntilReady) return; QGraphicsView* v = scene()->views().first();
    const int padYpx = 8; const int gapPx = 8; const int overrideH = ResizableMediaBase::getHeightOfMediaOverlaysPx(); const int fallbackH = m_overlayStyle.defaultHeight > 0 ? m_overlayStyle.defaultHeight : 36; const int rowHpx = (overrideH > 0) ? overrideH : fallbackH; const int totalWpx = 320; const int playWpx = rowHpx; const int stopWpx = rowHpx; const int repeatWpx = rowHpx; const int muteWpx = rowHpx; const int buttonGapPx = gapPx; const int volumeWpx = std::max(0, totalWpx - (playWpx + stopWpx + repeatWpx + muteWpx) - buttonGapPx * 4); const int progWpx = totalWpx;
    auto baseBrush = m_overlayStyle.backgroundBrush(); auto blendColor = [](const QColor& a, const QColor& b, qreal t){ auto clamp255=[](int v){return std::max(0,std::min(255,v));}; int r=clamp255(int(std::round(a.red()*(1.0-t)+b.red()*t))); int g=clamp255(int(std::round(a.green()*(1.0-t)+b.green()*t))); int bch=clamp255(int(std::round(a.blue()*(1.0-t)+b.blue()*t))); return QColor(r,g,bch,a.alpha()); }; const QColor accentBlue(74,144,226,255); const qreal tintStrength = 0.33; QColor baseColor = baseBrush.color().isValid() ? baseBrush.color() : QColor(0,0,0,160); QBrush activeBrush(blendColor(baseColor, accentBlue, tintStrength));
    QPointF bottomCenterItem(baseWidth()/2.0, baseHeight()); QPointF bottomCenterScene = mapToScene(bottomCenterItem); QPointF bottomCenterView = v->viewportTransform().map(bottomCenterScene); QPointF ctrlTopLeftView = bottomCenterView + QPointF(-totalWpx/2.0, gapPx); QPointF ctrlTopLeftScene = v->viewportTransform().inverted().map(ctrlTopLeftView); QPointF ctrlTopLeftItem = mapFromScene(ctrlTopLeftScene);
    if (m_controlsBg) { m_controlsBg->setRect(0,0,totalWpx, rowHpx*2 + gapPx); m_controlsBg->setPos(ctrlTopLeftScene); }
    const qreal x0=0; const qreal x1=x0+playWpx+buttonGapPx; const qreal x2=x1+stopWpx+buttonGapPx; const qreal x3=x2+repeatWpx+buttonGapPx; const qreal x4=x3+muteWpx+buttonGapPx; auto appliedCornerRadius = m_overlayStyle.cornerRadius;
    if (m_playBtnRectItem) { m_playBtnRectItem->setRect(0,0,playWpx,rowHpx); m_playBtnRectItem->setPos(x0,0); m_playBtnRectItem->setRadius(appliedCornerRadius); m_playBtnRectItem->setBrush(baseBrush);} if (m_stopBtnRectItem) { m_stopBtnRectItem->setRect(0,0,stopWpx,rowHpx); m_stopBtnRectItem->setPos(x1,0); m_stopBtnRectItem->setRadius(appliedCornerRadius); m_stopBtnRectItem->setBrush(baseBrush);} if (m_repeatBtnRectItem) { m_repeatBtnRectItem->setRect(0,0,repeatWpx,rowHpx); m_repeatBtnRectItem->setPos(x2,0); m_repeatBtnRectItem->setRadius(appliedCornerRadius); m_repeatBtnRectItem->setBrush(m_repeatEnabled ? activeBrush : baseBrush);} if (m_muteBtnRectItem) { m_muteBtnRectItem->setRect(0,0,muteWpx,rowHpx); m_muteBtnRectItem->setPos(x3,0); m_muteBtnRectItem->setRadius(appliedCornerRadius); bool muted = m_audio && m_audio->isMuted(); m_muteBtnRectItem->setBrush(muted ? activeBrush : baseBrush);} if (m_volumeBgRectItem) { m_volumeBgRectItem->setRect(0,0,volumeWpx,rowHpx); m_volumeBgRectItem->setPos(x4,0);} if (m_volumeFillRectItem) { const qreal margin=2.0; qreal vol = m_audio ? std::clamp<qreal>(m_audio->volume(),0.0,1.0):0.0; const qreal innerW = std::max<qreal>(0.0, volumeWpx - 2*margin); m_volumeFillRectItem->setRect(margin,margin, innerW*vol, rowHpx - 2*margin);} if (m_progressBgRectItem) { m_progressBgRectItem->setRect(0,0,progWpx,rowHpx); m_progressBgRectItem->setPos(0,rowHpx+gapPx);} if (m_progressFillRectItem) { if (!m_draggingProgress) { updateProgressBar(); } else { qreal ratio = (m_durationMs>0) ? (qreal(m_positionMs)/m_durationMs) : 0.0; ratio = std::clamp<qreal>(ratio,0.0,1.0); const qreal margin=2.0; m_progressFillRectItem->setRect(margin,margin,(progWpx-2*margin)*ratio,rowHpx-2*margin);} }
    auto placeSvg = [](QGraphicsSvgItem* svg, qreal targetW, qreal targetH){ if (!svg) return; QSizeF nat = svg->renderer() ? svg->renderer()->defaultSize() : QSizeF(24,24); if (nat.width()<=0||nat.height()<=0) nat = svg->boundingRect().size(); if (nat.width()<=0||nat.height()<=0) nat = QSizeF(24,24); qreal scale = std::min(targetW/nat.width(), targetH/nat.height())*0.6; svg->setScale(scale); qreal x = (targetW - nat.width()*scale)/2.0; qreal y=(targetH - nat.height()*scale)/2.0; svg->setPos(x,y); };
    bool isPlaying = (m_player && m_player->playbackState() == QMediaPlayer::PlayingState); if (m_holdLastFrameAtEnd) isPlaying=false; if (!m_repeatEnabled && m_durationMs>0 && (m_positionMs + 30 >= m_durationMs)) isPlaying=false; if (m_playIcon && m_pauseIcon) { m_playIcon->setVisible(!isPlaying); m_pauseIcon->setVisible(isPlaying); placeSvg(m_playIcon, playWpx, rowHpx); placeSvg(m_pauseIcon, playWpx, rowHpx);} placeSvg(m_stopIcon, stopWpx, rowHpx); placeSvg(m_repeatIcon, repeatWpx, rowHpx); bool muted = m_audio && m_audio->isMuted(); if (m_muteIcon && m_muteSlashIcon) { m_muteIcon->setVisible(!muted); m_muteSlashIcon->setVisible(muted); placeSvg(m_muteIcon, muteWpx, rowHpx); placeSvg(m_muteSlashIcon, muteWpx, rowHpx);} const qreal rowHItem = toItemLengthFromPixels(rowHpx); const qreal playWItem = toItemLengthFromPixels(playWpx); const qreal stopWItem = toItemLengthFromPixels(stopWpx); const qreal repeatWItem = toItemLengthFromPixels(repeatWpx); const qreal muteWItem = toItemLengthFromPixels(muteWpx); const qreal volumeWItem = toItemLengthFromPixels(volumeWpx); const qreal progWItem = toItemLengthFromPixels(progWpx); const qreal gapItem = toItemLengthFromPixels(gapPx); const qreal x0Item = toItemLengthFromPixels(int(std::round(x0))); const qreal x1Item = toItemLengthFromPixels(int(std::round(x1))); const qreal x2Item = toItemLengthFromPixels(int(std::round(x2))); const qreal x3Item = toItemLengthFromPixels(int(std::round(x3))); const qreal x4Item = toItemLengthFromPixels(int(std::round(x4))); m_playBtnRectItemCoords = QRectF(ctrlTopLeftItem.x()+x0Item, ctrlTopLeftItem.y()+0, playWItem,rowHItem); m_stopBtnRectItemCoords = QRectF(ctrlTopLeftItem.x()+x1Item, ctrlTopLeftItem.y()+0, stopWItem,rowHItem); m_repeatBtnRectItemCoords = QRectF(ctrlTopLeftItem.x()+x2Item, ctrlTopLeftItem.y()+0, repeatWItem,rowHItem); m_muteBtnRectItemCoords = QRectF(ctrlTopLeftItem.x()+x3Item, ctrlTopLeftItem.y()+0, muteWItem,rowHItem); m_volumeRectItemCoords = QRectF(ctrlTopLeftItem.x()+x4Item, ctrlTopLeftItem.y()+0, volumeWItem,rowHItem); m_progRectItemCoords = QRectF(ctrlTopLeftItem.x()+0, ctrlTopLeftItem.y()+rowHItem+gapItem, progWItem,rowHItem);
}

bool ResizableVideoItem::isVisibleInAnyView() const { if (!scene() || scene()->views().isEmpty()) return false; auto *view = scene()->views().first(); if (!view || !view->viewport()) return false; QRectF viewportRect = view->viewport()->rect(); QRectF sceneRect = view->mapToScene(viewportRect.toRect()).boundingRect(); QRectF itemSceneRect = mapToScene(boundingRect()).boundingRect(); return sceneRect.intersects(itemSceneRect); }
bool ResizableVideoItem::shouldProcessFrame() const { const qint64 now = QDateTime::currentMSecsSinceEpoch(); return (now - m_lastFrameProcessMs) >= m_frameProcessBudgetMs; }
bool ResizableVideoItem::shouldRepaint() const { const qint64 now = QDateTime::currentMSecsSinceEpoch(); return (now - m_lastRepaintMs) >= m_repaintBudgetMs; }
void ResizableVideoItem::logFrameStats() const { if (m_framesReceived > 0 && m_framesReceived % 120 == 0) { float processRatio = float(m_framesProcessed)/float(m_framesReceived); float skipRatio = float(m_framesSkipped)/float(m_framesReceived); float dropRatio = float(m_framesDropped)/float(m_framesReceived); float conversionEfficiency = (m_conversionsStarted > 0) ? float(m_conversionsCompleted)/float(m_conversionsStarted) : 0.0f; qDebug() << "VideoItem frame stats: received=" << m_framesReceived << "processed=" << m_framesProcessed << "(" << (processRatio*100.0f) << "%)" << "skipped=" << m_framesSkipped << "(" << (skipRatio*100.0f) << "%)" << "dropped=" << m_framesDropped << "(" << (dropRatio*100.0f) << "%)" << "conversions=" << m_conversionsStarted << "/" << m_conversionsCompleted << "(" << (conversionEfficiency*100.0f) << "% efficiency)"; } }
void ResizableVideoItem::updateProgressBar() { if (!m_progressFillRectItem) return; const qreal margin = 2.0; const QRectF bgRect = m_progressBgRectItem ? m_progressBgRectItem->rect() : QRectF(); const qreal progWpx = bgRect.width(); const qreal rowH = bgRect.height(); m_progressFillRectItem->setRect(margin, margin, (progWpx - 2*margin) * m_smoothProgressRatio, rowH - 2*margin); }

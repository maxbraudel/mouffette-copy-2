// Unified overlay implementation
#include "frontend/rendering/canvas/OverlayPanels.h"
#include "backend/files/Theme.h"
#include "frontend/ui/theme/AppColors.h"
#include "frontend/rendering/canvas/RoundedRectItem.h"
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QFont>
#include <QFontMetrics>
#include <cmath>
#include <QtSvgWidgets/QGraphicsSvgItem>

// Global constants for overlay z-ordering (moved from MainWindow.cpp)
namespace {
    constexpr qreal Z_SCENE_OVERLAY = 12000.0;
    constexpr qreal Z_OVERLAY_CONTENT = 12001.0;
}

// Utility function to apply standard overlay border styling
// Helper function to apply 1px border to overlay elements
void applyOverlayBorder(QAbstractGraphicsShapeItem* item) {
    if (item) {
        QPen borderPen(AppColors::gOverlayBorderColor, 1);
        item->setPen(borderPen);
    }
}

// ============================================================================
// OverlayTextElement Implementation (new system)
// ============================================================================

OverlayTextElement::OverlayTextElement(const QString& text, const QString& id)
    : OverlayElement(Label, id), m_text(text) {}

OverlayTextElement::~OverlayTextElement() {
    delete m_background; // deletes text item as child
}

void OverlayTextElement::createGraphicsItems() {
    if (m_background) return;
    m_background = new MouseBlockingRoundedRectItem();
    applyOverlayBorder(m_background);
    m_background->setZValue(Z_SCENE_OVERLAY);
    m_background->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_background->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    // Mark as overlay so view can treat clicks as overlay (not canvas)
    m_background->setData(0, QStringLiteral("overlay"));

    m_textItem = new MouseBlockingTextItem(m_text, m_background);
    m_textItem->setZValue(Z_OVERLAY_CONTENT);
    m_textItem->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_textItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_textItem->setData(0, QStringLiteral("overlay"));
}

static QBrush overlayStateBrush(const OverlayStyle& style) {
    // Currently always normal background; extension point for states
    return style.backgroundBrush();
}

static QBrush buttonBrushForState(const OverlayStyle& style, OverlayElement::ElementState st) {
    QColor base = AppColors::gOverlayBackgroundColor;
    const QColor accent(74,144,226,255);
    auto blend=[&](const QColor&a,const QColor&b,qreal t){return QColor(
        a.red()*(1-t)+b.red()*t,
        a.green()*(1-t)+b.green()*t,
        a.blue()*(1-t)+b.blue()*t,
        a.alpha());};
    switch(st) {
        // No hover highlight: keep normal background on hover
        case OverlayElement::Hovered: return QBrush(base);
        // Pressed and toggled use the exact active background color from style
        case OverlayElement::Active: return QBrush(AppColors::gOverlayActiveBackgroundColor);
        case OverlayElement::Toggled: return QBrush(AppColors::gOverlayActiveBackgroundColor);
        case OverlayElement::Disabled: {
            QColor dim = base; dim.setAlphaF(dim.alphaF()*0.35); return QBrush(dim);
        }
        case OverlayElement::Normal:
        default: return QBrush(base);
    }
}

void OverlayTextElement::applyStyle(const OverlayStyle& style) {
    m_currentStyle = style;
    createGraphicsItems();
    if (m_background) {
        m_background->setBrush(overlayStateBrush(style));
        m_background->setRadius(style.cornerRadius);
    }
    if (m_textItem) {
        QFont f = m_textItem->font();
        f.setPixelSize(16);
        m_textItem->setFont(f);
        m_textItem->setDefaultTextColor(style.textColor);
    }
}

QSizeF OverlayTextElement::preferredSize(const OverlayStyle& style) const {
    QFont f; f.setPixelSize(16);
    QFontMetrics fm(f);
    QRect r = fm.boundingRect(m_text);
    qreal w = r.width() + 2*style.paddingX;
    qreal h = r.height() + 2*style.paddingY;
    if (style.defaultHeight > 0) {
        // Enforce uniform element height but never shrink below natural content height
        h = std::max(h, static_cast<qreal>(style.defaultHeight));
    }
    if (m_maxWidthPx > 0 && w > m_maxWidthPx) w = m_maxWidthPx;
    return QSizeF(w, h);
}

void OverlayTextElement::setSize(const QSizeF& size) {
    createGraphicsItems();
    if (m_background) {
        m_background->setRect(0,0,size.width(), size.height());
    }
    if (m_textItem) {
        // Apply elision if necessary to fit within size.width() - 2*paddingX
        QFont f = m_textItem->font();
        QFontMetrics fm(f);
        const qreal innerW = std::max<qreal>(0.0, size.width() - 2*m_currentStyle.paddingX);
        const QString display = fm.elidedText(m_text, Qt::ElideRight, static_cast<int>(innerW));
        m_textItem->setPlainText(display);
        QRectF tb = m_textItem->boundingRect();
        qreal y = (size.height()-tb.height())/2.0;
        m_textItem->setPos((size.width()-tb.width())/2.0, y);
    }
}

void OverlayTextElement::setPosition(const QPointF& pos) {
    createGraphicsItems();
    if (m_background) m_background->setPos(pos);
}

void OverlayTextElement::setVisible(bool v) {
    m_visible = v;
    if (m_background) m_background->setVisible(v);
}

void OverlayTextElement::setText(const QString& text) {
    if (m_text == text) return;
    m_text = text;
    if (m_textItem) {
        m_textItem->setPlainText(m_text);
        // re-center based on current background size
        if (m_background) {
            QRectF bg = m_background->rect();
            QRectF tb = m_textItem->boundingRect();
            m_textItem->setPos((bg.width()-tb.width())/2.0, (bg.height()-tb.height())/2.0);
        }
    }
}

// ============================================================================
// OverlayButtonElement Implementation (stub for future states)
// ============================================================================

OverlayButtonElement::OverlayButtonElement(const QString& label, const QString& id)
    : OverlayElement(Button, id), m_label(label) {}

OverlayButtonElement::~OverlayButtonElement() { delete m_background; }

void OverlayButtonElement::createGraphicsItems() {
    if (m_background) return;
    m_background = new MouseBlockingRoundedRectItem();
    applyOverlayBorder(m_background);
    m_background->setZValue(Z_SCENE_OVERLAY);
    m_background->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_background->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_background->setData(0, QStringLiteral("overlay"));
    if (m_onClicked) {
        if (m_toggleOnly) {
            // Toggle buttons: invoke action on release
            m_background->setClickCallback(nullptr);
            m_background->setPressCallback([this](bool down){ if (!down && m_onClicked) m_onClicked(); });
        } else {
            // Normal buttons: invoke action on release so press visual (Active) persists while holding
            m_background->setClickCallback(nullptr);
            // We'll wire on release together with visual press callback below
        }
    }
    // Hover / press visual feedback (skip overriding if toggled state to keep its stronger tint)
    if (!m_toggleOnly) {
        m_background->setHoverCallback([this](bool inside){
            // Do not override Active (pressed) state and do not affect toggled/disabled
            if (state() == OverlayElement::Active || state() == OverlayElement::Toggled || state() == OverlayElement::Disabled) return;
            setState(inside ? OverlayElement::Hovered : OverlayElement::Normal);
        });
        m_background->setPressCallback([this](bool down){
            if (state() == OverlayElement::Toggled || state() == OverlayElement::Disabled) return;
            if (down) {
                setState(OverlayElement::Active);
            } else {
                setState(OverlayElement::Hovered);
                if (m_onClicked) m_onClicked(); // trigger action on release
            }
        });
    }

    if (!m_label.isEmpty()) {
        m_textItem = new MouseBlockingTextItem(m_label, m_background);
        m_textItem->setZValue(Z_OVERLAY_CONTENT);
        m_textItem->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
        m_textItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        m_textItem->setData(0, QStringLiteral("overlay"));
    }

    applySegmentCorners();
}

void OverlayButtonElement::applyStyle(const OverlayStyle& style) {
    m_currentStyle = style;
    createGraphicsItems();
    if (m_background) {
        m_background->setBrush(buttonBrushForState(style, state()));
        m_background->setRadius(style.cornerRadius);
        applySegmentCorners();
    }
    if (m_textItem) {
        QFont f = m_textItem->font();
        f.setPixelSize(16);
        m_textItem->setFont(f);
        m_textItem->setDefaultTextColor(style.textColor);
    }
}

QSizeF OverlayButtonElement::preferredSize(const OverlayStyle& style) const {
    // Square: side = defaultHeight if >0, else fixed 32 to match video control buttons.
        int base = style.defaultHeight > 0 ? style.defaultHeight : 36;
    if (m_textItem || !m_label.isEmpty()) {
        QFont f; f.setPixelSize(16);
        QFontMetrics fm(f);
        QRect r = fm.boundingRect(m_label);
        int needed = r.height() + 2*style.paddingY;
        base = std::max(base, needed);
        int neededW = r.width() + 2*style.paddingX;
        base = std::max(base, neededW); // ensure square fits text width too
    }
    return QSizeF(base, base);
}

void OverlayButtonElement::setSize(const QSizeF& size) {
    createGraphicsItems();
    if (m_background) m_background->setRect(0,0,size.width(), size.height());
    applySegmentCorners();
    updateLabelPosition();
    if (m_svgIcon) {
        // Scale icon to fit ~60% of button height preserving aspect ratio
        QRectF br = m_background->rect();
        QSizeF target(br.width()*0.6, br.height()*0.6);
        QRectF viewBox = m_svgIcon->boundingRect();
        if (viewBox.width() > 0 && viewBox.height() > 0) {
            qreal sx = target.width() / viewBox.width();
            qreal sy = target.height() / viewBox.height();
            qreal s = std::min(sx, sy);
            m_svgIcon->setScale(s);
            // Center
            QSizeF scaled(viewBox.width()*s, viewBox.height()*s);
            m_svgIcon->setPos((br.width()-scaled.width())/2.0, (br.height()-scaled.height())/2.0);
        }
    }
}

void OverlayButtonElement::updateLabelPosition() {
    if (!m_textItem || !m_background) return;
    QRectF tb = m_textItem->boundingRect();
    QRectF br = m_background->rect();
    m_textItem->setPos((br.width()-tb.width())/2.0, (br.height()-tb.height())/2.0);
}

void OverlayButtonElement::setPosition(const QPointF& pos) {
    createGraphicsItems();
    if (m_background) m_background->setPos(pos);
}

void OverlayButtonElement::setVisible(bool v) {
    m_visible = v;
    if (m_background) m_background->setVisible(v);
    if (m_textItem) m_textItem->setVisible(v);
    if (m_svgIcon) m_svgIcon->setVisible(v);
}

void OverlayButtonElement::setLabel(const QString& l) {
    if (m_label == l) return;
    m_label = l;
    if (!m_textItem && !m_label.isEmpty()) {
        // Create text item lazily if label added after construction
        m_textItem = new MouseBlockingTextItem(m_label, m_background);
        m_textItem->setZValue(Z_OVERLAY_CONTENT);
        m_textItem->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
        m_textItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        m_textItem->setData(0, QStringLiteral("overlay"));
        // Reapply style to set font/color
        applyStyle(m_currentStyle);
    } else if (m_textItem) {
        m_textItem->setPlainText(m_label);
    }
    updateLabelPosition();
}

void OverlayButtonElement::setSegmentRole(SegmentRole role) {
    if (m_segmentRole == role) return;
    m_segmentRole = role;
    if (role == SegmentRole::Leading || role == SegmentRole::Middle) {
        OverlayElement::setSpacingAfter(0.0);
    } else if (spacingAfter() <= 0.0) {
        OverlayElement::setSpacingAfter(-1.0);
    }
    applySegmentCorners();
}

// Override setState to update brush dynamically
void OverlayButtonElement::setState(ElementState s) {
    if (state() == s) return;
    // In toggleOnly mode, ignore transient hover/active states; only allow Normal/Toggled/Disabled
    if (m_toggleOnly) {
        if (s == OverlayElement::Hovered || s == OverlayElement::Active) return; // ignore
    }
    OverlayElement::setState(s);
    if (m_background) m_background->setBrush(buttonBrushForState(m_currentStyle, state()));
}

void OverlayButtonElement::setSvgIcon(const QString& resourcePath) {
    createGraphicsItems();
    if (m_textItem) {
        // If label and icon both requested, prefer icon only for now
        m_textItem->setVisible(false);
    }
    if (!m_svgIcon) {
        m_svgIcon = new QGraphicsSvgItem(resourcePath, m_background);
        m_svgIcon->setZValue(Z_OVERLAY_CONTENT);
        m_svgIcon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        m_svgIcon->setData(0, QStringLiteral("overlay"));
    } else {
        // Reload by deleting and recreating (QtSvg lacks direct source change API for some builds)
        delete m_svgIcon;
        m_svgIcon = new QGraphicsSvgItem(resourcePath, m_background);
        m_svgIcon->setZValue(Z_OVERLAY_CONTENT);
        m_svgIcon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        m_svgIcon->setData(0, QStringLiteral("overlay"));
    }
    // Trigger size/layout recompute using current background size
    if (m_background) setSize(m_background->rect().size());
}

void OverlayButtonElement::applySegmentCorners() {
    if (!m_background) return;
    const qreal radius = m_currentStyle.cornerRadius;
    switch (m_segmentRole) {
        case SegmentRole::Solo:
            m_background->setCornerRadii(radius, radius, radius, radius);
            break;
        case SegmentRole::Leading:
            m_background->setCornerRadii(radius, 0.0, 0.0, radius);
            break;
        case SegmentRole::Middle:
            m_background->setCornerRadii(0.0, 0.0, 0.0, 0.0);
            break;
        case SegmentRole::Trailing:
            m_background->setCornerRadii(0.0, radius, radius, 0.0);
            break;
    }
}

// ============================================================================
// OverlaySliderElement Implementation (horizontal track/fill)
// ============================================================================

class OverlaySliderElement::SliderHandleItem : public MouseBlockingRectItem {
public:
    explicit SliderHandleItem(OverlaySliderElement* owner)
        : MouseBlockingRectItem(), m_owner(owner) {
        setAcceptedMouseButtons(Qt::LeftButton);
        setAcceptHoverEvents(true);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        MouseBlockingRectItem::mousePressEvent(event);
        if (!m_owner || event->button() != Qt::LeftButton) return;
        m_owner->beginInteraction(event->pos());
    }

    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override {
        event->accept();
        if (!m_owner) return;
        if (event->buttons() & Qt::LeftButton) {
            m_owner->continueInteraction(event->pos());
        }
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        MouseBlockingRectItem::mouseReleaseEvent(event);
        if (!m_owner || event->button() != Qt::LeftButton) return;
        m_owner->endInteraction(event->pos());
    }

private:
    OverlaySliderElement* m_owner = nullptr;
};

OverlaySliderElement::OverlaySliderElement(const QString& id)
    : OverlayElement(Slider, id) {}

OverlaySliderElement::~OverlaySliderElement() { delete m_container; }

void OverlaySliderElement::createGraphicsItems() {
    if (m_container) return;
    m_container = new SliderHandleItem(this);
    m_container->setPen(Qt::NoPen);
    m_container->setBrush(Qt::NoBrush); // transparent container
    m_container->setZValue(Z_SCENE_OVERLAY);
    m_container->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_container->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_container->setData(0, QStringLiteral("overlay"));

    m_track = new MouseBlockingRoundedRectItem(m_container);
    applyOverlayBorder(m_track);
    m_track->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_track->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_track->setData(0, QStringLiteral("overlay"));
    m_track->setAcceptedMouseButtons(Qt::NoButton);
    m_track->setAcceptHoverEvents(false);

    m_fill = new MouseBlockingRoundedRectItem(m_container);
    m_fill->setPen(Qt::NoPen);
    m_fill->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_fill->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_fill->setData(0, QStringLiteral("overlay"));
    m_fill->setAcceptedMouseButtons(Qt::NoButton);
    m_fill->setAcceptHoverEvents(false);
}

QGraphicsItem* OverlaySliderElement::graphicsItem() {
    return m_container;
}

void OverlaySliderElement::applyStyle(const OverlayStyle& style) {
    m_currentStyle = style;
    createGraphicsItems();
    if (m_track) {
        m_track->setBrush(buttonBrushForState(style, OverlayElement::Normal));
        m_track->setRadius(0.0); // Rectangular, no border radius
    }
    if (m_fill) {
        // Use stronger tint depending on state (e.g., Active brighter)
        QColor fillColor = AppColors::gOverlayActiveBackgroundColor;
        if (state() == OverlayElement::Disabled) {
            fillColor = AppColors::gOverlayBackgroundColor;
        }
        m_fill->setBrush(fillColor);
        m_fill->setRadius(0.0); // Rectangular, no border radius
    }
}

QSizeF OverlaySliderElement::preferredSize(const OverlayStyle& style) const {
    // Heuristic width: 8 * defaultHeight (or 8 * 24) to give a decent slider length
    int h = style.defaultHeight > 0 ? style.defaultHeight : 24;
    int w = h * 8;
    return QSizeF(w, h);
}

void OverlaySliderElement::setSize(const QSizeF& size) {
    createGraphicsItems();
    if (!m_container) return;
    m_container->setRect(0,0,size.width(), size.height());
    // Use full height to match button height - no vertical inset
    m_trackRect = QRectF(0, 0, size.width(), size.height());
    if (m_track) m_track->setRect(m_trackRect);
    updateFill();
}

void OverlaySliderElement::updateFill() {
    if (!m_fill) return;
    qreal w = m_trackRect.width() * std::clamp<qreal>(m_value, 0.0, 1.0);
    m_fillRect = QRectF(m_trackRect.left(), m_trackRect.top(), w, m_trackRect.height());
    m_fill->setRect(m_fillRect);
}

void OverlaySliderElement::setPosition(const QPointF& pos) {
    createGraphicsItems();
    if (m_container) m_container->setPos(pos);
}

void OverlaySliderElement::setVisible(bool v) {
    m_visible = v;
    if (m_container) m_container->setVisible(v);
}

void OverlaySliderElement::setValue(qreal v) {
    qreal clamped = std::clamp<qreal>(v, 0.0, 1.0);
    if (std::abs(clamped - m_value) < 1e-6) return;
    m_value = clamped;
    updateFill();
}

void OverlaySliderElement::setInteractionCallbacks(std::function<void(qreal)> onBegin,
                                                   std::function<void(qreal)> onUpdate,
                                                   std::function<void(qreal)> onEnd) {
    m_onBegin = std::move(onBegin);
    m_onUpdate = std::move(onUpdate);
    m_onEnd = std::move(onEnd);
    createGraphicsItems();
}

// Provide state updates for slider (mainly affects fill tint)
void OverlaySliderElement::setState(ElementState s) {
    if (state() == s) return;
    OverlayElement::setState(s);
    // Reapply style to update tint intensities based on new state
    applyStyle(m_currentStyle);
    // Preserve geometry after style reapplication
    updateFill();
}

void OverlaySliderElement::beginInteraction(const QPointF& localPos) {
    m_dragging = true;
    setState(OverlayElement::Active);
    const qreal newValue = valueFromLocalPos(localPos);
    setValue(newValue);
    if (m_onBegin) m_onBegin(m_value);
    // Note: setValue already triggered m_onUpdate, no need to call it again
}

void OverlaySliderElement::continueInteraction(const QPointF& localPos) {
    if (!m_dragging) return;
    const qreal newValue = valueFromLocalPos(localPos);
    setValue(newValue);
    if (m_onUpdate) m_onUpdate(m_value);
}

void OverlaySliderElement::endInteraction(const QPointF& localPos) {
    if (!m_dragging) return;
    const qreal newValue = valueFromLocalPos(localPos);
    setValue(newValue);
    if (m_onUpdate) m_onUpdate(m_value);
    if (m_onEnd) m_onEnd(m_value);
    m_dragging = false;
    setState(OverlayElement::Normal);
}

qreal OverlaySliderElement::valueFromLocalPos(const QPointF& localPos) const {
    if (m_trackRect.width() <= 0.0) return 0.0;
    qreal ratio = (localPos.x() - m_trackRect.left()) / m_trackRect.width();
    return std::clamp<qreal>(ratio, 0.0, 1.0);
}

// ============================================================================
// OverlayPanel Implementation
// ============================================================================

OverlayPanel::OverlayPanel(Position position, Layout layout)
    : m_position(position), m_layout(layout) {}

OverlayPanel::~OverlayPanel() { delete m_background; }

void OverlayPanel::setLayout(Layout layout) {
    if (m_layout != layout) {
        m_layout = layout;
        updateLabelsLayout();
    }
}

void OverlayPanel::setStyle(const OverlayStyle& style) {
    m_style = style;
    updateBackground();
    for (auto& element : m_elements) {
        element->applyStyle(style);
    }
}

//=============================================================================
// OverlayPanel Element Management (New System)
//=============================================================================

void OverlayPanel::addElement(std::shared_ptr<OverlayElement> element) {
    if (!element) return;
    
    m_elements.append(element);
    
    // Apply current style to the new element
    element->applyStyle(m_style);
    
    // Ensure background container exists so we can layout in local coordinates
    if (!m_background) {
        createBackground();
        // If we have a scene but background not yet added, add it now
        if (m_scene && !m_background->scene() && !m_parentItem) {
            m_scene->addItem(m_background);
        }
    }

    // Parent graphics item under background (preferred) or fallback to scene/parent item
    if (auto* graphicsItem = element->graphicsItem()) {
        if (m_background) {
            graphicsItem->setParentItem(m_background);
        } else if (m_parentItem) {
            graphicsItem->setParentItem(m_parentItem);
        } else if (m_scene) {
            m_scene->addItem(graphicsItem);
        }
    }
    
    // Recompute layout (size + child positions) if we already know view anchor later
    updateLabelsLayout();
}

// Convenience helpers -------------------------------------------------------
std::shared_ptr<OverlayTextElement> OverlayPanel::addText(const QString& text, const QString& id) {
    auto el = std::make_shared<OverlayTextElement>(text, id);
    addElement(el);
    return el;
}

std::shared_ptr<OverlayButtonElement> OverlayPanel::addButton(const QString& label, const QString& id) {
    auto el = std::make_shared<OverlayButtonElement>(label, id);
    addElement(el);
    return el;
}

std::shared_ptr<OverlaySliderElement> OverlayPanel::addSlider(const QString& id) {
    auto el = std::make_shared<OverlaySliderElement>(id);
    addElement(el);
    return el;
}

void OverlayPanel::newRow() {
    addElement(std::make_shared<RowBreakElement>());
}

void OverlayPanel::removeElement(const QString& id) {
    auto it = std::find_if(m_elements.begin(), m_elements.end(),
                          [&id](const std::shared_ptr<OverlayElement>& element) {
                              return element->id() == id;
                          });
    if (it != m_elements.end()) {
        m_elements.erase(it);
        updateLabelsLayout();
    }
}

void OverlayPanel::removeElement(std::shared_ptr<OverlayElement> element) {
    if (m_elements.removeOne(element)) {
        updateLabelsLayout();
    }
}

void OverlayPanel::clearElements() {
    m_elements.clear();
    updateLabelsLayout();
}

std::shared_ptr<OverlayElement> OverlayPanel::findElement(const QString& id) const {
    auto it = std::find_if(m_elements.begin(), m_elements.end(),
                          [&id](const std::shared_ptr<OverlayElement>& element) {
                              return element->id() == id;
                          });
    return (it != m_elements.end()) ? *it : nullptr;
}

std::shared_ptr<OverlayButtonElement> OverlayPanel::getButton(const QString& id) const {
    auto element = findElement(id);
    return std::dynamic_pointer_cast<OverlayButtonElement>(element);
}

std::shared_ptr<OverlaySliderElement> OverlayPanel::getSlider(const QString& id) const {
    auto element = findElement(id);
    return std::dynamic_pointer_cast<OverlaySliderElement>(element);
}

void OverlayPanel::addStandardVideoControls(const VideoControlCallbacks& callbacks) {
    // Row 1: Play/Pause, Stop, Repeat buttons, then Mute + Volume slider
    auto playPause = addButton(QString(), QStringLiteral("play-pause"));
    if (playPause) {
        playPause->setSvgIcon(":/icons/icons/play.svg");
        playPause->setSegmentRole(OverlayButtonElement::SegmentRole::Solo);
        if (callbacks.onPlayPause) playPause->setOnClicked(callbacks.onPlayPause);
    }

    auto stop = addButton(QString(), QStringLiteral("stop"));
    if (stop) {
        stop->setSvgIcon(":/icons/icons/stop.svg");
        stop->setSegmentRole(OverlayButtonElement::SegmentRole::Solo);
        if (callbacks.onStop) stop->setOnClicked(callbacks.onStop);
    }

    auto repeat = addButton(QString(), QStringLiteral("repeat"));
    if (repeat) {
        repeat->setSvgIcon(":/icons/icons/loop.svg");
        repeat->setSegmentRole(OverlayButtonElement::SegmentRole::Solo);
        repeat->setToggleOnly(true);
        repeat->setSpacingAfter(m_style.itemSpacing);
        if (callbacks.onRepeat) repeat->setOnClicked(callbacks.onRepeat);
    }

    auto mute = addButton(QString(), QStringLiteral("mute"));
    if (mute) {
        mute->setSvgIcon(":/icons/icons/volume-on.svg");
        mute->setSegmentRole(OverlayButtonElement::SegmentRole::Solo);
        mute->setToggleOnly(true);
        mute->setSpacingAfter(m_style.itemSpacing);
        if (callbacks.onMute) mute->setOnClicked(callbacks.onMute);
    }

    auto volume = addSlider(QStringLiteral("volume"));
    if (volume && callbacks.onVolumeBegin && callbacks.onVolumeUpdate && callbacks.onVolumeEnd) {
        volume->setInteractionCallbacks(callbacks.onVolumeBegin, callbacks.onVolumeUpdate, callbacks.onVolumeEnd);
    }

    // Row 2: Progress bar slider spanning full width
    newRow();

    auto progress = addSlider(QStringLiteral("progress"));
    if (progress && callbacks.onProgressBegin && callbacks.onProgressUpdate && callbacks.onProgressEnd) {
        progress->setInteractionCallbacks(callbacks.onProgressBegin, callbacks.onProgressUpdate, callbacks.onProgressEnd);
    }
}

// (Legacy label management removed)

void OverlayPanel::setVisible(bool visible) {
    if (m_visible == visible) return;
    m_visible = visible;
    if (m_background) m_background->setVisible(visible);
    for (auto &e : m_elements) e->setVisible(visible);
    // If we're becoming visible and we have a cached anchor + view, force a relayout to avoid initial offset
    if (m_visible && m_hasLastAnchor && m_lastView) {
        updateLayoutWithAnchor(m_lastAnchorScenePoint, m_lastView);
    }
}

void OverlayPanel::setParentItem(QGraphicsItem* parent) {
    m_parentItem = parent;
    if (m_background) {
        m_background->setParentItem(parent);
    }
}

void OverlayPanel::setScene(QGraphicsScene* scene) {
    m_scene = scene;
    if (m_background && !m_parentItem && scene) {
        scene->addItem(m_background);
    }
    
    // Add any existing elements to the scene if they have no parent/scene
    if (scene) {
        for (auto& element : m_elements) {
            QGraphicsItem* gi = element->graphicsItem();
            if (gi && !gi->scene() && !gi->parentItem()) scene->addItem(gi);
        }
    }
    // If scene just set and we have cached anchor+view and visible, relayout now
    if (m_scene && m_visible && m_hasLastAnchor && m_lastView) {
        updateLayoutWithAnchor(m_lastAnchorScenePoint, m_lastView);
    }
}

void OverlayPanel::clearBackgroundVisibilityOverride() {
    if (!m_backgroundVisibilityOverridden) return;
    m_backgroundVisibilityOverridden = false;
    m_backgroundVisible = (m_position != Top);
    updateBackground();
}

void OverlayPanel::updateLayoutWithAnchor(const QPointF& anchorScenePoint, QGraphicsView* view) {
    if (!view || m_elements.isEmpty()) return;
    // Cache parameters for potential deferred relayout when becoming visible
    m_lastAnchorScenePoint = anchorScenePoint;
    m_lastView = view;
    m_hasLastAnchor = true;
    // If currently not visible we still cache but skip heavy work to avoid flicker
    if (!m_visible) return;
    m_currentSize = calculateSize();
    m_currentPosition = calculatePanelPositionFromAnchor(anchorScenePoint, view);
    
    // Ensure background exists and is positioned like video controls background
    if (!m_background) {
        createBackground();
    }

    // For top panels we never render a shared background rect; for others keep it.
    if (!m_backgroundVisibilityOverridden) {
        m_backgroundVisible = (m_position != Top);
    }
    updateBackground();
    updateLabelsLayout();
}


QSizeF OverlayPanel::calculateSize() const {
    if (m_elements.isEmpty()) return QSizeF(0, 0);
    if (m_layout == Vertical) {
        // Vertical layout unchanged (single column stacking)
        qreal totalW = 0, totalH = 0; int count = 0;
        for (const auto &e : m_elements) if (e->isVisible()) {
            QSizeF sz = e->preferredSize(m_style);
            totalW = std::max(totalW, sz.width());
            totalH += sz.height();
            ++count;
        }
        if (count > 1) totalH += (count-1)*m_style.itemSpacing;
        totalW += 2*m_style.paddingX;
        totalH += 2*m_style.paddingY;
        return QSizeF(totalW, totalH);
    }
    // Horizontal layout with possible explicit row breaks
    QList<qreal> rowWidths; rowWidths.append(0.0);
    QList<qreal> rowHeights; rowHeights.append(0.0);
    QList<int> rowCounts; rowCounts.append(0);
    QList<qreal> rowPendingSpacing; rowPendingSpacing.append(m_style.itemSpacing);
    int r = 0;
    for (const auto &e : m_elements) {
        if (e->type() == OverlayElement::RowBreak) {
            // Only start a new row if current row has elements (avoid empty duplicates)
            if (rowCounts[r] == 0) continue;
            rowWidths.append(0.0);
            rowHeights.append(0.0);
            rowCounts.append(0);
            rowPendingSpacing.append(m_style.itemSpacing);
            ++r;
            continue;
        }
        if (!e->isVisible()) continue;
        QSizeF sz = e->preferredSize(m_style);
        if (rowCounts[r] > 0) {
            const qreal spacing = rowPendingSpacing[r] >= 0.0 ? rowPendingSpacing[r] : m_style.itemSpacing;
            rowWidths[r] += spacing;
        }
        rowWidths[r] += sz.width();
        rowHeights[r] = std::max(rowHeights[r], sz.height());
        rowCounts[r]++;
        rowPendingSpacing[r] = (e->spacingAfter() >= 0.0) ? e->spacingAfter() : m_style.itemSpacing;
    }
    // Aggregate size
    qreal panelW = 0, panelH = 0;
    for (int i=0;i<rowWidths.size();++i) {
        panelW = std::max(panelW, rowWidths[i]);
        panelH += rowHeights[i];
        if (i+1 < rowWidths.size()) panelH += m_style.itemSpacing; // gap between rows
    }
    panelW += 2*m_style.paddingX;
    panelH += 2*m_style.paddingY;
    if (m_style.maxWidth > 0) panelW = std::min(panelW, static_cast<qreal>(m_style.maxWidth));
    return QSizeF(panelW, panelH);
}

void OverlayPanel::createBackground() {
    m_background = new MouseBlockingRectItem();
    m_background->setPen(Qt::NoPen);
    m_background->setZValue(m_style.zOverlay);
    m_background->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_background->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_background->setData(0, QStringLiteral("overlay"));
    updateBackground();
    // Re-parent existing elements so layout can be done in local coords
    for (auto &element : m_elements) {
        if (auto *gi = element->graphicsItem()) {
            if (!gi->parentItem()) gi->setParentItem(m_background);
        }
    }
    // elements parented already
}

void OverlayPanel::updateBackground() {
    if (!m_background) return;
    // No rectangular background behind overlay panels—individual elements provide their own styling.
    // This gives a cleaner look without the "ugly background" rectangle.
    m_background->setBrush(Qt::NoBrush);
    m_background->setData(0, QVariant());
    // Let child elements receive clicks (buttons, sliders)
    m_background->setAcceptedMouseButtons(Qt::NoButton);
    
    m_background->setRect(0, 0, m_currentSize.width(), m_currentSize.height());
    m_background->setPos(m_currentPosition);
}

void OverlayPanel::updateLabelsLayout() {
    if (m_elements.isEmpty()) return;
    const bool haveContainer = (m_background != nullptr);
    if (m_layout == Vertical) {
        QPointF currentPos(m_style.paddingX, m_style.paddingY);
        for (auto &element : m_elements) {
            if (!element->isVisible()) continue;
            if (element->type() == OverlayElement::RowBreak) continue; // ignore row breaks in vertical mode
            QSizeF elementSize = element->preferredSize(m_style);
            element->setSize(elementSize);
            if (haveContainer) {
                if (auto *gi = element->graphicsItem()) {
                    if (gi->parentItem() != m_background) gi->setParentItem(m_background);
                }
                element->setPosition(currentPos);
            } else {
                element->setPosition(m_currentPosition + currentPos);
            }
            const qreal spacing = (element->spacingAfter() >= 0.0) ? element->spacingAfter() : m_style.itemSpacing;
            currentPos.setY(currentPos.y() + elementSize.height() + spacing);
        }
    } else {
        // Horizontal with potential row breaks
        qreal panelInnerW = m_currentSize.width() - 2*m_style.paddingX;
        QPointF origin(m_style.paddingX, m_style.paddingY);
        qreal cursorX = origin.x();
        qreal cursorY = origin.y();
        qreal currentRowMaxH = 0;
        qreal pendingSpacing = 0.0;
        bool firstInRow = true;

        // Detect special two-row top panel pattern: first row = single text element, then row break, then buttons.
        bool specialTwoRowTop = false;
        if (m_position == Top) {
            int rowBreakIndex = -1;
            int visibleBeforeBreak = 0;
            OverlayTextElement* firstText = nullptr;
            for (int i=0;i<m_elements.size();++i) {
                auto &el = m_elements[i];
                if (!el->isVisible()) continue;
                if (el->type() == OverlayElement::RowBreak) { rowBreakIndex = i; break; }
                ++visibleBeforeBreak;
                if (visibleBeforeBreak == 1) firstText = dynamic_cast<OverlayTextElement*>(el.get());
                else firstText = nullptr; // more than one element before break
            }
            if (rowBreakIndex >=0 && firstText && visibleBeforeBreak==1) {
                // Count buttons after break
                qreal buttonsRowWidth = 0;
                qreal buttonsRowHeight = 0;
                qreal buttonsPendingSpacing = 0.0;
                bool buttonsFirst = true;
                for (int j=rowBreakIndex+1;j<m_elements.size();++j) {
                    auto &el = m_elements[j];
                    if (!el->isVisible()) continue;
                    if (el->type() == OverlayElement::RowBreak) break; // only first row after break considered
                    QSizeF sz = el->preferredSize(m_style);
                    if (!buttonsFirst) {
                        const qreal spacing = buttonsPendingSpacing >= 0.0 ? buttonsPendingSpacing : m_style.itemSpacing;
                        buttonsRowWidth += spacing;
                    }
                    buttonsRowWidth += sz.width();
                    buttonsRowHeight = std::max(buttonsRowHeight, sz.height());
                    buttonsPendingSpacing = (el->spacingAfter() >= 0.0) ? el->spacingAfter() : m_style.itemSpacing;
                    buttonsFirst = false;
                }
                if (!buttonsFirst) {
                    // Force the filename text element width to match the buttons row width exactly.
                    // If the natural text is wider it will be elided; if narrower it is expanded to align edges.
                    QSizeF textPref = firstText->preferredSize(m_style);
                    firstText->setSize(QSizeF(buttonsRowWidth, textPref.height()));
                    // Ensure the overall panel width matches this new enforced width so centering math stays correct.
                    qreal desiredPanelWidth = buttonsRowWidth + 2*m_style.paddingX;
                    if (std::abs(desiredPanelWidth - m_currentSize.width()) > 0.5) {
                        // Adjust panel width and shift panel position left/right so its visual center is preserved.
                        qreal oldWidth = m_currentSize.width();
                        m_currentSize.setWidth(desiredPanelWidth);
                        qreal deltaW = desiredPanelWidth - oldWidth;
                        // Shift current panel position left by half the added width so anchor center remains stable.
                        m_currentPosition.rx() -= deltaW / 2.0;
                        if (m_background) {
                            m_background->setRect(0, 0, m_currentSize.width(), m_currentSize.height());
                            m_background->setPos(m_currentPosition);
                        }
                    }
                    specialTwoRowTop = true;
                    // Layout first row explicitly, then second row.
                    if (haveContainer) {
                        if (auto *gi = firstText->graphicsItem()) {
                            if (gi->parentItem() != m_background) gi->setParentItem(m_background);
                        }
                        firstText->setPosition(QPointF(m_style.paddingX, m_style.paddingY));
                    } else {
                        firstText->setPosition(m_currentPosition + QPointF(m_style.paddingX, m_style.paddingY));
                    }
                    qreal ySecond = m_style.paddingY + firstText->preferredSize(m_style).height() + m_style.itemSpacing;
                    QPointF btnCursor(m_style.paddingX, ySecond);
                    qreal rowMaxH = 0;
                    qreal spacingBefore = 0.0;
                    bool firstInSecondRow = true;
                    for (int j=rowBreakIndex+1;j<m_elements.size();++j) {
                        auto &el = m_elements[j];
                        if (!el->isVisible()) continue;
                        if (el->type() == OverlayElement::RowBreak) break;
                        QSizeF sz = el->preferredSize(m_style);
                        if (firstInSecondRow) {
                            firstInSecondRow = false;
                        } else {
                            btnCursor.setX(btnCursor.x() + spacingBefore);
                        }
                        el->setSize(sz);
                        if (haveContainer) {
                            if (auto *gi = el->graphicsItem()) {
                                if (gi->parentItem() != m_background) gi->setParentItem(m_background);
                            }
                            el->setPosition(btnCursor);
                        } else {
                            el->setPosition(m_currentPosition + btnCursor);
                        }
                        btnCursor.setX(btnCursor.x() + sz.width());
                        spacingBefore = (el->spacingAfter() >= 0.0) ? el->spacingAfter() : m_style.itemSpacing;
                        rowMaxH = std::max(rowMaxH, sz.height());
                    }
                }
            }
        }

        if (!specialTwoRowTop) {
            for (auto &element : m_elements) {
                if (element->type() == OverlayElement::RowBreak) {
                    // New row only if something placed in current row
                    if (!firstInRow) {
                        origin.setY(origin.y() + currentRowMaxH + m_style.itemSpacing);
                        cursorX = m_style.paddingX;
                        cursorY = origin.y();
                        currentRowMaxH = 0;
                        pendingSpacing = 0.0;
                        firstInRow = true;
                    }
                    continue;
                }
                if (!element->isVisible()) continue;
                QSizeF elementSize = element->preferredSize(m_style);
                // Constrain width if exceeding panel width (rare)
                if (m_style.maxWidth > 0) {
                    qreal maxInner = std::min(panelInnerW, static_cast<qreal>(m_style.maxWidth - 2*m_style.paddingX));
                    if (elementSize.width() > maxInner) elementSize.setWidth(maxInner);
                }

                qreal spacingBefore = 0.0;
                if (!firstInRow) {
                    spacingBefore = (pendingSpacing >= 0.0) ? pendingSpacing : m_style.itemSpacing;
                }
                const qreal elementX = cursorX + spacingBefore;

                if (element->type() == OverlayElement::Slider) {
                    const QString elementId = element->id();
                    const qreal currentOffset = elementX - m_style.paddingX;
                    const qreal remaining = std::max<qreal>(0.0, panelInnerW - currentOffset);
                    if (elementId == QLatin1String("progress")) {
                        elementSize.setWidth(remaining); // remaining == panelInnerW when offset is zero
                    } else {
                        elementSize.setWidth(remaining);
                    }
                }

                element->setSize(elementSize);
                QPointF elementPos(elementX, cursorY);
                if (haveContainer) {
                    if (auto *gi = element->graphicsItem()) {
                        if (gi->parentItem() != m_background) gi->setParentItem(m_background);
                    }
                    element->setPosition(elementPos);
                } else {
                    element->setPosition(m_currentPosition + elementPos);
                }
                cursorX = elementX + elementSize.width();
                pendingSpacing = (element->spacingAfter() >= 0.0) ? element->spacingAfter() : m_style.itemSpacing;
                currentRowMaxH = std::max(currentRowMaxH, elementSize.height());
                firstInRow = false;
            }
        }
        // If we used the special two-row top layout, ensure final background rect and position reflect possibly updated m_currentSize while keeping anchor center.
        if (specialTwoRowTop && m_position == Top) {
            // Recompute horizontal centering relative to anchor: anchor viewport X center should align with panel center.
            if (m_lastView) {
                const QTransform &vt = m_lastView->viewportTransform();
                QPointF anchorViewport = vt.map(m_lastAnchorScenePoint);
                QPointF panelTopLeftViewport(anchorViewport.x() - m_currentSize.width()/2.0, vt.map(m_currentPosition).y());
                // Map back without altering vertical (we keep existing vertical in viewport space)
                QPointF newTopLeftScene = vt.inverted().map(panelTopLeftViewport);
                qreal dx = newTopLeftScene.x() - m_currentPosition.x();
                if (std::abs(dx) > 0.1) {
                    m_currentPosition.setX(newTopLeftScene.x());
                    if (m_background) m_background->setPos(m_currentPosition);
                }
            }
        }
    }
    // (Shrinking background for single text no longer needed – top panels have no shared background)
}


QPointF OverlayPanel::calculatePanelPositionFromAnchor(const QPointF& anchorScenePoint, QGraphicsView* view) const {
    if (!view) return QPointF();
    
    // Use EXACT same logic as video controls: work in viewport pixels for gap, then map back
    const QTransform &vt = view->viewportTransform();
    QPointF anchorViewport = vt.map(anchorScenePoint);
    
    QPointF panelTopLeftViewport;
    if (m_position == Top) {
        // Position above anchor: gap pixels up from anchor point. To make perceived top/bottom spacing look equal,
        // we subtract only (gap + height - paddingY) so the visual empty space outside the panel matches bottom gap.
        qreal effectiveGap = std::max<qreal>(0.0, m_style.gap - m_style.paddingY);
        panelTopLeftViewport = anchorViewport + QPointF(-m_currentSize.width() / 2.0, -(effectiveGap + m_currentSize.height()));
    } else {
        // Position below anchor: gap pixels down from anchor point  
        // Panel top-left = anchor - (width/2, -gap) in viewport pixels
        panelTopLeftViewport = anchorViewport + QPointF(-m_currentSize.width() / 2.0, m_style.gap);
    }
    
    // Map back to scene coordinates - this ensures pixel-perfect gap regardless of zoom
    return vt.inverted().map(panelTopLeftViewport);
}


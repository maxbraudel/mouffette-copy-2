// Unified overlay implementation
#include "OverlayPanels.h"
#include "RoundedRectItem.h"
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
    m_background->setPen(Qt::NoPen);
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
    QColor base = style.backgroundColor;
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
        case OverlayElement::Active: return QBrush(style.activeBackgroundColor);
        case OverlayElement::Toggled: return QBrush(style.activeBackgroundColor);
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
    return QSizeF(w, h);
}

void OverlayTextElement::setSize(const QSizeF& size) {
    createGraphicsItems();
    if (m_background) {
        m_background->setRect(0,0,size.width(), size.height());
    }
    if (m_textItem) {
        QRectF tb = m_textItem->boundingRect();
        // If a defaultHeight is enforced, we rely on m_currentStyle for vertical centering; otherwise natural center
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
    m_background->setPen(Qt::NoPen);
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
}

void OverlayButtonElement::applyStyle(const OverlayStyle& style) {
    m_currentStyle = style;
    createGraphicsItems();
    if (m_background) {
        m_background->setBrush(buttonBrushForState(style, state()));
        m_background->setRadius(style.cornerRadius);
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
        m_svgIcon->setElementId(QString());
    }
    // Trigger size/layout recompute using current background size
    if (m_background) setSize(m_background->rect().size());
}

// ============================================================================
// OverlaySliderElement Implementation (horizontal track/fill)
// ============================================================================

OverlaySliderElement::OverlaySliderElement(const QString& id)
    : OverlayElement(Slider, id) {}

OverlaySliderElement::~OverlaySliderElement() { delete m_container; }

void OverlaySliderElement::createGraphicsItems() {
    if (m_container) return;
    m_container = new MouseBlockingRectItem();
    m_container->setPen(Qt::NoPen);
    m_container->setBrush(Qt::NoBrush); // transparent container
    m_container->setZValue(Z_SCENE_OVERLAY);
    m_container->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_container->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_container->setData(0, QStringLiteral("overlay"));

    m_track = new MouseBlockingRoundedRectItem(m_container);
    m_track->setPen(Qt::NoPen);
    m_track->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_track->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_track->setData(0, QStringLiteral("overlay"));

    m_fill = new MouseBlockingRoundedRectItem(m_container);
    m_fill->setPen(Qt::NoPen);
    m_fill->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_fill->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_fill->setData(0, QStringLiteral("overlay"));
}

void OverlaySliderElement::applyStyle(const OverlayStyle& style) {
    m_currentStyle = style;
    createGraphicsItems();
    if (m_track) {
        QColor trackColor = style.backgroundColor;
        trackColor.setAlphaF(std::min(1.0, trackColor.alphaF() * 0.55)); // lighter track
        m_track->setBrush(trackColor);
        m_track->setRadius(style.cornerRadius);
    }
    if (m_fill) {
        // Use stronger tint depending on state (e.g., Active brighter)
        qreal t = 0.6;
        if (state() == OverlayElement::Hovered) t = 0.7;
        else if (state() == OverlayElement::Active) t = 0.85;
        else if (state() == OverlayElement::Disabled) t = 0.25;
        else if (state() == OverlayElement::Toggled) t = 0.9;
        m_fill->setBrush(style.tintedBackgroundBrush(t));
        m_fill->setRadius(style.cornerRadius);
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
    // Track occupies full height minus vertical padding fraction (use paddingY to inset slightly)
    qreal insetY = std::min<qreal>(m_currentStyle.paddingY, size.height()/4.0);
    qreal trackH = size.height() - 2*insetY;
    m_trackRect = QRectF(0, insetY, size.width(), trackH);
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

// Provide state updates for slider (mainly affects fill tint)
void OverlaySliderElement::setState(ElementState s) {
    if (state() == s) return;
    OverlayElement::setState(s);
    // Reapply style to update tint intensities based on new state
    applyStyle(m_currentStyle);
    // Preserve geometry after style reapplication
    updateFill();
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

    // Decide visual background presence: if only a single text element on a top panel, hide visual fill
    if (m_position == Top) {
        int visibleElements = 0;
        bool singleText = false;
        for (const auto &e : m_elements) {
            if (!e->isVisible()) continue;
            ++visibleElements;
            if (visibleElements == 1 && dynamic_cast<OverlayTextElement*>(e.get())) singleText = true;
            if (visibleElements > 1) break;
        }
        m_backgroundVisible = !(visibleElements == 1 && singleText);
    } else {
        m_backgroundVisible = true;
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
    int r = 0;
    for (const auto &e : m_elements) {
        if (e->type() == OverlayElement::RowBreak) {
            // Only start a new row if current row has elements (avoid empty duplicates)
            if (rowCounts[r] == 0) continue;
            rowWidths.append(0.0); rowHeights.append(0.0); rowCounts.append(0); ++r; continue;
        }
        if (!e->isVisible()) continue;
        QSizeF sz = e->preferredSize(m_style);
        if (rowCounts[r] > 0) rowWidths[r] += m_style.itemSpacing;
        rowWidths[r] += sz.width();
        rowHeights[r] = std::max(rowHeights[r], sz.height());
        rowCounts[r]++;
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
    // For the top media overlay (filename + settings) we do NOT want any rectangular background.
    // Only the individual elements (text/button) should be visible. So force a transparent brush
    // and do not tag the whole rect as an overlay hit target. Bottom/other panels retain previous logic.
    if (m_position == Top) {
        m_background->setBrush(Qt::NoBrush);
        m_background->setData(0, QVariant());
        // Let child elements receive clicks (filename text, settings button)
        m_background->setAcceptedMouseButtons(Qt::NoButton);
    } else {
        // Existing behavior for non-top panels (e.g. video controls)
        if (m_backgroundVisible) {
            m_background->setBrush(m_style.backgroundBrush());
            m_background->setData(0, QStringLiteral("overlay"));
        } else {
            m_background->setBrush(Qt::NoBrush);
            m_background->setData(0, QVariant());
        }
    }
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
            currentPos.setY(currentPos.y() + elementSize.height() + m_style.itemSpacing);
        }
    } else {
        // Horizontal with potential row breaks
        qreal panelInnerW = m_currentSize.width() - 2*m_style.paddingX;
        QPointF origin(m_style.paddingX, m_style.paddingY);
        QPointF cursor = origin;
        qreal currentRowMaxH = 0;
        for (auto &element : m_elements) {
            if (element->type() == OverlayElement::RowBreak) {
                // New row only if something placed in current row
                if (cursor.x() != origin.x()) {
                    // advance to next row
                    origin.setY(origin.y() + currentRowMaxH + m_style.itemSpacing);
                    cursor = QPointF(m_style.paddingX, origin.y());
                    currentRowMaxH = 0;
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
            element->setSize(elementSize);
            if (haveContainer) {
                if (auto *gi = element->graphicsItem()) {
                    if (gi->parentItem() != m_background) gi->setParentItem(m_background);
                }
                element->setPosition(cursor);
            } else {
                element->setPosition(m_currentPosition + cursor);
            }
            cursor.setX(cursor.x() + elementSize.width() + m_style.itemSpacing);
            currentRowMaxH = std::max(currentRowMaxH, elementSize.height());
            // If next element is row break we'll wrap automatically on next iteration
        }
    }
    // Special case: if top panel with a single visible text element and background hidden,
    // shrink the background rect to match that element's bounding rect so the hit area is tight.
    if (m_position == Top && !m_backgroundVisible) {
        int visibleCount = 0;
        OverlayTextElement* onlyText = nullptr;
        for (auto &e : m_elements) {
            if (!e->isVisible()) continue;
            ++visibleCount;
            if (visibleCount == 1) {
                onlyText = dynamic_cast<OverlayTextElement*>(e.get());
            } else {
                onlyText = nullptr; break;
            }
        }
        if (onlyText && m_background) {
            // Use element size without extra padding (background already positioned at m_currentPosition)
            QSizeF tightSize = onlyText->preferredSize(m_style);
            m_background->setRect(0,0,tightSize.width(), tightSize.height());
        }
    }
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


// (Legacy label hit-testing and interaction removed)
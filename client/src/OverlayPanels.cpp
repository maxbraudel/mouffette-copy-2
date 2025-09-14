// Unified overlay implementation
#include "OverlayPanels.h"
#include "RoundedRectItem.h"
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QFont>
#include <QFontMetrics>
#include <cmath>

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
    return QSizeF(r.width() + 2*style.paddingX, r.height() + 2*style.paddingY);
}

void OverlayTextElement::setSize(const QSizeF& size) {
    createGraphicsItems();
    if (m_background) {
        m_background->setRect(0,0,size.width(), size.height());
    }
    if (m_textItem) {
        QRectF tb = m_textItem->boundingRect();
        m_textItem->setPos((size.width()-tb.width())/2.0, (size.height()-tb.height())/2.0);
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
    
    qreal totalWidth = 0;
    qreal totalHeight = 0;
    qreal maxWidth = 0;
    qreal maxHeight = 0;
    
    // Calculate size for elements (new system)
    for (const auto& element : m_elements) {
        if (!element->isVisible()) continue;
        
        QSizeF elementSize = element->preferredSize(m_style);
        maxWidth = std::max(maxWidth, elementSize.width());
        maxHeight = std::max(maxHeight, elementSize.height());
        
        if (m_layout == Horizontal) {
            totalWidth += elementSize.width();
            totalHeight = std::max(totalHeight, elementSize.height());
        } else {
            totalWidth = std::max(totalWidth, elementSize.width());
            totalHeight += elementSize.height();
        }
    }
    
    // Count visible elements for spacing
    int visibleCount = 0;
    for (const auto& element : m_elements) if (element->isVisible()) ++visibleCount;
    
    if (visibleCount > 1) {
        if (m_layout == Horizontal) {
            totalWidth += (visibleCount - 1) * m_style.itemSpacing;
        } else {
            totalHeight += (visibleCount - 1) * m_style.itemSpacing;
        }
    }
    
    // Add padding
    totalWidth += 2 * m_style.paddingX;
    totalHeight += 2 * m_style.paddingY;
    
    // Constrain to maximum width
    if (m_style.maxWidth > 0) {
        totalWidth = std::min(totalWidth, static_cast<qreal>(m_style.maxWidth));
    }
    
    return QSizeF(totalWidth, totalHeight);
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

    // Always keep background item for positioning; make it visually transparent if disabled
    if (m_backgroundVisible) {
        m_background->setBrush(m_style.backgroundBrush());
    } else {
        m_background->setBrush(Qt::NoBrush);
    }
    m_background->setRect(0, 0, m_currentSize.width(), m_currentSize.height());
    m_background->setPos(m_currentPosition);
}

void OverlayPanel::updateLabelsLayout() {
    if (m_elements.isEmpty()) return;
    
    QPointF currentPos(m_style.paddingX, m_style.paddingY);
    
    const bool haveContainer = (m_background != nullptr);

    // Layout elements (new system)
    for (auto& element : m_elements) {
        if (!element->isVisible()) continue;
        
        QSizeF elementSize = element->preferredSize(m_style);
        
        // Adjust size if constrained by panel width
        if (m_layout == Horizontal && m_style.maxWidth > 0) {
            qreal availableWidth = m_currentSize.width() - 2 * m_style.paddingX;
            if (elementSize.width() > availableWidth) {
                elementSize.setWidth(availableWidth);
            }
        }
        
        element->setSize(elementSize);
        if (haveContainer) {
            if (auto *gi = element->graphicsItem()) {
                if (gi->parentItem() != m_background) gi->setParentItem(m_background);
            }
            // Local position inside container
            element->setPosition(currentPos);
        } else {
            // Absolute fallback
            element->setPosition(m_currentPosition + currentPos);
        }
        
        // Move to next position
        if (m_layout == Horizontal) {
            currentPos.setX(currentPos.x() + elementSize.width() + m_style.itemSpacing);
        } else {
            currentPos.setY(currentPos.y() + elementSize.height() + m_style.itemSpacing);
        }
    }
    
    // done
}


QPointF OverlayPanel::calculatePanelPositionFromAnchor(const QPointF& anchorScenePoint, QGraphicsView* view) const {
    if (!view) return QPointF();
    
    // Use EXACT same logic as video controls: work in viewport pixels for gap, then map back
    const QTransform &vt = view->viewportTransform();
    QPointF anchorViewport = vt.map(anchorScenePoint);
    
    QPointF panelTopLeftViewport;
    if (m_position == Top) {
        // Position above anchor: gap pixels up from anchor point
        // Panel top-left = anchor - (width/2, gap + height) in viewport pixels
        panelTopLeftViewport = anchorViewport + QPointF(-m_currentSize.width() / 2.0, -(m_style.gap + m_currentSize.height()));
    } else {
        // Position below anchor: gap pixels down from anchor point  
        // Panel top-left = anchor - (width/2, -gap) in viewport pixels
        panelTopLeftViewport = anchorViewport + QPointF(-m_currentSize.width() / 2.0, m_style.gap);
    }
    
    // Map back to scene coordinates - this ensures pixel-perfect gap regardless of zoom
    return vt.inverted().map(panelTopLeftViewport);
}


// (Legacy label hit-testing and interaction removed)
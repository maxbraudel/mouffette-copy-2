#include "OverlayPanels.h"
#include "RoundedRectItem.h"
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QFont>
#include <QFontMetrics>
#include <QtSvg/QSvgRenderer>
#include <cmath>

// Global constants for overlay z-ordering (moved from MainWindow.cpp)
namespace {
    constexpr qreal Z_SCENE_OVERLAY = 12000.0;
    constexpr qreal Z_OVERLAY_CONTENT = 12001.0;
}

//=============================================================================
// OverlayTextElement Implementation
//=============================================================================

void OverlayTextElement::createGraphicsItems() {
    if (m_background) return; // Already created
    
    // Create background with rounded corners
    m_background = new RoundedRectItem();
    m_background->setPen(Qt::NoPen);
    m_background->setZValue(Z_SCENE_OVERLAY);
    m_background->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_background->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_background->setAcceptedMouseButtons(Qt::NoButton);
    
    // Create text item as child of background
    m_textItem = new QGraphicsTextItem(m_text, m_background);
    m_textItem->setZValue(Z_OVERLAY_CONTENT);
    m_textItem->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_textItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_textItem->setAcceptedMouseButtons(Qt::NoButton);
}

void OverlayTextElement::applyStyle(const OverlayStyle& style) {
    m_currentStyle = style;
    createGraphicsItems();
    
    // Apply background styling
    m_background->setBrush(getStateBrush(style));
    m_background->setRadius(style.cornerRadius);
    
    // Apply text styling
    QFont font = m_textItem->font();
    font.setPixelSize(12); // Standard overlay text size
    m_textItem->setFont(font);
    m_textItem->setDefaultTextColor(style.textColor);
    
    updateAppearance();
}

QSizeF OverlayTextElement::preferredSize(const OverlayStyle& style) const {
    // Calculate text bounds
    QFont font;
    font.setPixelSize(12);
    QFontMetrics fm(font);
    QRect textRect = fm.boundingRect(m_text);
    
    // Add padding
    return QSizeF(textRect.width() + 2 * style.paddingX,
                  textRect.height() + 2 * style.paddingY);
}

void OverlayTextElement::setSize(const QSizeF& size) {
    createGraphicsItems();
    
    // Update background size
    m_background->setRect(0, 0, size.width(), size.height());
    
    // Center text within background
    if (m_textItem) {
        QRectF textBounds = m_textItem->boundingRect();
        qreal x = (size.width() - textBounds.width()) / 2.0;
        qreal y = (size.height() - textBounds.height()) / 2.0;
        m_textItem->setPos(x, y);
    }
}

void OverlayTextElement::setPosition(const QPointF& pos) {
    createGraphicsItems();
    m_background->setPos(pos);
}

bool OverlayTextElement::contains(const QPointF& point) const {
    if (!m_background) return false;
    return m_background->contains(m_background->mapFromScene(point));
}

QRectF OverlayTextElement::boundingRect() const {
    if (!m_background) return QRectF();
    return m_background->boundingRect();
}

void OverlayTextElement::updateVisibility() {
    createGraphicsItems();
    m_background->setVisible(isVisible());
}

void OverlayTextElement::updateAppearance() {
    if (!m_background) return;
    m_background->setBrush(getStateBrush(m_currentStyle));
}

void OverlayTextElement::updateText() {
    if (m_textItem) {
        m_textItem->setPlainText(m_text);
        // Re-center text after text change
        if (m_background) {
            QRectF bgRect = m_background->rect();
            QRectF textBounds = m_textItem->boundingRect();
            qreal x = (bgRect.width() - textBounds.width()) / 2.0;
            qreal y = (bgRect.height() - textBounds.height()) / 2.0;
            m_textItem->setPos(x, y);
        }
    }
}

//=============================================================================
// OverlayTextLabel Implementation (Legacy)
//=============================================================================

OverlayTextLabel::OverlayTextLabel(const QString& text, const QString& id)
    : OverlayLabel(Text, id), m_text(text)
{
    // Create background and text items
    m_background = new RoundedRectItem();
    m_background->setPen(Qt::NoPen);
    m_background->setZValue(Z_SCENE_OVERLAY);
    m_background->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_background->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_background->setAcceptedMouseButtons(Qt::NoButton);
    
    m_textItem = new QGraphicsTextItem(m_text, m_background);
    m_textItem->setZValue(Z_OVERLAY_CONTENT);
    m_textItem->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_textItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_textItem->setAcceptedMouseButtons(Qt::NoButton);
}

OverlayTextLabel::~OverlayTextLabel() {
    delete m_background; // Will also delete m_textItem as child
}

void OverlayTextLabel::setText(const QString& text) {
    if (m_text != text) {
        m_text = text;
        if (m_textItem) {
            m_textItem->setPlainText(m_text);
        }
        updateLayout();
    }
}

QSizeF OverlayTextLabel::preferredSize(const OverlayStyle& style) const {
    if (m_text.isEmpty()) return QSizeF(0, 0);
    
    QFontMetrics fm(m_textItem ? m_textItem->font() : QFont());
    QRectF textRect = fm.boundingRect(m_text);
    
    return QSizeF(
        textRect.width() + 2 * style.paddingX,
        textRect.height() + 2 * style.paddingY
    );
}

void OverlayTextLabel::setSize(const QSizeF& size) {
    m_currentSize = size;
    updateLayout();
}

QGraphicsItem* OverlayTextLabel::graphicsItem() {
    return m_background;
}

void OverlayTextLabel::applyStyle(const OverlayStyle& style) {
    if (m_background) {
        m_background->setBrush(style.backgroundBrush());
        m_background->setRadius(style.cornerRadius);
    }
    
    if (m_textItem) {
        m_textItem->setDefaultTextColor(style.textColor);
    }
    
    updateLayout();
}

void OverlayTextLabel::setPosition(const QPointF& pos) {
    if (m_background) {
        m_background->setPos(pos);
    }
}

bool OverlayTextLabel::contains(const QPointF& point) const {
    if (!m_background || !isVisible()) return false;
    return m_background->contains(m_background->mapFromScene(point));
}

void OverlayTextLabel::updateLayout() {
    if (!m_background || !m_textItem) return;
    
    // Update background size
    m_background->setRect(0, 0, m_currentSize.width(), m_currentSize.height());
    
    // Center text within background
    QRectF textRect = m_textItem->boundingRect();
    qreal textX = (m_currentSize.width() - textRect.width()) / 2.0;
    qreal textY = (m_currentSize.height() - textRect.height()) / 2.0;
    m_textItem->setPos(textX, textY);
}

//=============================================================================
// OverlayButton Implementation
//=============================================================================

OverlayButton::OverlayButton(const QString& iconPath, const QString& id)
    : OverlayLabel(Button, id), m_iconPath(iconPath)
{
    // Create background
    m_background = new RoundedRectItem();
    m_background->setPen(Qt::NoPen);
    m_background->setZValue(Z_SCENE_OVERLAY);
    m_background->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_background->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_background->setAcceptedMouseButtons(Qt::NoButton);
    
    // Create icon if path provided
    if (!m_iconPath.isEmpty()) {
        m_icon = new QGraphicsSvgItem(m_iconPath, m_background);
        m_icon->setZValue(Z_OVERLAY_CONTENT);
        m_icon->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
        m_icon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        m_icon->setAcceptedMouseButtons(Qt::NoButton);
    }
}

OverlayButton::~OverlayButton() {
    delete m_background; // Will also delete m_icon as child
}

void OverlayButton::setIconPath(const QString& path) {
    if (m_iconPath != path) {
        m_iconPath = path;
        updateIcon();
    }
}

void OverlayButton::setActive(bool active) {
    if (m_active != active) {
        m_active = active;
        updateState();
    }
}

QSizeF OverlayButton::preferredSize(const OverlayStyle& style) const {
    // Buttons are typically square, using the default height
    int size = (style.defaultHeight > 0) ? style.defaultHeight : 32;
    return QSizeF(size, size);
}

void OverlayButton::setSize(const QSizeF& size) {
    m_currentSize = size;
    updateLayout();
}

QGraphicsItem* OverlayButton::graphicsItem() {
    return m_background;
}

void OverlayButton::applyStyle(const OverlayStyle& style) {
    m_currentStyle = style;
    if (m_background) {
        m_background->setBrush(m_active ? style.tintedBackgroundBrush() : style.backgroundBrush());
        m_background->setRadius(style.cornerRadius);
    }
    updateLayout();
}

void OverlayButton::setPosition(const QPointF& pos) {
    if (m_background) {
        m_background->setPos(pos);
    }
}

bool OverlayButton::contains(const QPointF& point) const {
    if (!m_background || !isVisible() || !isEnabled()) return false;
    return m_background->contains(m_background->mapFromScene(point));
}

void OverlayButton::handleClick(const QPointF& point) {
    if (contains(point) && onClicked) {
        onClicked();
    }
}

void OverlayButton::updateState() {
    applyStyle(m_currentStyle); // Re-apply style to update active state
}

void OverlayButton::updateIcon() {
    if (m_icon) {
        delete m_icon;
        m_icon = nullptr;
    }
    
    if (!m_iconPath.isEmpty() && m_background) {
        m_icon = new QGraphicsSvgItem(m_iconPath, m_background);
        m_icon->setZValue(Z_OVERLAY_CONTENT);
        m_icon->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
        m_icon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        m_icon->setAcceptedMouseButtons(Qt::NoButton);
        updateLayout();
    }
}

void OverlayButton::updateLayout() {
    if (!m_background) return;
    
    // Update background size
    m_background->setRect(0, 0, m_currentSize.width(), m_currentSize.height());
    
    // Center icon within background
    if (m_icon) {
        QSizeF iconNaturalSize = m_icon->renderer() ? 
            m_icon->renderer()->defaultSize() : QSizeF(24, 24);
        if (iconNaturalSize.width() <= 0 || iconNaturalSize.height() <= 0) {
            iconNaturalSize = QSizeF(24, 24);
        }
        
        // Scale icon to 60% of button size
        qreal scale = std::min(
            m_currentSize.width() / iconNaturalSize.width(),
            m_currentSize.height() / iconNaturalSize.height()
        ) * 0.6;
        
        m_icon->setScale(scale);
        
        // Center the scaled icon
        qreal iconX = (m_currentSize.width() - iconNaturalSize.width() * scale) / 2.0;
        qreal iconY = (m_currentSize.height() - iconNaturalSize.height() * scale) / 2.0;
        m_icon->setPos(iconX, iconY);
    }
}

//=============================================================================
// OverlaySlider Implementation
//=============================================================================

OverlaySlider::OverlaySlider(const QString& id)
    : OverlayLabel(Slider, id)
{
    // Create background
    m_background = new QGraphicsRectItem();
    m_background->setPen(Qt::NoPen);
    m_background->setZValue(Z_SCENE_OVERLAY);
    m_background->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_background->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_background->setAcceptedMouseButtons(Qt::NoButton);
    
    // Create fill
    m_fill = new QGraphicsRectItem(m_background);
    m_fill->setPen(Qt::NoPen);
    m_fill->setBrush(QColor(74, 144, 226)); // Primary accent color
    m_fill->setZValue(Z_OVERLAY_CONTENT);
    m_fill->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_fill->setAcceptedMouseButtons(Qt::NoButton);
}

OverlaySlider::~OverlaySlider() {
    delete m_background; // Will also delete m_fill as child
}

void OverlaySlider::setValue(qreal value) {
    qreal clampedValue = std::clamp(value, m_minimum, m_maximum);
    if (m_value != clampedValue) {
        m_value = clampedValue;
        updateFill();
        if (onValueChanged) {
            onValueChanged(m_value);
        }
    }
}

void OverlaySlider::setRange(qreal min, qreal max) {
    m_minimum = min;
    m_maximum = std::max(min, max);
    setValue(m_value); // Re-clamp current value
}

QSizeF OverlaySlider::preferredSize(const OverlayStyle& style) const {
    // Sliders typically have a minimum width and use the default height
    int height = (style.defaultHeight > 0) ? style.defaultHeight : 24;
    return QSizeF(80, height); // Minimum reasonable width
}

void OverlaySlider::setSize(const QSizeF& size) {
    m_currentSize = size;
    updateFill();
}

void OverlaySlider::applyStyle(const OverlayStyle& style) {
    if (m_background) {
        m_background->setBrush(style.backgroundBrush());
    }
    updateFill();
}

void OverlaySlider::setPosition(const QPointF& pos) {
    if (m_background) {
        m_background->setPos(pos);
    }
}

bool OverlaySlider::contains(const QPointF& point) const {
    if (!m_background || !isVisible() || !isEnabled()) return false;
    return m_background->contains(m_background->mapFromScene(point));
}

void OverlaySlider::handleClick(const QPointF& point) {
    if (!contains(point)) return;
    
    qreal newValue = pointToValue(m_background->mapFromScene(point));
    setValue(newValue);
    m_dragging = true;
}

void OverlaySlider::handleDrag(const QPointF& point, const QPointF& delta) {
    Q_UNUSED(delta);
    if (!m_dragging || !contains(point)) return;
    
    qreal newValue = pointToValue(m_background->mapFromScene(point));
    setValue(newValue);
}

void OverlaySlider::updateFill() {
    if (!m_background || !m_fill) return;
    
    // Update background size
    m_background->setRect(0, 0, m_currentSize.width(), m_currentSize.height());
    
    // Update fill size based on current value
    const qreal margin = 2.0;
    qreal fillRatio = (m_maximum > m_minimum) ? 
        (m_value - m_minimum) / (m_maximum - m_minimum) : 0.0;
    
    qreal fillWidth = std::max(0.0, m_currentSize.width() - 2 * margin) * fillRatio;
    qreal fillHeight = std::max(0.0, m_currentSize.height() - 2 * margin);
    
    m_fill->setRect(margin, margin, fillWidth, fillHeight);
}

qreal OverlaySlider::pointToValue(const QPointF& point) const {
    if (m_currentSize.width() <= 0 || m_maximum <= m_minimum) return m_value;
    
    qreal ratio = std::clamp(point.x() / m_currentSize.width(), 0.0, 1.0);
    return m_minimum + ratio * (m_maximum - m_minimum);
}

//=============================================================================
// OverlayPanel Implementation
//=============================================================================

OverlayPanel::OverlayPanel(Position position, Layout layout)
    : m_position(position), m_layout(layout)
{
    // createBackground(); // Disabled - overlay elements handle their own backgrounds
}

OverlayPanel::~OverlayPanel() {
    clear();
    delete m_background;
}

void OverlayPanel::setLayout(Layout layout) {
    if (m_layout != layout) {
        m_layout = layout;
        updateLabelsLayout();
    }
}

void OverlayPanel::setStyle(const OverlayStyle& style) {
    m_style = style;
    updateBackground();
    
    // Apply style to all labels
    for (auto& label : m_labels) {
        label->applyStyle(style);
    }
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

//=============================================================================
// OverlayPanel Label Management (Legacy System)
//=============================================================================

void OverlayPanel::addLabel(std::shared_ptr<OverlayLabel> label) {
    if (!label) return;
    
    m_labels.append(label);

    // Ensure background so positioning remains in local space
    if (!m_background) {
        createBackground();
        if (m_scene && !m_background->scene() && !m_parentItem) {
            m_scene->addItem(m_background);
        }
    }

    // Parent label graphics item appropriately
    if (auto* item = label->graphicsItem()) {
        if (m_background) {
            item->setParentItem(m_background);
        } else if (m_parentItem) {
            item->setParentItem(m_parentItem);
        } else if (m_scene) {
            m_scene->addItem(item);
        }
    }

    label->applyStyle(m_style);
    updateLabelsLayout();
}

void OverlayPanel::removeLabel(const QString& id) {
    auto it = std::find_if(m_labels.begin(), m_labels.end(),
        [&id](const std::shared_ptr<OverlayLabel>& label) {
            return label->id() == id;
        });
    
    if (it != m_labels.end()) {
        m_labels.erase(it);
        updateLabelsLayout();
    }
}

void OverlayPanel::removeLabel(std::shared_ptr<OverlayLabel> label) {
    auto it = std::find(m_labels.begin(), m_labels.end(), label);
    if (it != m_labels.end()) {
        m_labels.erase(it);
        updateLabelsLayout();
    }
}

void OverlayPanel::clear() {
    m_labels.clear();
    updateLabelsLayout();
}

std::shared_ptr<OverlayLabel> OverlayPanel::findLabel(const QString& id) const {
    auto it = std::find_if(m_labels.begin(), m_labels.end(),
        [&id](const std::shared_ptr<OverlayLabel>& label) {
            return label->id() == id;
        });
    
    return (it != m_labels.end()) ? *it : nullptr;
}

void OverlayPanel::setVisible(bool visible) {
    if (m_visible != visible) {
        m_visible = visible;
        if (m_background) {
            m_background->setVisible(visible);
        }
        for (auto& label : m_labels) {
            label->setVisible(visible);
        }
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
    
    // Add any existing elements to the scene
    if (scene) {
        for (auto& element : m_elements) {
            QGraphicsItem* graphicsItem = element->graphicsItem();
            if (graphicsItem && !graphicsItem->scene() && !graphicsItem->parentItem()) {
                scene->addItem(graphicsItem);
            }
        }
        for (auto& label : m_labels) {
            QGraphicsItem* graphicsItem = label->graphicsItem();
            if (graphicsItem && !graphicsItem->scene() && !graphicsItem->parentItem()) {
                scene->addItem(graphicsItem);
            }
        }
    }
}

void OverlayPanel::updateLayoutWithAnchor(const QPointF& anchorScenePoint, QGraphicsView* view) {
    if (!view || (m_labels.isEmpty() && m_elements.isEmpty())) return;
    m_currentSize = calculateSize();
    m_currentPosition = calculatePanelPositionFromAnchor(anchorScenePoint, view);
    
    // Ensure background exists and is positioned like video controls background
    if (!m_background) {
        createBackground();
    }

    // Decide visual background presence: if only a single text element on a top panel, hide visual fill
    if (m_position == Top && m_labels.isEmpty()) {
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
    if (m_labels.isEmpty() && m_elements.isEmpty()) return QSizeF(0, 0);
    
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
    
    // Calculate size for labels (legacy system)
    for (const auto& label : m_labels) {
        if (!label->isVisible()) continue;
        
        QSizeF labelSize = label->preferredSize(m_style);
        maxWidth = std::max(maxWidth, labelSize.width());
        maxHeight = std::max(maxHeight, labelSize.height());
        
        if (m_layout == Horizontal) {
            totalWidth += labelSize.width();
            totalHeight = std::max(totalHeight, labelSize.height());
        } else {
            totalWidth = std::max(totalWidth, labelSize.width());
            totalHeight += labelSize.height();
        }
    }
    
    // Add spacing between items
    int visibleCount = 0;
    for (const auto& element : m_elements) {
        if (element->isVisible()) visibleCount++;
    }
    for (const auto& label : m_labels) {
        if (label->isVisible()) visibleCount++;
    }
    
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
    m_background = new QGraphicsRectItem();
    m_background->setPen(Qt::NoPen);
    m_background->setZValue(m_style.zOverlay);
    m_background->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_background->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_background->setAcceptedMouseButtons(Qt::NoButton);
    updateBackground();
    // Re-parent existing elements and labels so layout can be done in local coords
    for (auto &element : m_elements) {
        if (auto *gi = element->graphicsItem()) {
            if (!gi->parentItem()) gi->setParentItem(m_background);
        }
    }
    for (auto &label : m_labels) {
        if (auto *gi = label->graphicsItem()) {
            if (!gi->parentItem()) gi->setParentItem(m_background);
        }
    }
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
    if (m_elements.isEmpty() && m_labels.isEmpty()) return;
    
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
    
    // Layout labels (legacy system)
    for (auto& label : m_labels) {
        if (!label->isVisible()) continue;
        
        QSizeF labelSize = label->preferredSize(m_style);
        
        // Adjust size if constrained by panel width
        if (m_layout == Horizontal && m_style.maxWidth > 0) {
            qreal availableWidth = m_currentSize.width() - 2 * m_style.paddingX;
            if (labelSize.width() > availableWidth) {
                labelSize.setWidth(availableWidth);
            }
        }
        
        label->setSize(labelSize);
        if (haveContainer) {
            if (auto *gi = label->graphicsItem()) {
                if (gi->parentItem() != m_background) gi->setParentItem(m_background);
            }
            label->setPosition(currentPos);
        } else {
            label->setPosition(m_currentPosition + currentPos);
        }
        
        // Move to next position
        if (m_layout == Horizontal) {
            currentPos.setX(currentPos.x() + labelSize.width() + m_style.itemSpacing);
        } else {
            currentPos.setY(currentPos.y() + labelSize.height() + m_style.itemSpacing);
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


std::shared_ptr<OverlayLabel> OverlayPanel::labelAt(const QPointF& scenePos) const {
    for (const auto& label : m_labels) {
        if (label->contains(scenePos)) {
            return label;
        }
    }
    return nullptr;
}

void OverlayPanel::handleClick(const QPointF& scenePos) {
    if (auto label = labelAt(scenePos)) {
        label->handleClick(scenePos);
    }
}

void OverlayPanel::handleDrag(const QPointF& scenePos, const QPointF& delta) {
    if (auto label = labelAt(scenePos)) {
        label->handleDrag(scenePos, delta);
    }
}
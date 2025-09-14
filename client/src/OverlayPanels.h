#ifndef OVERLAYPANELS_H
#define OVERLAYPANELS_H

#include <QGraphicsItem>
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QGraphicsSvgItem>
#include <QGraphicsView>
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QString>
#include <QList>
#include <QPen>
#include <QCursor>
#include <memory>

#include "RoundedRectItem.h"

/**
 * Unified styling for all media overlay elements
 */
struct OverlayStyle {
    // Background colors
    QColor backgroundColor = QColor(0, 0, 0, 160);           // Main translucent dark background
    QColor activeBackgroundColor = QColor(74, 144, 226, 180); // Blue tint for active states
    
    // Text colors
    QColor textColor = Qt::white;
    QColor activeTextColor = Qt::white;
    
    // Layout dimensions (pixels)
    int cornerRadius = 6;
    int paddingX = 8;
    int paddingY = 4;
    int gap = 8;               // Gap between panel and media edge
    int itemSpacing = 8;       // Gap between items within a panel
    
    // Panel sizing
    int defaultHeight = -1;    // -1 = auto-size based on content
    int maxWidth = 300;
    
    // Z-ordering
    qreal zOverlay = 12000.0;
    qreal zOverlayContent = 12001.0;
    
    // Helper methods
    QBrush backgroundBrush() const { return QBrush(backgroundColor); }
    QBrush activeBackgroundBrush() const { return QBrush(activeBackgroundColor); }
    
    /**
     * Create tinted version of background for active states
     * @param tintStrength 0.0 = original color, 1.0 = full accent color
     */
    QBrush tintedBackgroundBrush(qreal tintStrength = 0.33) const {
        const QColor accent(74, 144, 226, 255);
        auto blend = [](const QColor& a, const QColor& b, qreal t) {
            auto clamp = [](int v) { return std::max(0, std::min(255, v)); };
            int r = clamp(static_cast<int>(a.red()   * (1.0 - t) + b.red()   * t));
            int g = clamp(static_cast<int>(a.green() * (1.0 - t) + b.green() * t));
            int bl = clamp(static_cast<int>(a.blue()  * (1.0 - t) + b.blue()  * t));
            return QColor(r, g, bl, a.alpha());
        };
        return QBrush(blend(backgroundColor, accent, tintStrength));
    }
};

/**
 * Base class for standardized overlay UI elements with consistent appearance
 * All overlay elements have a grey background with border radius and can contain text/icons
 */
class OverlayElement {
public:
    enum ElementType {
        Label,       // Simple text display
        Button,      // Clickable with potential state changes
        ToggleButton, // Button that switches between states (play/pause, loop on/off)
        Slider       // Interactive progress/volume control
    };
    
    enum ElementState {
        Normal,      // Default appearance
        Hovered,     // Mouse over (if interactive)
        Active,      // Pressed or active state
        Disabled,    // Grayed out, non-interactive
        Toggled      // For toggle buttons in "on" state
    };
    
    OverlayElement(ElementType type, const QString& id = QString())
        : m_type(type), m_id(id), m_state(Normal), m_visible(true), m_interactive(type != Label) {}
    
    virtual ~OverlayElement() = default;
    
    // Core properties
    ElementType type() const { return m_type; }
    QString id() const { return m_id; }
    void setId(const QString& id) { m_id = id; }
    
    bool isVisible() const { return m_visible; }
    virtual void setVisible(bool visible) { m_visible = visible; updateVisibility(); }
    
    ElementState state() const { return m_state; }
    virtual void setState(ElementState state) { m_state = state; updateAppearance(); }
    
    bool isInteractive() const { return m_interactive; }
    void setInteractive(bool interactive) { m_interactive = interactive; }
    
    // Styling
    virtual void applyStyle(const OverlayStyle& style) = 0;
    virtual QSizeF preferredSize(const OverlayStyle& style) const = 0;
    virtual void setSize(const QSizeF& size) = 0;
    virtual void setPosition(const QPointF& pos) = 0;
    
    // Graphics item management
    virtual QGraphicsItem* graphicsItem() = 0;
    virtual const QGraphicsItem* graphicsItem() const = 0;
    
    // Event handling for interactive elements
    virtual void handleClick() {}
    virtual void handleHover(bool entered) { 
        if (m_interactive) {
            setState(entered ? Hovered : Normal);
        }
    }
    
    // Geometry
    virtual bool contains(const QPointF& point) const = 0;
    virtual QRectF boundingRect() const = 0;
    
protected:
    virtual void updateVisibility() = 0;
    virtual void updateAppearance() = 0;
    
    // Get brush for current state
    QBrush getStateBrush(const OverlayStyle& style) const {
        switch (m_state) {
            case Active:
            case Toggled:
                return style.tintedBackgroundBrush(0.5);
            case Hovered:
                return style.tintedBackgroundBrush(0.2);
            case Disabled:
                return QBrush(QColor(style.backgroundColor.red(), 
                                   style.backgroundColor.green(), 
                                   style.backgroundColor.blue(), 
                                   style.backgroundColor.alpha() / 2));
            case Normal:
            default:
                return style.backgroundBrush();
        }
    }
    
private:
    ElementType m_type;
    QString m_id;
    ElementState m_state;
    bool m_visible;
    bool m_interactive;
};

/**
 * Text-only overlay element - displays filename, status text, etc.
 */
class OverlayTextElement : public OverlayElement {
public:
    OverlayTextElement(const QString& text, const QString& id = QString())
        : OverlayElement(Label, id), m_text(text), m_background(nullptr), m_textItem(nullptr) {}
    
    ~OverlayTextElement() override {
        delete m_background; // Will also delete m_textItem as child
    }
    
    // Text management
    QString text() const { return m_text; }
    void setText(const QString& text) { m_text = text; updateText(); }
    
    // OverlayElement interface
    void applyStyle(const OverlayStyle& style) override;
    QSizeF preferredSize(const OverlayStyle& style) const override;
    void setSize(const QSizeF& size) override;
    void setPosition(const QPointF& pos) override;
    
    QGraphicsItem* graphicsItem() override { return m_background; }
    const QGraphicsItem* graphicsItem() const override { return m_background; }
    
    bool contains(const QPointF& point) const override;
    QRectF boundingRect() const override;
    
protected:
    void updateVisibility() override;
    void updateAppearance() override;
    void updateText();
    
    void createGraphicsItems();
    
private:
    QString m_text;
    RoundedRectItem* m_background;
    QGraphicsTextItem* m_textItem;
    OverlayStyle m_currentStyle;
};

/**
 * Base class for individual overlay elements (labels, buttons, sliders)
 * @deprecated Use OverlayElement instead
 */
class OverlayLabel {
public:
    enum Type {
        Text,        // Static text label
        Button,      // Clickable button with icon
        Slider,      // Progress/volume slider
        Custom       // Custom graphics item
    };
    
    OverlayLabel(Type type, const QString& id = QString())
        : m_type(type), m_id(id), m_visible(true), m_enabled(true) {}
    
    virtual ~OverlayLabel() = default;
    
    // Core properties
    Type type() const { return m_type; }
    QString id() const { return m_id; }
    void setId(const QString& id) { m_id = id; }
    
    bool isVisible() const { return m_visible; }
    void setVisible(bool visible) { m_visible = visible; updateVisibility(); }
    
    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool enabled) { m_enabled = enabled; updateState(); }
    
    // Size management
    virtual QSizeF preferredSize(const OverlayStyle& style) const = 0;
    virtual void setSize(const QSizeF& size) = 0;
    
    // Graphics item management
    virtual QGraphicsItem* graphicsItem() = 0;
    virtual void applyStyle(const OverlayStyle& style) = 0;
    virtual void setPosition(const QPointF& pos) = 0;
    
    // Hit testing
    virtual bool contains(const QPointF& point) const = 0;
    virtual void handleClick(const QPointF& point) {}
    virtual void handleDrag(const QPointF& point, const QPointF& delta) {}
    
protected:
    virtual void updateVisibility() {
        if (auto* item = graphicsItem()) {
            item->setVisible(m_visible);
        }
    }
    
    virtual void updateState() {
        // Base implementation does nothing; subclasses can override
    }
    
private:
    Type m_type;
    QString m_id;
    bool m_visible;
    bool m_enabled;
};

/**
 * Text label overlay element
 */
class OverlayTextLabel : public OverlayLabel {
public:
    explicit OverlayTextLabel(const QString& text = QString(), const QString& id = QString());
    ~OverlayTextLabel() override;
    
    // Text content
    QString text() const { return m_text; }
    void setText(const QString& text);
    
    // OverlayLabel interface
    QSizeF preferredSize(const OverlayStyle& style) const override;
    void setSize(const QSizeF& size) override;
    QGraphicsItem* graphicsItem() override;
    void applyStyle(const OverlayStyle& style) override;
    void setPosition(const QPointF& pos) override;
    bool contains(const QPointF& point) const override;
    
private:
    void updateLayout();
    
    QString m_text;
    RoundedRectItem* m_background = nullptr;
    QGraphicsTextItem* m_textItem = nullptr;
    QSizeF m_currentSize;
};

/**
 * Button overlay element with icon support
 */
class OverlayButton : public OverlayLabel {
public:
    explicit OverlayButton(const QString& iconPath = QString(), const QString& id = QString());
    ~OverlayButton() override;
    
    // Icon management
    QString iconPath() const { return m_iconPath; }
    void setIconPath(const QString& path);
    
    // State
    bool isActive() const { return m_active; }
    void setActive(bool active);
    
    // OverlayLabel interface
    QSizeF preferredSize(const OverlayStyle& style) const override;
    void setSize(const QSizeF& size) override;
    QGraphicsItem* graphicsItem() override;
    void applyStyle(const OverlayStyle& style) override;
    void setPosition(const QPointF& pos) override;
    bool contains(const QPointF& point) const override;
    void handleClick(const QPointF& point) override;
    
    // Callback for click events
    std::function<void()> onClicked;
    
protected:
    void updateState() override;
    
private:
    void updateIcon();
    void updateLayout();
    
    QString m_iconPath;
    bool m_active = false;
    RoundedRectItem* m_background = nullptr;
    QGraphicsSvgItem* m_icon = nullptr;
    QSizeF m_currentSize;
    OverlayStyle m_currentStyle;
};

/**
 * Slider overlay element for progress/volume controls
 */
class OverlaySlider : public OverlayLabel {
public:
    explicit OverlaySlider(const QString& id = QString());
    ~OverlaySlider() override;
    
    // Value management
    qreal value() const { return m_value; }
    void setValue(qreal value); // 0.0 to 1.0
    
    qreal minimum() const { return m_minimum; }
    qreal maximum() const { return m_maximum; }
    void setRange(qreal min, qreal max);
    
    // OverlayLabel interface
    QSizeF preferredSize(const OverlayStyle& style) const override;
    void setSize(const QSizeF& size) override;
    QGraphicsItem* graphicsItem() override { return m_background; }
    void applyStyle(const OverlayStyle& style) override;
    void setPosition(const QPointF& pos) override;
    bool contains(const QPointF& point) const override;
    void handleClick(const QPointF& point) override;
    void handleDrag(const QPointF& point, const QPointF& delta) override;
    
    // Callbacks
    std::function<void(qreal)> onValueChanged;
    
private:
    void updateFill();
    qreal pointToValue(const QPointF& point) const;
    
    qreal m_value = 0.0;
    qreal m_minimum = 0.0;
    qreal m_maximum = 1.0;
    QGraphicsRectItem* m_background = nullptr;
    QGraphicsRectItem* m_fill = nullptr;
    QSizeF m_currentSize;
    bool m_dragging = false;
};

/**
 * Panel that manages a collection of overlay labels
 */
class OverlayPanel {
public:
    enum Position {
        Top,    // Above media item  
        Bottom  // Below media item
    };
    
    enum Layout {
        Horizontal,  // Items arranged left to right
        Vertical     // Items arranged top to bottom
    };
    
    explicit OverlayPanel(Position position, Layout layout = Horizontal);
    ~OverlayPanel();
    
    // Panel configuration
    Position position() const { return m_position; }
    Layout layout() const { return m_layout; }
    void setLayout(Layout layout);
    
    // Style management
    const OverlayStyle& style() const { return m_style; }
    void setStyle(const OverlayStyle& style);
    
    // Element management (new system)
    void addElement(std::shared_ptr<OverlayElement> element);
    void removeElement(const QString& id);
    void removeElement(std::shared_ptr<OverlayElement> element);
    void clearElements();
    
    std::shared_ptr<OverlayElement> findElement(const QString& id) const;
    const QList<std::shared_ptr<OverlayElement>>& elements() const { return m_elements; }
    
    // Label management (legacy system)
    void addLabel(std::shared_ptr<OverlayLabel> label);
    void removeLabel(const QString& id);
    void removeLabel(std::shared_ptr<OverlayLabel> label);
    void clear();
    
    std::shared_ptr<OverlayLabel> findLabel(const QString& id) const;
    const QList<std::shared_ptr<OverlayLabel>>& labels() const { return m_labels; }
    
    // Visibility
    bool isVisible() const { return m_visible; }
    void setVisible(bool visible);
    
    // Panel management
    void setParentItem(QGraphicsItem* parent);
    void setScene(QGraphicsScene* scene);
    QGraphicsScene* scene() const { return m_scene; }
    
    // Layout and positioning (unified): anchorScenePoint is top-center (Top) or bottom-center (Bottom)
    void updateLayoutWithAnchor(const QPointF& anchorScenePoint, QGraphicsView* view);
    QSizeF calculateSize() const;
    
    // Hit testing
    std::shared_ptr<OverlayLabel> labelAt(const QPointF& scenePos) const;
    void handleClick(const QPointF& scenePos);
    void handleDrag(const QPointF& scenePos, const QPointF& delta);
    
private:
    void createBackground();
    void updateBackground();
    void updateLabelsLayout();
    QPointF calculatePanelPositionFromAnchor(const QPointF& anchorScenePoint, QGraphicsView* view) const; // single source of truth
    
    Position m_position;
    Layout m_layout;
    OverlayStyle m_style;
    bool m_visible = true;
    
    QList<std::shared_ptr<OverlayElement>> m_elements;  // New element system
    QList<std::shared_ptr<OverlayLabel>> m_labels;      // Legacy label system
    QGraphicsRectItem* m_background = nullptr;
    QGraphicsItem* m_parentItem = nullptr;
    QGraphicsScene* m_scene = nullptr;
    
    QPointF m_currentPosition;
    QSizeF m_currentSize;
};

#endif // OVERLAYPANELS_H
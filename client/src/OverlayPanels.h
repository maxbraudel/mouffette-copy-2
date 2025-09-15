// ============================================================================
// Unified Overlay System (clean header)
// ============================================================================
#ifndef OVERLAYPANELS_H
#define OVERLAYPANELS_H

#include <QGraphicsItem>
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGraphicsSceneMouseEvent>
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QString>
#include <QList>
#include <memory>

#include "RoundedRectItem.h"

// Forward declarations
class QGraphicsSceneMouseEvent;

// Custom RoundedRectItem that blocks mouse events from passing through
class MouseBlockingRoundedRectItem : public RoundedRectItem {
public:
    MouseBlockingRoundedRectItem(QGraphicsItem* parent = nullptr) 
        : RoundedRectItem(parent) {
        setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    }
    void setClickCallback(std::function<void()> cb) { m_clickCallback = std::move(cb); }

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        // Accept the event to prevent it from propagating to items behind
        event->accept();
        if (m_clickCallback) m_clickCallback();
    }
    
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        // Accept the event to prevent it from propagating to items behind
        event->accept();
    }
private:
    std::function<void()> m_clickCallback;
};

// Custom QGraphicsTextItem that blocks mouse events from passing through
class MouseBlockingTextItem : public QGraphicsTextItem {
public:
    MouseBlockingTextItem(const QString& text, QGraphicsItem* parent = nullptr) 
        : QGraphicsTextItem(text, parent) {
        setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    }

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        // Accept the event to prevent it from propagating to items behind
        event->accept();
    }
    
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        // Accept the event to prevent it from propagating to items behind
        event->accept();
    }
};

// Custom QGraphicsRectItem that blocks mouse events from passing through
class MouseBlockingRectItem : public QGraphicsRectItem {
public:
    MouseBlockingRectItem(QGraphicsItem* parent = nullptr) 
        : QGraphicsRectItem(parent) {
        setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    }

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        // Accept the event to prevent it from propagating to items behind
        event->accept();
    }
    
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        // Accept the event to prevent it from propagating to items behind
        event->accept();
    }
};

// Style shared by all overlay elements
struct OverlayStyle {
    QColor backgroundColor = QColor(0,0,0,160);
    QColor activeBackgroundColor = QColor(74,144,226,180);
    QColor textColor = Qt::white;
    QColor activeTextColor = Qt::white;
    int cornerRadius = 8;
    int paddingX = 12;
    int paddingY = 8;
    int gap = 8;          // Distance panel <-> media edge (pixels, in viewport space)
    int itemSpacing = 8;  // Space between elements
    int defaultHeight = -1; // >0 forces uniform element height (text vertically centered)
    int maxWidth = 300;
    qreal zOverlay = 12000.0;
    qreal zOverlayContent = 12001.0;
    QBrush backgroundBrush() const { return QBrush(backgroundColor); }
    QBrush tintedBackgroundBrush(qreal t = 0.33) const {
        const QColor accent(74,144,226,255);
        auto blend=[&](const QColor&a,const QColor&b){return QColor(
            a.red()*(1-t)+b.red()*t,
            a.green()*(1-t)+b.green()*t,
            a.blue()*(1-t)+b.blue()*t,
            a.alpha());};
        return QBrush(blend(backgroundColor, accent));
    }
};

class OverlayElement {
public:
    enum ElementType { Label, Button, ToggleButton, Slider, RowBreak };
    enum ElementState { Normal, Hovered, Active, Disabled, Toggled };
    OverlayElement(ElementType type, const QString& id = QString()) : m_type(type), m_id(id) {}
    virtual ~OverlayElement() = default;
    QString id() const { return m_id; }
    void setId(const QString& id) { m_id = id; }
    ElementType type() const { return m_type; }
    virtual void applyStyle(const OverlayStyle& style) = 0;
    virtual QSizeF preferredSize(const OverlayStyle& style) const = 0;
    virtual void setSize(const QSizeF& size) = 0;
    virtual void setPosition(const QPointF& pos) = 0;
    virtual QGraphicsItem* graphicsItem() = 0;
    virtual bool isVisible() const = 0;
    virtual void setVisible(bool v) = 0;
    // State management (default no-op: subclasses override to update appearance)
    virtual void setState(ElementState s) { m_state = s; }
    ElementState state() const { return m_state; }
protected:
    ElementType m_type;
    QString m_id;
    ElementState m_state = Normal;
};

class OverlayTextElement : public OverlayElement {
public:
    OverlayTextElement(const QString& text, const QString& id = QString());
    ~OverlayTextElement() override;
    QString text() const { return m_text; }
    void setText(const QString& text);
    // OverlayElement implementation
    void applyStyle(const OverlayStyle& style) override;
    QSizeF preferredSize(const OverlayStyle& style) const override;
    void setSize(const QSizeF& size) override;
    void setPosition(const QPointF& pos) override;
    QGraphicsItem* graphicsItem() override { return m_background; }
    bool isVisible() const override { return m_visible; }
    void setVisible(bool v) override;
private:
    void createGraphicsItems();
    void updateAppearance();
    void updateText();
    QString m_text;
    bool m_visible = true;
    MouseBlockingRoundedRectItem* m_background = nullptr;
    MouseBlockingTextItem* m_textItem = nullptr;
    OverlayStyle m_currentStyle; // last applied style snapshot
};

// Basic square button element (stub). Will gain richer state visuals in a later step.
class OverlayButtonElement : public OverlayElement {
public:
    // If label empty, button renders blank square (icon-ready placeholder)
    OverlayButtonElement(const QString& label = QString(), const QString& id = QString());
    ~OverlayButtonElement() override;

    QString label() const { return m_label; }
    void setLabel(const QString& l);

    // OverlayElement implementation
    void applyStyle(const OverlayStyle& style) override;
    QSizeF preferredSize(const OverlayStyle& style) const override; // square based on defaultHeight (or text if larger)
    void setSize(const QSizeF& size) override;
    void setPosition(const QPointF& pos) override;
    QGraphicsItem* graphicsItem() override { return m_background; }
    bool isVisible() const override { return m_visible; }
    void setVisible(bool v) override;
    void setState(ElementState s) override; // updates brush
    void setOnClicked(std::function<void()> cb) { m_onClicked = std::move(cb); if (m_background) m_background->setClickCallback(m_onClicked); }
private:
    void createGraphicsItems();
    void updateLabelPosition();
    QString m_label;
    bool m_visible = true;
    MouseBlockingRoundedRectItem* m_background = nullptr;
    MouseBlockingTextItem* m_textItem = nullptr; // optional text placeholder
    OverlayStyle m_currentStyle;
    std::function<void()> m_onClicked;
};

// Linear horizontal slider (track + fill). Value range [0,1].
class OverlaySliderElement : public OverlayElement {
public:
    OverlaySliderElement(const QString& id = QString());
    ~OverlaySliderElement() override;

    qreal value() const { return m_value; }
    void setValue(qreal v); // clamps and updates fill

    // OverlayElement implementation
    void applyStyle(const OverlayStyle& style) override;
    QSizeF preferredSize(const OverlayStyle& style) const override; // width heuristic, height = max(defaultHeight, track)
    void setSize(const QSizeF& size) override;
    void setPosition(const QPointF& pos) override;
    QGraphicsItem* graphicsItem() override { return m_container; }
    bool isVisible() const override { return m_visible; }
    void setVisible(bool v) override;
    void setState(ElementState s) override; // updates tint

    // Track/fill geometry accessors (for potential hit-testing in migration)
    QRectF trackRect() const { return m_trackRect; }
    QRectF fillRect() const { return m_fillRect; }
private:
    void createGraphicsItems();
    void updateFill();
    qreal m_value = 0.0; // 0..1
    bool m_visible = true;
    MouseBlockingRectItem* m_container = nullptr; // parent container (transparent, still blocks events)
    MouseBlockingRoundedRectItem* m_track = nullptr; // full track
    MouseBlockingRoundedRectItem* m_fill = nullptr;  // filled portion
    OverlayStyle m_currentStyle;
    QRectF m_trackRect;
    QRectF m_fillRect;
};

// Sentinel element representing a row break (no graphics)
class RowBreakElement : public OverlayElement {
public:
    RowBreakElement() : OverlayElement(RowBreak, QString()) {}
    void applyStyle(const OverlayStyle&) override {}
    QSizeF preferredSize(const OverlayStyle&) const override { return QSizeF(0,0); }
    void setSize(const QSizeF&) override {}
    void setPosition(const QPointF&) override {}
    QGraphicsItem* graphicsItem() override { return nullptr; }
    bool isVisible() const override { return true; }
    void setVisible(bool) override {}
};

class OverlayPanel {
public:
    enum Position { Top, Bottom };
    enum Layout { Horizontal, Vertical };
    explicit OverlayPanel(Position position, Layout layout = Horizontal);
    ~OverlayPanel();
    Position position() const { return m_position; }
    Layout layout() const { return m_layout; }
    void setLayout(Layout layout);
    const OverlayStyle& style() const { return m_style; }
    void setStyle(const OverlayStyle& style);
    // Elements management
    void addElement(std::shared_ptr<OverlayElement> element);
    void removeElement(const QString& id);
    void removeElement(std::shared_ptr<OverlayElement> element);
    void clearElements();
    std::shared_ptr<OverlayElement> findElement(const QString& id) const;
    // Convenience factories (return the created shared_ptr for chaining)
    std::shared_ptr<OverlayTextElement> addText(const QString& text, const QString& id = QString());
    std::shared_ptr<OverlayButtonElement> addButton(const QString& label = QString(), const QString& id = QString());
    std::shared_ptr<OverlaySliderElement> addSlider(const QString& id = QString());
    void newRow(); // inserts a row break token
    const QList<std::shared_ptr<OverlayElement>>& elements() const { return m_elements; }
    // Visibility
    bool isVisible() const { return m_visible; }
    void setVisible(bool visible);
    // Scene / parenting
    void setParentItem(QGraphicsItem* parent);
    void setScene(QGraphicsScene* scene);
    QGraphicsScene* scene() const { return m_scene; }
    // Layout & positioning
    void updateLayoutWithAnchor(const QPointF& anchorScenePoint, QGraphicsView* view);
    QSizeF calculateSize() const;
    void setBackgroundVisible(bool visible) { m_backgroundVisible = visible; updateBackground(); }
    bool backgroundVisible() const { return m_backgroundVisible; }
private:
    void createBackground();
    void updateBackground();
    void updateLabelsLayout(); // positions child elements (name retained for continuity)
    QPointF calculatePanelPositionFromAnchor(const QPointF& anchorScenePoint, QGraphicsView* view) const;
    Position m_position;
    Layout m_layout;
    OverlayStyle m_style;
    bool m_visible = true;
    bool m_backgroundVisible = true;
    QList<std::shared_ptr<OverlayElement>> m_elements;
    MouseBlockingRectItem* m_background = nullptr; // parent & backdrop
    QGraphicsItem* m_parentItem = nullptr;     // external parent if any
    QGraphicsScene* m_scene = nullptr;
    QPointF m_currentPosition; // cached scene position (panel top-left)
    QSizeF m_currentSize;      // cached size
        // Store last anchor to allow deferred relayout when becoming visible after view adjustments
        QPointF m_lastAnchorScenePoint;
        bool m_hasLastAnchor = false;
        QGraphicsView* m_lastView = nullptr; // last view used for layout (not owned)
};

#endif // OVERLAYPANELS_H
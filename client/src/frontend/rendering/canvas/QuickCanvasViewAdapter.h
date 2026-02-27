#ifndef QUICKCANVASVIEWADAPTER_H
#define QUICKCANVASVIEWADAPTER_H

#include <QObject>
#include <QVariantList>

class QQuickWidget;
class MediaListModel;

class QuickCanvasViewAdapter : public QObject {
    Q_OBJECT
public:
    explicit QuickCanvasViewAdapter(QQuickWidget* quickWidget, QObject* parent = nullptr);
    ~QuickCanvasViewAdapter() override = default;

    // Called once after the QML root object is ready to wire the stable
    // C++ model to the root's mediaListModel property.
    void initMediaListModel(MediaListModel* model);

    // Updates the legacy JS-array property used by utility functions
    // (hit-testing, selection chrome, input layer, etc.).
    void setMediaModel(const QVariantList& model);

    void setSelectionChromeModel(const QVariantList& model);
    void setSnapGuidesModel(const QVariantList& model);

private:
    QQuickWidget*    m_quickWidget;
    MediaListModel*  m_mediaListModel { nullptr };
};

#endif // QUICKCANVASVIEWADAPTER_H

#ifndef QUICKCANVASVIEWADAPTER_H
#define QUICKCANVASVIEWADAPTER_H

#include <QObject>
#include <QVariantList>

class QQuickWidget;

class QuickCanvasViewAdapter : public QObject {
    Q_OBJECT
public:
    explicit QuickCanvasViewAdapter(QQuickWidget* quickWidget, QObject* parent = nullptr);
    ~QuickCanvasViewAdapter() override = default;

    void setMediaModel(const QVariantList& model);
    void setSelectionChromeModel(const QVariantList& model);
    void setSnapGuidesModel(const QVariantList& model);

private:
    QQuickWidget* m_quickWidget;
};

#endif // QUICKCANVASVIEWADAPTER_H

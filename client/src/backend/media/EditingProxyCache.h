#pragma once

#include "backend/media/ResidentMediaAsset.h"
#include <QObject>
#include <memory>

// One low-priority, cancellable background producer for independent editing
// images. Its disk cache survives occurrences; its decoder and source lease do
// not survive the last owner. Calls are made on the GUI thread.
class EditingProxyCache final : public QObject {
public:
    static EditingProxyCache& instance();
    ~EditingProxyCache() override;
    void acquire(QObject* owner, std::shared_ptr<const ResidentMediaAsset> asset);
    void release(QObject* owner);
    void setEnabled(bool enabled);
    void setInteractive(QObject* owner, bool active);
    void prioritize(QObject* owner, int frameIndex);
    int pendingAssets() const;
private:
    EditingProxyCache();
    struct Impl;
    std::unique_ptr<Impl> d;
    void dispatch();
};

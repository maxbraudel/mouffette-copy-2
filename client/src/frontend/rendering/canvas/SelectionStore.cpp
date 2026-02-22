#include "frontend/rendering/canvas/SelectionStore.h"

QString SelectionStore::selectedMediaId() const {
    return m_selectedMediaId;
}

void SelectionStore::setSelectedMediaId(const QString& mediaId) {
    m_selectedMediaId = mediaId;
}

void SelectionStore::clear() {
    m_selectedMediaId.clear();
}

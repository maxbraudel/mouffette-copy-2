#ifndef SELECTIONSTORE_H
#define SELECTIONSTORE_H

#include <QString>

class SelectionStore {
public:
    QString selectedMediaId() const;
    void setSelectedMediaId(const QString& mediaId);
    void clear();

private:
    QString m_selectedMediaId;
};

#endif // SELECTIONSTORE_H

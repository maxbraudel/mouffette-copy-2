#ifndef POINTERSESSION_H
#define POINTERSESSION_H

#include <QString>

class PointerSession {
public:
    bool draggingMedia() const;
    void setDraggingMedia(bool dragging);

    bool resizeActive() const;
    QString resizeMediaId() const;
    QString resizeHandleId() const;

    void beginResize(const QString& mediaId, const QString& handleId);
    void clearResize();

private:
    bool m_draggingMedia = false;
    bool m_resizeActive = false;
    QString m_resizeMediaId;
    QString m_resizeHandleId;
};

#endif // POINTERSESSION_H

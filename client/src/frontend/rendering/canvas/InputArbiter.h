#ifndef INPUTARBITER_H
#define INPUTARBITER_H

#include <QString>

class InputArbiter {
public:
    enum class Mode {
        Idle,
        Pan,
        Move,
        Resize,
        TextCreate
    };

    bool isIdle() const;
    Mode mode() const;

    bool beginPan();
    bool beginMove(const QString& mediaId);
    bool beginResize(const QString& mediaId, const QString& handleId);
    bool beginTextCreate();

    bool endPan();
    bool endMove(const QString& mediaId);
    bool endResize(const QString& mediaId, const QString& handleId = QString());
    bool endTextCreate();

    bool isSameResizeSession(const QString& mediaId, const QString& handleId) const;
    QString activeMediaId() const;
    QString activeHandleId() const;

    void reset();

private:
    bool begin(Mode requestedMode, const QString& mediaId, const QString& handleId);

    Mode m_mode = Mode::Idle;
    QString m_mediaId;
    QString m_handleId;
};

#endif // INPUTARBITER_H

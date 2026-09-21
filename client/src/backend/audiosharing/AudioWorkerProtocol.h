#pragma once
#include <QCborMap>
#include <QCborValue>
#include <QLocalSocket>
#include <QtEndian>

namespace AudioWorkerProtocol {
inline constexpr qsizetype MaximumMessage = 64 * 1024;
// A busy GUI must not queue seconds of live audio ahead of a mute command.
// Four worst-case Opus frames plus their envelopes fit in this bound.
inline constexpr qint64 MaximumBacklog = 8 * 1024;
inline bool send(QLocalSocket* socket, const QCborMap& message) {
    if (!socket || socket->state() != QLocalSocket::ConnectedState
        || socket->bytesToWrite() > MaximumBacklog) return false;
    const auto body = QCborValue(message).toCbor();
    if (body.size() > MaximumMessage) return false;
    QByteArray frame(4, Qt::Uninitialized);
    qToBigEndian<quint32>(quint32(body.size()), frame.data());
    frame.append(body);
    return socket->write(frame) == frame.size();
}
// Control commands must either arrive or tear down the audio process. Dropping
// a mute/stop/reset would leave an audible or capturing stream alive.
inline bool sendControl(QLocalSocket* socket, const QCborMap& message) {
    if (send(socket, message)) return true;
    if (socket) socket->abort();
    return false;
}
inline bool take(QByteArray& input, QCborMap& message, bool& malformed) {
    malformed = false;
    if (input.size() < 4) return false;
    const quint32 size = qFromBigEndian<quint32>(input.constData());
    if (size > MaximumMessage || size == 0) { malformed = true; return false; }
    if (input.size() < qint64(size) + 4) return false;
    QCborParserError error;
    auto value = QCborValue::fromCbor(input.constData() + 4, size, &error);
    input.remove(0, size + 4);
    if (error.error != QCborError::NoError || !value.isMap()) { malformed = true; return false; }
    message = value.toMap();
    return true;
}
}

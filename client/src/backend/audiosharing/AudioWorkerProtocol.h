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
    if (!socket || socket->state() != QLocalSocket::ConnectedState) return false;
    const auto body = QCborValue(message).toCbor();
    if (body.isEmpty() || body.size() > MaximumMessage
        || socket->bytesToWrite() + body.size() + 4 > MaximumBacklog) return false;
    QByteArray frame(4, Qt::Uninitialized);
    qToBigEndian<quint32>(quint32(body.size()), frame.data());
    frame.append(body);
    const auto written = socket->write(frame);
    // A partial length-prefixed frame cannot be followed by another message:
    // that would permanently desynchronize every subsequent control command.
    if (written != frame.size()) { socket->abort(); return false; }
    return true;
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

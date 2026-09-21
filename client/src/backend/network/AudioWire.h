#pragma once
#include <QByteArray>
#include <QUuid>
#include <QtEndian>

namespace AudioWire {
constexpr int HeaderBytes = 36;
constexpr int MaximumPayloadBytes = 1275;
constexpr quint64 MaximumInteger = 9007199254740991ULL;
inline QByteArray packet(const QString& epoch, quint64 sequence, qint64 timestampUs, const QByteArray& payload) {
    QByteArray result(HeaderBytes, '\0');
    result.replace(qsizetype(0),4,"MAU1",4); result.replace(4,16,QUuid(epoch).toRfc4122());
    qToBigEndian<quint64>(sequence,result.data()+20);
    qToBigEndian<quint64>(quint64(timestampUs),result.data()+28);
    return result+payload;
}
inline QByteArray ack(const QString& epoch, quint64 sequence) {
    QByteArray result(28,'\0'); result.replace(qsizetype(0),4,"MAA1",4);
    result.replace(4,16,QUuid(epoch).toRfc4122());
    qToBigEndian<quint64>(sequence,result.data()+20); return result;
}
struct Frame { QString epoch; quint64 sequence=0; qint64 timestampUs=0; QByteArray payload; };
inline bool parse(const QByteArray& bytes, Frame& frame, bool acknowledgement=false) {
    if (acknowledgement ? bytes.size()!=28 || !bytes.startsWith("MAA1")
        : bytes.size()<=HeaderBytes || bytes.size()>HeaderBytes+MaximumPayloadBytes || !bytes.startsWith("MAU1")) return false;
    const QUuid uuid = QUuid::fromRfc4122(bytes.mid(4,16));
    const auto sequence = qFromBigEndian<quint64>(bytes.constData()+20);
    if(uuid.isNull() || uuid.variant()!=QUuid::DCE || uuid.version()<QUuid::Time || uuid.version()>QUuid::Sha1
        || !sequence || sequence>MaximumInteger) return false;
    frame.epoch=uuid.toString(QUuid::WithoutBraces); frame.sequence=sequence;
    if(!acknowledgement) {
        const auto timestamp=qFromBigEndian<quint64>(bytes.constData()+28);
        if(timestamp>MaximumInteger) return false;
        frame.timestampUs=qint64(timestamp); frame.payload=bytes.mid(HeaderBytes);
    }
    return true;
}
}

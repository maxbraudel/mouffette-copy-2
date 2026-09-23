#pragma once

#include <QByteArray>
#include <QList>
#include <QSet>

// Installed FFmpeg encoders describe the build, not the GPUs in the machine.
// Only exclude a vendor when adapter enumeration conclusively found no such
// device. An unavailable/incomplete inventory preserves FFmpeg's own probing.
struct ScreenEncoderAdapterInventory {
    static constexpr quint32 Nvidia = 0x10de;
    static constexpr quint32 Intel = 0x8086;
    static constexpr quint32 Amd = 0x1002;

    bool complete = false;
    QSet<quint32> hardwareVendors;

    QList<QByteArray> windowsHardwareCandidates() const {
        QList<QByteArray> result;
        if (!complete || hardwareVendors.contains(Nvidia)) result.append("h264_nvenc");
        if (!complete || hardwareVendors.contains(Intel)) result.append("h264_qsv");
        if (!complete || hardwareVendors.contains(Amd)) result.append("h264_amf");
        return result;
    }
};

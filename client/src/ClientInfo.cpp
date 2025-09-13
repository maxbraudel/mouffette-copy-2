#include "ClientInfo.h"
#include <QJsonArray>

// ScreenInfo implementation
QJsonObject ScreenInfo::toJson() const {
    QJsonObject obj;
    obj["id"] = id;
    obj["width"] = width;
    obj["height"] = height;
    obj["x"] = x;
    obj["y"] = y;
    obj["primary"] = primary;
    return obj;
}

ScreenInfo ScreenInfo::fromJson(const QJsonObject& json) {
    ScreenInfo screen;
    screen.id = json["id"].toInt();
    screen.width = json["width"].toInt();
    screen.height = json["height"].toInt();
    screen.x = json["x"].toInt(0);
    screen.y = json["y"].toInt(0);
    screen.primary = json["primary"].toBool();
    return screen;
}

// ClientInfo implementation
ClientInfo::ClientInfo() : m_status("unknown") {
}

ClientInfo::ClientInfo(const QString& id, const QString& machineName, const QString& platform)
    : m_id(id), m_machineName(machineName), m_platform(platform), m_status("connected") {
}

QJsonObject ClientInfo::toJson() const {
    QJsonObject obj;
    obj["id"] = m_id;
    obj["machineName"] = m_machineName;
    obj["platform"] = m_platform;
    obj["status"] = m_status;
    if (m_volumePercent >= 0) obj["volumePercent"] = m_volumePercent;
    
    QJsonArray screensArray;
    for (const auto& screen : m_screens) {
        screensArray.append(screen.toJson());
    }
    obj["screens"] = screensArray;
    
    return obj;
}

ClientInfo ClientInfo::fromJson(const QJsonObject& json) {
    ClientInfo client;
    client.m_id = json["id"].toString();
    client.m_machineName = json["machineName"].toString();
    client.m_platform = json["platform"].toString();
    client.m_status = json["status"].toString();
    client.m_volumePercent = json.contains("volumePercent") ? json["volumePercent"].toInt(-1) : -1;
    
    QJsonArray screensArray = json["screens"].toArray();
    for (const auto& screenValue : screensArray) {
        client.m_screens.append(ScreenInfo::fromJson(screenValue.toObject()));
    }
    
    return client;
}

QString ClientInfo::getDisplayText() const {
    QString platformIcon;
    if (m_platform == "macOS") {
        platformIcon = "🍎";
    } else if (m_platform == "Windows") {
        platformIcon = "🪟";
    } else if (m_platform == "Linux") {
        platformIcon = "🐧";
    } else {
        platformIcon = "💻";
    }

    // Show only platform icon and machine name; omit screens/volume to avoid stale info
    return QString("%1 %2").arg(platformIcon).arg(m_machineName);
}

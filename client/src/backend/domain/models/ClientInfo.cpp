#include "backend/domain/models/ClientInfo.h"
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
    if (!uiZones.isEmpty()) {
        QJsonArray arr; for (const auto& z : uiZones) arr.append(z.toJson()); obj["uiZones"] = arr;
    }
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
    if (json.contains("uiZones") && json.value("uiZones").isArray()) {
        QJsonArray arr = json.value("uiZones").toArray();
        for (const auto& v : arr) screen.uiZones.append(ScreenInfo::UIZone::fromJson(v.toObject()));
    }
    return screen;
}

// ClientInfo implementation
ClientInfo::ClientInfo() : m_status("unknown"), m_fromMemory(false), m_isOnline(false) {
}

ClientInfo::ClientInfo(const QString& id, const QString& machineName, const QString& platform)
    : m_id(id), m_machineName(machineName), m_platform(platform), m_status("connected"), m_fromMemory(false), m_isOnline(true) {
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

QJsonObject ScreenInfo::UIZone::toJson() const {
    QJsonObject obj; 
    obj["type"] = type; 
    obj["x"] = x; 
    obj["y"] = y; 
    obj["width"] = width; 
    obj["height"] = height; 
    return obj;
}

ScreenInfo::UIZone ScreenInfo::UIZone::fromJson(const QJsonObject &json) {
    ScreenInfo::UIZone z; 
    z.type = json.value("type").toString(); 
    z.x = json.value("x").toDouble(); 
    z.y = json.value("y").toDouble(); 
    z.width = json.value("width").toDouble(); 
    z.height = json.value("height").toDouble(); 
    return z;
}

ClientInfo ClientInfo::fromJson(const QJsonObject& json) {
    ClientInfo client;
    client.m_id = json["id"].toString();
    client.m_machineName = json["machineName"].toString();
    client.m_platform = json["platform"].toString();
    client.m_status = json["status"].toString();
    client.m_volumePercent = json.contains("volumePercent") ? json["volumePercent"].toInt(-1) : -1;
    client.m_fromMemory = false;
    client.m_isOnline = true;
    if (json.contains("persistentClientId")) {
        client.m_clientId = json.value("persistentClientId").toString();
    }
    
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
    QString text = QString("%1 %2").arg(platformIcon, m_machineName);
    if (!m_isOnline) {
        if (m_fromMemory) {
            text += QStringLiteral(" (from memory – active instance)");
        } else {
            text += QStringLiteral(" (offline)");
        }
    }
    return text;
}

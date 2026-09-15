#include "backend/domain/models/ClientInfo.h"
#include <QDateTime>
#include <QJsonArray>
#include <QSet>
#include <QStringList>

#include <cmath>

// ScreenInfo implementation
bool ScreenInfo::UIZone::isValid() const {
    static const QSet<QString> allowedTypes = {
        QStringLiteral("taskbar"), QStringLiteral("menu_bar"),
        QStringLiteral("dock")
    };
    const auto bounded = [](qreal value, qreal minimum, qreal maximum) {
        return std::isfinite(value) && std::floor(value) == value
            && value >= minimum && value <= maximum;
    };
    return allowedTypes.contains(type)
        && bounded(x, -1'000'000, 1'000'000)
        && bounded(y, -1'000'000, 1'000'000)
        && bounded(width, 0, 100'000)
        && bounded(height, 0, 100'000);
}

bool ScreenInfo::isValid() const {
    if (id < 0 || id > 1'000'000 || width < 1 || width > 100'000
        || height < 1 || height > 100'000 || x < -1'000'000
        || x > 1'000'000 || y < -1'000'000 || y > 1'000'000
        || uiZones.size() > 16) {
        return false;
    }
    for (const UIZone& zone : uiZones) {
        if (!zone.isValid()) return false;
    }
    return true;
}

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
    : m_id(id), m_machineName(machineName), m_platform(platform), m_status("connected"),
      m_fromMemory(false), m_isOnline(true), m_endpointId(id),
      m_availabilityStatus(QStringLiteral("Available")) {
}

QJsonObject ClientInfo::toJson() const {
    QJsonObject obj;
    obj["installationId"] = m_installationId;
    obj["endpointId"] = m_endpointId;
    obj["instanceId"] = m_instanceId;
    obj["instanceOrdinal"] = m_instanceOrdinal;
    if (!m_runtimeId.isEmpty()) obj["runtimeId"] = m_runtimeId;
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
    client.m_installationId = json.value("installationId").toString();
    client.m_endpointId = json.value("endpointId").toString();
    client.m_instanceId = json.value("instanceId").toString();
    client.m_instanceOrdinal = json.value("instanceOrdinal").toInt(1);
    client.m_runtimeId = json.value("runtimeId").toString();
    client.m_id = client.m_endpointId;
    client.m_machineName = json["machineName"].toString();
    client.m_platform = json["platform"].toString();
    client.m_status = json.value("status").toString(QStringLiteral("Available"));
    client.m_volumePercent = json.contains("volumePercent") ? json["volumePercent"].toInt(-1) : -1;
    client.m_fromMemory = false;
    client.m_isOnline = true;
    client.m_availabilityStatus = client.m_status;
    
    QJsonArray screensArray = json["screens"].toArray();
    for (const auto& screenValue : screensArray) {
        client.m_screens.append(ScreenInfo::fromJson(screenValue.toObject()));
    }
    
    return client;
}

QString ClientInfo::getIdentityDisplayText() const {
    QString platformIcon;
    if (m_platform == "macOS") {
        platformIcon = "(apple)";
    } else if (m_platform == "Windows") {
        platformIcon = "(windows)";
    } else if (m_platform == "Linux") {
        platformIcon = "(linux)";
    } else {
        platformIcon = "💻";
    }

    QString machineName = m_machineName.trimmed().isEmpty()
        ? QStringLiteral("Unnamed client")
        : m_machineName.trimmed();
    if (m_instanceOrdinal >= 2) {
        machineName += QStringLiteral(" — Instance %1").arg(m_instanceOrdinal);
    }
    return QStringLiteral("%1 %2").arg(platformIcon, machineName);
}

QString ClientInfo::availabilityBadgeText() const
{
    // Network presence always wins over stale session state. A durable project
    // remains selectable while its target is disconnected.
    if (!m_isOnline) {
        return m_availabilityStatus.compare(
                   QStringLiteral("Unreachable"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("Unreachable")
            : QStringLiteral("Disconnected");
    }

    const auto normalize = [](const QString& raw) -> QString {
        const QString value = raw.trimmed();
        if (value.compare(QStringLiteral("available"), Qt::CaseInsensitive) == 0) {
            return QStringLiteral("Available");
        }
        if (value.compare(QStringLiteral("connecting"), Qt::CaseInsensitive) == 0
            || value.compare(QStringLiteral("opening"), Qt::CaseInsensitive) == 0) {
            return QStringLiteral("Connecting");
        }
        if (value.compare(QStringLiteral("connected"), Qt::CaseInsensitive) == 0
            || value.compare(QStringLiteral("active"), Qt::CaseInsensitive) == 0) {
            return QStringLiteral("Connected");
        }
        if (value.compare(QStringLiteral("reconnecting"), Qt::CaseInsensitive) == 0
            || value.compare(QStringLiteral("grace"), Qt::CaseInsensitive) == 0) {
            return QStringLiteral("Reconnecting");
        }
        if (value.compare(QStringLiteral("disconnecting"), Qt::CaseInsensitive) == 0
            || value.compare(QStringLiteral("terminating"), Qt::CaseInsensitive) == 0
            || value.compare(QStringLiteral("cleanup_pending"), Qt::CaseInsensitive) == 0) {
            return QStringLiteral("Disconnecting");
        }
        if (value.compare(QStringLiteral("disconnected"), Qt::CaseInsensitive) == 0) {
            return QStringLiteral("Disconnected");
        }
        if (value.compare(QStringLiteral("unreachable"), Qt::CaseInsensitive) == 0) {
            return QStringLiteral("Unreachable");
        }
        return {};
    };

    QString normalized = normalize(m_availabilityStatus);
    if (normalized.isEmpty()) {
        normalized = normalize(m_status);
    }
    return normalized.isEmpty() ? QStringLiteral("Unreachable") : normalized;
}

QString ClientInfo::formatRemainingTime(qint64 remainingMs)
{
    const qint64 clampedMs = qMax<qint64>(0, remainingMs);
    const qint64 totalSeconds = (clampedMs + 999) / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

QString ClientInfo::getProjectSummaryText(qint64 nowMs) const
{
    if (!m_hasProject) {
        return {};
    }

    const qint64 current = nowMs >= 0 ? nowMs : QDateTime::currentMSecsSinceEpoch();
    QStringList parts{QStringLiteral("Project")};
    if (m_remoteSessionCloseAtMs >= current && m_remoteSessionCloseAtMs > 0) {
        parts.append(QStringLiteral("Disconnect in %1")
                         .arg(formatRemainingTime(m_remoteSessionCloseAtMs - current)));
    }
    if (m_projectDeleteAtMs >= current && m_projectDeleteAtMs > 0) {
        parts.append(QStringLiteral("Delete project in %1")
                         .arg(formatRemainingTime(m_projectDeleteAtMs - current)));
    }
    return parts.join(QStringLiteral(" · "));
}

QString ClientInfo::getDisplayText() const
{
    return QStringLiteral("%1 — %2")
        .arg(getIdentityDisplayText(), availabilityBadgeText());
}

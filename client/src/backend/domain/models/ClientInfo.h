#ifndef CLIENTINFO_H
#define CLIENTINFO_H

#include <QString>
#include <QList>
#include <QJsonObject>
#include <QtGlobal>

struct ScreenInfo {
    int id;
    int width;
    int height;
    int x;
    int y;
    bool primary;
    struct UIZone {
        QString type; // menu_bar, dock, taskbar
        int x = 0; // relative to screen origin
        int y = 0;
        int width = 0;
        int height = 0;
        bool operator==(const UIZone& other) const {
            return type == other.type && x == other.x && y == other.y
                && width == other.width && height == other.height;
        }
        bool isValid() const;
        QJsonObject toJson() const;
        static UIZone fromJson(const QJsonObject &json);
    };
    QList<UIZone> uiZones; // per-screen UI overlays
    
    ScreenInfo() : id(0), width(0), height(0), x(0), y(0), primary(false) {}
    ScreenInfo(int id, int w, int h, int x, int y, bool p) : id(id), width(w), height(h), x(x), y(y), primary(p) {}
    
    bool operator==(const ScreenInfo& other) const {
        return id == other.id && width == other.width && height == other.height
            && x == other.x && y == other.y && primary == other.primary && uiZones == other.uiZones;
    }
    bool operator!=(const ScreenInfo& other) const { return !(*this == other); }
    bool isValid() const;
    QJsonObject toJson() const;
    static ScreenInfo fromJson(const QJsonObject& json);
};


class ClientInfo {
public:
    ClientInfo();
    ClientInfo(const QString& id, const QString& machineName, const QString& platform);
    
    // Getters
    QString getId() const { return m_id; }
    QString getMachineName() const { return m_machineName; }
    QString getPlatform() const { return m_platform; }
    QString getStatus() const { return m_status; }
    QList<ScreenInfo> getScreens() const { return m_screens; }
    int getVolumePercent() const { return m_volumePercent; }
    bool isFromMemory() const { return m_fromMemory; }
    void setFromMemory(bool fromMemory) { m_fromMemory = fromMemory; }
    bool isOnline() const { return m_isOnline; }
    void setOnline(bool online) { m_isOnline = online; }
    QString installationId() const { return m_installationId; }
    QString endpointId() const { return m_endpointId; }
    QString instanceId() const { return m_instanceId; }
    int instanceOrdinal() const { return m_instanceOrdinal; }
    QString runtimeId() const { return m_runtimeId; }
    QString availabilityStatus() const { return m_availabilityStatus; }
    QString projectId() const { return m_projectId; }
    bool hasProject() const { return m_hasProject; }
    qint64 projectMediaReleaseAtMs() const { return m_projectMediaReleaseAtMs; }
    qint64 remoteSessionCloseAtMs() const { return m_remoteSessionCloseAtMs; }
    qint64 projectDeleteAtMs() const { return m_projectDeleteAtMs; }
    
    // Setters
    void setId(const QString& id) { m_id = id; }
    void setMachineName(const QString& name) { m_machineName = name; }
    void setPlatform(const QString& platform) { m_platform = platform; }
    void setStatus(const QString& status) { m_status = status; }
    void setScreens(const QList<ScreenInfo>& screens) { m_screens = screens; }
    void setVolumePercent(int v) { m_volumePercent = v; }
    void setInstallationId(const QString& id) { m_installationId = id; }
    void setEndpointId(const QString& id) { m_endpointId = id; m_id = id; }
    void setInstanceId(const QString& id) { m_instanceId = id; }
    void setInstanceOrdinal(int ordinal) { m_instanceOrdinal = ordinal; }
    void setRuntimeId(const QString& id) { m_runtimeId = id; }
    void setAvailabilityStatus(const QString& status) { m_availabilityStatus = status; }
    void setProjectId(const QString& id) { m_projectId = id; }
    void setHasProject(bool value) { m_hasProject = value; }
    void setProjectMediaReleaseAtMs(qint64 value) { m_projectMediaReleaseAtMs = value; }
    void setRemoteSessionCloseAtMs(qint64 value) { m_remoteSessionCloseAtMs = value; }
    void setProjectDeleteAtMs(qint64 value) { m_projectDeleteAtMs = value; }
    
    // JSON serialization
    QJsonObject toJson() const;
    static ClientInfo fromJson(const QJsonObject& json);
    
    // Helper methods
    QString getIdentityDisplayText() const;
    QString availabilityBadgeText() const;
    QString getProjectDeadlineText(qint64 nowMs = -1) const;
    QString getDisplayText() const;
    static QString formatRemainingTime(qint64 remainingMs);
    int getScreenCount() const { return m_screens.size(); }
    
private:
    QString m_id;
    QString m_machineName;
    QString m_platform;
    QString m_status;
    QList<ScreenInfo> m_screens;
    int m_volumePercent = -1; // 0-100, -1 when unknown
    bool m_fromMemory = false;
    bool m_isOnline = true;
    QString m_installationId;
    QString m_endpointId;
    QString m_instanceId;
    int m_instanceOrdinal = 1;
    QString m_runtimeId;
    QString m_availabilityStatus = QStringLiteral("Unreachable");
    QString m_projectId;
    bool m_hasProject = false;
    qint64 m_projectMediaReleaseAtMs = -1;
    qint64 m_remoteSessionCloseAtMs = 0;
    qint64 m_projectDeleteAtMs = 0;
};

#endif // CLIENTINFO_H

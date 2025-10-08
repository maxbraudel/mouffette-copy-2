#ifndef CLIENTINFO_H
#define CLIENTINFO_H

#include <QString>
#include <QList>
#include <QJsonObject>

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
        QJsonObject toJson() const;
        static UIZone fromJson(const QJsonObject &json);
    };
    QList<UIZone> uiZones; // per-screen UI overlays
    
    ScreenInfo() : id(0), width(0), height(0), x(0), y(0), primary(false) {}
    ScreenInfo(int id, int w, int h, int x, int y, bool p) : id(id), width(w), height(h), x(x), y(y), primary(p) {}
    
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
    QString identityKey() const { return m_identityKey; }
    void setIdentityKey(const QString& key) { m_identityKey = key; }
    
    // Setters
    void setId(const QString& id) { m_id = id; }
    void setMachineName(const QString& name) { m_machineName = name; }
    void setPlatform(const QString& platform) { m_platform = platform; }
    void setStatus(const QString& status) { m_status = status; }
    void setScreens(const QList<ScreenInfo>& screens) { m_screens = screens; }
    void setVolumePercent(int v) { m_volumePercent = v; }
    
    // JSON serialization
    QJsonObject toJson() const;
    static ClientInfo fromJson(const QJsonObject& json);
    
    // Helper methods
    QString getDisplayText() const;
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
    QString m_identityKey;
};

#endif // CLIENTINFO_H

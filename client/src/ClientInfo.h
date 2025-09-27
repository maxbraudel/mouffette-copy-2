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
    
    ScreenInfo() : id(0), width(0), height(0), x(0), y(0), primary(false) {}
    ScreenInfo(int id, int w, int h, int x, int y, bool p) : id(id), width(w), height(h), x(x), y(y), primary(p) {}
    
    QJsonObject toJson() const;
    static ScreenInfo fromJson(const QJsonObject& json);
};

struct SystemUIElement {
    QString type; // menu_bar, dock, taskbar
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int screenId = -1; // optional originating screen index
    SystemUIElement() {}
    SystemUIElement(const QString& t, int X, int Y, int W, int H, int sid = -1) : type(t), x(X), y(Y), width(W), height(H), screenId(sid) {}
    QJsonObject toJson() const {
        QJsonObject o; o["type"] = type; o["x"] = x; o["y"] = y; o["width"] = width; o["height"] = height; if (screenId >= 0) o["screenId"] = screenId; return o; }
    static SystemUIElement fromJson(const QJsonObject& o) {
        SystemUIElement e; e.type = o.value("type").toString(); e.x = o.value("x").toInt(); e.y = o.value("y").toInt(); e.width = o.value("width").toInt(); e.height = o.value("height").toInt(); e.screenId = o.value("screenId").toInt(-1); return e; }
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
    QList<SystemUIElement> getSystemUIElements() const { return m_systemUIElements; }
    
    // Setters
    void setId(const QString& id) { m_id = id; }
    void setMachineName(const QString& name) { m_machineName = name; }
    void setPlatform(const QString& platform) { m_platform = platform; }
    void setStatus(const QString& status) { m_status = status; }
    void setScreens(const QList<ScreenInfo>& screens) { m_screens = screens; }
    void setVolumePercent(int v) { m_volumePercent = v; }
    void setSystemUIElements(const QList<SystemUIElement>& elems) { m_systemUIElements = elems; }
    
    // JSON serialization
    QJsonObject toJson() const;
    static ClientInfo fromJson(const QJsonObject& json);
    
    // Helper methods
    QString getDisplayText() const;
    int getScreenCount() const { return m_screens.size(); }
    bool hasSystemUI() const { return !m_systemUIElements.isEmpty(); }
    
private:
    QString m_id;
    QString m_machineName;
    QString m_platform;
    QString m_status;
    QList<ScreenInfo> m_screens;
    int m_volumePercent = -1; // 0-100, -1 when unknown
    QList<SystemUIElement> m_systemUIElements; // OS UI zones (menu bar, dock, taskbar)
};

#endif // CLIENTINFO_H

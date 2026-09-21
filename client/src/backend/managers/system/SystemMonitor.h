#ifndef SYSTEMMONITOR_H
#define SYSTEMMONITOR_H

#include <QObject>
#include <QList>
#include <QPointF>
#include <QString>
#include <functional>
#include "backend/platform/LocalScreenTopology.h"
#include "backend/domain/models/ClientInfo.h"

class QTimer;
class QProcess;
class QScreen;
class ScreenInfo;

/**
 * @brief SystemMonitor - Monitors system information (volume, screens, platform)
 * 
 * This class encapsulates platform-specific system monitoring logic including:
 * - Volume monitoring (macOS async polling, Windows Core Audio)
 * - Screen configuration detection
 * - Platform and machine name detection
 * 
 * Volume monitoring uses asynchronous polling on macOS to avoid UI blocking.
 * On Windows, it queries the system on-demand using Core Audio APIs.
 */
class SystemMonitor : public QObject {
    Q_OBJECT
    
public:
    using ScreenProvider = std::function<QList<LocalScreenTopology::Screen>(bool*)>;
    explicit SystemMonitor(QObject* parent = nullptr, ScreenProvider screenProvider = {});
    ~SystemMonitor() override;
    
    /**
     * @brief Get the current system volume percentage
     * @return Volume level 0-100, or -1 if unknown
     * 
     * On macOS: Returns cached value updated asynchronously
     * On Windows: Queries system directly (fast operation)
     */
    int getSystemVolumePercent();
    
    /**
     * @brief Start volume monitoring
     * 
     * Begins periodic volume polling:
     * - macOS: Starts async osascript polling (~1.2s interval)
     * - Windows: Starts polling timer (~1.2s interval)
     */
    void startVolumeMonitoring();
    
    /**
     * @brief Stop volume monitoring
     */
    void stopVolumeMonitoring();
    
    /**
     * @brief Get information about all local screens
     * @return List of ScreenInfo objects describing each display
     */
    QList<ScreenInfo> getLocalScreenInfo() const;
    bool captureScreenInfo(QList<ScreenInfo>* screens);
    // Exact inventory used by the last published snapshot, including native
    // Windows ordering. Capture must never guess screen IDs from Qt's order.
    QList<LocalScreenTopology::Screen> screenCaptureTopology() const {
        return m_topologyReady ? m_topology : QList<LocalScreenTopology::Screen>{};
    }

    /**
     * Sample the desktop cursor in the advertised screen's local coordinates.
     * Units match ScreenInfo (physical pixels on macOS/Windows, Qt pixels on
     * other platforms). Returns false when the cursor or screen is unavailable.
     */
    bool getLocalCursorPosition(int* screenId, QPointF* screenPosition) const;
    
    /**
     * @brief Get the machine/host name
     * @return Machine name string
     */
    QString getMachineName() const;
    
    /**
     * @brief Get the platform name (macOS, Windows, Linux, etc.)
     * @return Platform identifier string
     */
    QString getPlatformName() const;
    
signals:
    void screenTopologyInvalidated();
    /**
     * @brief Emitted when system volume changes
     * @param volumePercent New volume level (0-100)
     */
    void volumeChanged(int volumePercent);
    
    /**
     * @brief Emitted when screen configuration changes
     * @param screens New screen configuration
     */
    void screenConfigurationChanged(const QList<ScreenInfo>& screens);
    
private:
    void watchScreen(QScreen* screen);
    void scheduleScreenConfigurationChanged();

    // Volume monitoring state
    ScreenProvider m_screenProvider;
    QList<LocalScreenTopology::Screen> m_topology;
    QList<ScreenInfo> m_screens;
    bool m_topologyReady = false;
    int m_cachedSystemVolume = -1;  // Last known value (0-100), -1 = unknown
    QTimer* m_screenChangeTimer = nullptr;
    
#ifdef Q_OS_MACOS
    QProcess* m_volProc = nullptr;  // For async osascript calls
    QTimer* m_volTimer = nullptr;   // Background polling timer
#else
    QTimer* m_volTimer = nullptr;   // Polling timer for non-macOS
#endif
};

#endif // SYSTEMMONITOR_H

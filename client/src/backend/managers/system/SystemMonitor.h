#ifndef SYSTEMMONITOR_H
#define SYSTEMMONITOR_H

#include <QObject>
#include <QList>
#include <QPointF>
#include <QString>
#include <functional>
#include "backend/managers/system/SystemVolumeMonitorBackend.h"
#include "backend/platform/LocalScreenTopology.h"
#include "backend/domain/models/ClientInfo.h"

class QTimer;
class QScreen;
class ScreenInfo;

/**
 * @brief SystemMonitor - Monitors system information (volume, screens, platform)
 * 
 * This class encapsulates platform-specific system monitoring logic including:
 * - Native volume notifications (macOS and Windows Core Audio)
 * - Screen configuration detection
 * - Platform and machine name detection
 * 
 * Native callbacks update a cached value on the owning Qt thread.
 * A recovery timer handles unavailable or replaced output devices.
 */
class SystemMonitor : public QObject {
    Q_OBJECT
    
public:
    using ScreenProvider = std::function<QList<LocalScreenTopology::Screen>(bool*)>;
    using VolumeBackendFactory = std::function<std::unique_ptr<SystemVolumeMonitorBackend>(
        SystemVolumeMonitorBackend::Publish)>;
    explicit SystemMonitor(QObject* parent = nullptr, ScreenProvider screenProvider = {},
                           VolumeBackendFactory volumeBackendFactory = {});
    ~SystemMonitor() override;
    
    /**
     * @brief Get the current system volume percentage
     * @return Volume level 0-100, or -1 if unknown
     * 
     * Returns the cached native value without re-querying the audio device.
     */
    int getSystemVolumePercent();
    
    /**
     * @brief Start volume monitoring
     * 
     * Reads the initial value and subscribes to native change notifications.
     * Calling start again after stop restarts monitoring.
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
     * @param volumePercent New volume level (0-100), or -1 when unavailable
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
    
    VolumeBackendFactory m_volumeBackendFactory;
    std::unique_ptr<SystemVolumeMonitorBackend> m_volumeBackend;
    bool m_volumeMonitoring = false;
};

#endif // SYSTEMMONITOR_H

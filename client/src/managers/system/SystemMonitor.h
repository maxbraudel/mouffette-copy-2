#ifndef SYSTEMMONITOR_H
#define SYSTEMMONITOR_H

#include <QObject>
#include <QList>
#include <QString>

class QTimer;
class QProcess;
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
    explicit SystemMonitor(QObject* parent = nullptr);
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
    // Volume monitoring state
    int m_cachedSystemVolume = -1;  // Last known value (0-100), -1 = unknown
    
#ifdef Q_OS_MACOS
    QProcess* m_volProc = nullptr;  // For async osascript calls
    QTimer* m_volTimer = nullptr;   // Background polling timer
#else
    QTimer* m_volTimer = nullptr;   // Polling timer for non-macOS
#endif
};

#endif // SYSTEMMONITOR_H

#include "backend/managers/system/SystemMonitor.h"
#include "backend/managers/system/ScreenCoordinateMapping.h"
#include "backend/platform/LocalScreenTopology.h"
#include "backend/config/AppConfig.h"
#include "backend/domain/models/ClientInfo.h"
#include <QTimer>
#include <QProcess>
#include <QGuiApplication>
#include <QCursor>
#include <QScreen>
#include <QHostInfo>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#endif

SystemMonitor::SystemMonitor(QObject* parent)
    : QObject(parent)
{
    m_screenChangeTimer = new QTimer(this);
    m_screenChangeTimer->setSingleShot(true);
    m_screenChangeTimer->setInterval(
        AppConfig::instance().screenChangeDebounceMs());
    connect(m_screenChangeTimer, &QTimer::timeout, this, [this]() {
        emit screenConfigurationChanged(getLocalScreenInfo());
    });

    if (QGuiApplication* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        for (QScreen* screen : app->screens()) {
            watchScreen(screen);
        }
        connect(app, &QGuiApplication::screenAdded, this, [this](QScreen* screen) {
            watchScreen(screen);
            scheduleScreenConfigurationChanged();
        });
        connect(app, &QGuiApplication::screenRemoved, this, [this](QScreen*) {
            scheduleScreenConfigurationChanged();
        });
        connect(app, &QGuiApplication::primaryScreenChanged, this, [this](QScreen*) {
            scheduleScreenConfigurationChanged();
        });
    }
}

void SystemMonitor::watchScreen(QScreen* screen) {
    if (!screen) return;
    const auto schedule = [this]() { scheduleScreenConfigurationChanged(); };
    connect(screen, &QScreen::geometryChanged, this, schedule);
    connect(screen, &QScreen::availableGeometryChanged, this, schedule);
    connect(screen, &QScreen::logicalDotsPerInchChanged, this, schedule);
    connect(screen, &QScreen::physicalDotsPerInchChanged, this, schedule);
    connect(screen, &QScreen::virtualGeometryChanged, this, schedule);
}

void SystemMonitor::scheduleScreenConfigurationChanged() {
    if (m_screenChangeTimer) {
        m_screenChangeTimer->start();
    }
}

SystemMonitor::~SystemMonitor() {
    stopVolumeMonitoring();
}

int SystemMonitor::getSystemVolumePercent() {
#ifdef Q_OS_MACOS
    // Return cached value; updated asynchronously in startVolumeMonitoring()
    return m_cachedSystemVolume;
    
#elif defined(Q_OS_WIN)
    // Use Windows Core Audio APIs (MMDevice + IAudioEndpointVolume)
    HRESULT hr;
    IMMDeviceEnumerator* pEnum = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioEndpointVolume* pEndpointVol = nullptr;
    bool coInit = SUCCEEDED(CoInitialize(nullptr));
    int result = -1;
    
    do {
        hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, 
                            IID_IMMDeviceEnumerator, (void**)&pEnum);
        if (FAILED(hr) || !pEnum) break;
        
        hr = pEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
        if (FAILED(hr) || !pDevice) break;
        
        hr = pDevice->Activate(IID_IAudioEndpointVolume, CLSCTX_ALL, nullptr, (void**)&pEndpointVol);
        if (FAILED(hr) || !pEndpointVol) break;
        
        float levelScalar = 0.0f;
        hr = pEndpointVol->GetMasterVolumeLevelScalar(&levelScalar);
        if (SUCCEEDED(hr)) {
            result = static_cast<int>(levelScalar * 100.0f + 0.5f);
            result = std::clamp(result, 0, 100);
        }
    } while (false);
    
    if (pEndpointVol) pEndpointVol->Release();
    if (pDevice) pDevice->Release();
    if (pEnum) pEnum->Release();
    if (coInit) CoUninitialize();
    
    return result;
    
#else
    // Linux or other platforms: not implemented
    return -1;
#endif
}

void SystemMonitor::startVolumeMonitoring() {
#ifdef Q_OS_MACOS
    // Asynchronous polling to avoid blocking the UI thread
    if (!m_volProc) {
        m_volProc = new QProcess(this);
        // No visible window; ensure fast exit
        connect(m_volProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int, QProcess::ExitStatus) {
            const QByteArray out = m_volProc->readAllStandardOutput().trimmed();
            bool ok = false;
            int vol = QString::fromUtf8(out).toInt(&ok);
            if (ok) {
                vol = std::clamp(vol, 0, 100);
                if (vol != m_cachedSystemVolume) {
                    m_cachedSystemVolume = vol;
                    emit volumeChanged(vol);
                }
            }
        });
    }
    
    if (!m_volTimer) {
        m_volTimer = new QTimer(this);
        m_volTimer->setInterval(
            AppConfig::instance().systemVolumePollIntervalMs());
        connect(m_volTimer, &QTimer::timeout, this, [this]() {
            if (m_volProc->state() == QProcess::NotRunning) {
                m_volProc->start("/usr/bin/osascript", 
                               {"-e", "output volume of (get volume settings)"});
            }
        });
        m_volTimer->start();
    }
    
#else
    // Non-macOS: simple polling; Windows call is fast
    if (!m_volTimer) {
        m_volTimer = new QTimer(this);
        m_volTimer->setInterval(
            AppConfig::instance().systemVolumePollIntervalMs());
        connect(m_volTimer, &QTimer::timeout, this, [this]() {
            int v = getSystemVolumePercent();
            if (v != m_cachedSystemVolume) {
                m_cachedSystemVolume = v;
                emit volumeChanged(v);
            }
        });
        m_volTimer->start();
    }
#endif
}

void SystemMonitor::stopVolumeMonitoring() {
#ifdef Q_OS_MACOS
    if (m_volTimer) {
        m_volTimer->stop();
    }
    if (m_volProc && m_volProc->state() != QProcess::NotRunning) {
        m_volProc->kill();
        m_volProc->waitForFinished(AppConfig::instance().processStopTimeoutMs());
    }
#else
    if (m_volTimer) {
        m_volTimer->stop();
    }
#endif
}

QList<ScreenInfo> SystemMonitor::getLocalScreenInfo() const {
    QList<ScreenInfo> screens;
    const auto topology = LocalScreenTopology::screens();
    for (qsizetype index = 0; index < topology.size(); ++index) {
        const auto& screen = topology[index];
        const QRect& geometry = screen.advertisedGeometry;
        screens.append(ScreenInfo(static_cast<int>(index), geometry.width(), geometry.height(),
                                  geometry.x(), geometry.y(), screen.primary));
    }

    return screens;
}

bool SystemMonitor::getLocalCursorPosition(int* screenId,
                                          QPointF* screenPosition) const
{
    if (!screenId || !screenPosition) return false;
    // Screen ids follow enumeration order. Wait until the debounced topology
    // snapshot is published before using ids from a newly changed desktop.
    if (m_screenChangeTimer && m_screenChangeTimer->isActive()) return false;

#ifdef Q_OS_WIN
    // Qt cursor coordinates are logical. Query physical coordinates to match
    // the monitor rectangles advertised by getLocalScreenInfo().
    POINT physicalPosition{};
    if (!GetPhysicalCursorPos(&physicalPosition)) return false;

    const auto topology = LocalScreenTopology::screens();
    for (qsizetype index = 0; index < topology.size(); ++index) {
        const auto& screen = topology[index];
        if (!screen.nativeWindowsCoordinates) break;
        const QPoint position(physicalPosition.x, physicalPosition.y);
        if (!screen.advertisedGeometry.contains(position)) continue;
        *screenId = static_cast<int>(index);
        *screenPosition = position - screen.advertisedGeometry.topLeft();
        return true;
    }
    if (!topology.isEmpty() && topology.first().nativeWindowsCoordinates) return false;
    // Mirror getLocalScreenInfo()'s Qt fallback when native enumeration fails.
#endif

    const QPoint position = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(position);
    if (!screen) return false;
    const int id = QGuiApplication::screens().indexOf(screen);
    if (id < 0) return false;
#ifdef Q_OS_MACOS
    const qreal scale = std::max<qreal>(1.0, screen->devicePixelRatio());
#else
    const qreal scale = 1.0;
#endif
    *screenId = id;
    *screenPosition = ScreenCoordinateMapping::screenLocalPosition(
        position, screen->geometry(), scale);
    return true;
}

QString SystemMonitor::getMachineName() const {
    QString hostName = QHostInfo::localHostName();
    if (hostName.isEmpty()) {
        hostName = "Unknown Machine";
    }
    return hostName;
}

QString SystemMonitor::getPlatformName() const {
#ifdef Q_OS_MACOS
    return "macOS";
#elif defined(Q_OS_WIN)
    return "Windows";
#elif defined(Q_OS_LINUX)
    return "Linux";
#else
    return "Unknown";
#endif
}

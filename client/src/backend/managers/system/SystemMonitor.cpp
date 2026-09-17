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

SystemMonitor::SystemMonitor(QObject* parent, ScreenProvider screenProvider)
    : QObject(parent), m_screenProvider(std::move(screenProvider))
{
    m_screenChangeTimer = new QTimer(this);
    m_screenChangeTimer->setSingleShot(true);
    m_screenChangeTimer->setInterval(
        AppConfig::instance().screenChangeDebounceMs());
    connect(m_screenChangeTimer, &QTimer::timeout, this, [this]() {
        QList<ScreenInfo> screens;
        if (captureScreenInfo(&screens)) emit screenConfigurationChanged(screens);
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
    m_topologyReady = false;
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
    return m_screens;
}

bool SystemMonitor::captureScreenInfo(QList<ScreenInfo>* screens)
{
    bool valid = false;
    const auto topology = m_screenProvider
        ? m_screenProvider(&valid) : LocalScreenTopology::screens(false, &valid);
    if (!valid) {
        m_topologyReady = false;
        return false;
    }
    QList<ScreenInfo> result;
    for (qsizetype index = 0; index < topology.size(); ++index) {
        const auto& entry = topology[index];
        const QRect g = entry.advertisedGeometry;
        const QRect work = entry.advertisedAvailableGeometry.intersected(g);
        ScreenInfo screen(static_cast<int>(index), g.width(), g.height(), g.x(), g.y(), entry.primary);
        if (!work.isEmpty()) {
            const auto zone = [&](const QString& type, int x, int y, int w, int h) {
                if (w > 0 && h > 0) screen.uiZones.append({type, x, y, w, h});
            };
#ifdef Q_OS_WIN
            const QString top = QStringLiteral("taskbar"), other = top;
#elif defined(Q_OS_MACOS)
            const QString top = QStringLiteral("menu_bar"), other = QStringLiteral("dock");
#else
            const QString top = QStringLiteral("taskbar"), other = top;
#endif
            zone(top, 0, 0, g.width(), work.top() - g.top());
            zone(other, 0, work.bottom() - g.top() + 1, g.width(), g.bottom() - work.bottom());
            zone(other, 0, 0, work.left() - g.left(), g.height());
            zone(other, work.right() - g.left() + 1, 0, g.right() - work.right(), g.height());
        }
        result.append(screen);
    }
    // The same immutable enumeration supplies both publication and cursor ids.
    // Never enumerate monitors on the 16-ms mouse sampling path.
    m_topology = topology;
    m_screens = result;
    m_topologyReady = !(m_screenChangeTimer && m_screenChangeTimer->isActive());
    if (screens) *screens = result;
    return true;
}

bool SystemMonitor::getLocalCursorPosition(int* screenId, QPointF* screenPosition) const
{
    if (!screenId || !screenPosition || !m_topologyReady) return false;
#ifdef Q_OS_WIN
    if (!m_topology.isEmpty() && m_topology.first().nativeWindowsCoordinates) {
        POINT physicalPosition{};
        if (!GetPhysicalCursorPos(&physicalPosition)) return false;
        const QPoint position(physicalPosition.x, physicalPosition.y);
        for (qsizetype i = 0; i < m_topology.size(); ++i) {
            const QRect bounds = m_topology[i].advertisedGeometry;
            if (!bounds.contains(position)) continue;
            *screenId = static_cast<int>(i);
            *screenPosition = position - bounds.topLeft();
            return true;
        }
        return false;
    }
#endif
    const QPoint position = QCursor::pos();
    for (qsizetype i = 0; i < m_topology.size(); ++i) {
        const auto& entry = m_topology[i];
        if (!entry.geometry.contains(position)) continue;
        const qreal scale = entry.geometry.width() > 0
            ? qreal(entry.advertisedGeometry.width()) / entry.geometry.width() : 1.0;
        *screenId = static_cast<int>(i);
        *screenPosition = ScreenCoordinateMapping::screenLocalPosition(position, entry.geometry, scale);
        return true;
    }
    return false;
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

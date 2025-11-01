#include "backend/managers/system/SystemMonitor.h"
#include "backend/domain/models/ClientInfo.h"
#include <QTimer>
#include <QProcess>
#include <QGuiApplication>
#include <QScreen>
#include <QHostInfo>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <array>

namespace {

constexpr size_t kMaxEnumeratedMonitors = 16; // Defensive upper bound

struct WinMonRect {
    RECT rc;
    RECT rcWork;
    bool primary;
};

struct MonitorEnumContext {
    std::array<WinMonRect, kMaxEnumeratedMonitors> monitors{};
    size_t count = 0;
    bool overflow = false;
};

static BOOL CALLBACK MouffetteEnumMonProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* ctx = reinterpret_cast<MonitorEnumContext*>(lParam);
    if (!ctx) {
        return FALSE;
    }

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) {
        return TRUE; // Skip but continue enumeration
    }

    if (ctx->count >= ctx->monitors.size()) {
        ctx->overflow = true;
        return TRUE; // Continue but do not write past the buffer
    }

    WinMonRect entry{};
    entry.rc = mi.rcMonitor;
    entry.rcWork = mi.rcWork;
    entry.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    ctx->monitors[ctx->count++] = entry;
    return TRUE;
}

} // namespace
#endif

SystemMonitor::SystemMonitor(QObject* parent)
    : QObject(parent)
{
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
        m_volTimer->setInterval(1200); // ~1.2s cadence
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
        m_volTimer->setInterval(1200);
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
        m_volProc->waitForFinished(100);
    }
#else
    if (m_volTimer) {
        m_volTimer->stop();
    }
#endif
}

QList<ScreenInfo> SystemMonitor::getLocalScreenInfo() const {
    QList<ScreenInfo> screens;
    
#ifdef Q_OS_WIN
    // Use WinAPI to enumerate monitors in PHYSICAL pixels with correct origins (no logical gaps)
    MonitorEnumContext ctx;
    EnumDisplayMonitors(nullptr, nullptr, MouffetteEnumMonProc, reinterpret_cast<LPARAM>(&ctx));
    
    if (ctx.count == 0) {
        // Fallback to Qt API
        QList<QScreen*> screenList = QGuiApplication::screens();
        for (int i = 0; i < screenList.size(); ++i) {
            QScreen* s = screenList[i];
            const QRect g = s->geometry();
            screens.append(ScreenInfo(i, g.width(), g.height(), g.x(), g.y(), 
                                    s == QGuiApplication::primaryScreen()));
        }
    } else {
        for (size_t i = 0; i < ctx.count; ++i) {
            const auto& m = ctx.monitors[i];
            const int px = m.rc.left;
            const int py = m.rc.top;
            const int pw = m.rc.right - m.rc.left;
            const int ph = m.rc.bottom - m.rc.top;
            // For ScreenInfo we keep absolute coordinates so cursor mapping (physical) matches exactly
            screens.append(ScreenInfo(static_cast<int>(i), pw, ph, px, py, m.primary));
        }
    }
#else
    // macOS, Linux, etc. - use Qt's screen API
    QList<QScreen*> screenList = QGuiApplication::screens();
    for (int i = 0; i < screenList.size(); ++i) {
        QScreen* screen = screenList[i];
        QRect geometry = screen->geometry();
        bool isPrimary = (screen == QGuiApplication::primaryScreen());
        screens.append(ScreenInfo(i, geometry.width(), geometry.height(), 
                                geometry.x(), geometry.y(), isPrimary));
    }
#endif
    
    return screens;
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

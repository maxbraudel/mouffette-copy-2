#include "backend/managers/system/SystemVolumeMonitorBackend.h"
#include "backend/config/AppConfig.h"

#include <QDebug>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cmath>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

namespace {
struct AudioChanges {
    std::atomic_bool device{false};
    std::atomic_bool volume{false};
};

// Core Audio owns callback threads. A shared mailbox outlives any in-flight
// notification, contains no QObject pointers, and never waits on the UI thread.
class AudioNotifications final : public IMMNotificationClient,
                                 public IAudioEndpointVolumeCallback
{
public:
    explicit AudioNotifications(std::shared_ptr<AudioChanges> changes)
        : m_changes(std::move(changes)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IMMNotificationClient)
            *object = static_cast<IMMNotificationClient*>(this);
        else if (iid == IID_IAudioEndpointVolumeCallback)
            *object = static_cast<IAudioEndpointVolumeCallback*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --m_references;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override
    {
        if (!data) return E_POINTER;
        m_changes->volume.store(true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        if (flow == eRender && (role == eMultimedia || role == eConsole))
            m_changes->device.store(true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return deviceChanged(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return deviceChanged(); }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return deviceChanged(); }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
    { return deviceChanged(); }

private:
    HRESULT deviceChanged() { m_changes->device.store(true); return S_OK; }
    std::atomic<ULONG> m_references{1};
    std::shared_ptr<AudioChanges> m_changes;
};

class WindowsVolumeMonitor final : public SystemVolumeMonitorBackend
{
public:
    explicit WindowsVolumeMonitor(Publish publish)
        : SystemVolumeMonitorBackend(std::move(publish))
    {
        // Only drain notification flags at display cadence. Native audio reads
        // happen on changes; the slower timer is recovery/fallback only.
        m_events.setInterval(16);
        m_events.setTimerType(Qt::PreciseTimer);
        connect(&m_events, &QTimer::timeout, this, [this] {
            if (m_deviceChanges && m_deviceChanges->device.exchange(false)) refreshDevice();
            if (m_volumeChanges && m_volumeChanges->volume.exchange(false)) readVolume();
        });
        m_recovery.setInterval(AppConfig::instance().systemVolumePollIntervalMs());
        connect(&m_recovery, &QTimer::timeout, this, [this] { refreshDevice(); });
    }
    ~WindowsVolumeMonitor() override { stop(); }

    void start() override
    {
        if (m_running) return;
        m_running = true;
        refreshDevice();
        m_events.start();
        m_recovery.start();
    }

    void stop() override
    {
        m_running = false;
        m_events.stop();
        m_recovery.stop();
        releaseEndpoint();
        releaseEnumerator();
        if (m_ownsCom) CoUninitialize();
        m_ownsCom = false;
        m_comReady = false;
    }

private:
    void reportFailure(const char* stage, HRESULT result)
    {
        const QString error = QStringLiteral("%1 (HRESULT 0x%2)")
            .arg(QString::fromLatin1(stage), QString::number(static_cast<quint32>(result), 16));
        if (error == m_lastError) return;
        m_lastError = error;
        qWarning().noquote() << "System volume: Windows Core Audio:" << error;
    }

    void releaseEndpoint()
    {
        if (m_endpoint && m_volumeSubscribed)
            m_endpoint->UnregisterControlChangeNotify(m_volumeNotifications);
        m_volumeSubscribed = false;
        if (m_volumeNotifications) m_volumeNotifications->Release();
        m_volumeNotifications = nullptr;
        if (m_endpoint) m_endpoint->Release();
        m_endpoint = nullptr;
        m_endpointId.clear();
        m_volumeChanges.reset();
    }

    void releaseEnumerator()
    {
        if (m_enumerator && m_deviceSubscribed)
            m_enumerator->UnregisterEndpointNotificationCallback(m_deviceNotifications);
        m_deviceSubscribed = false;
        if (m_deviceNotifications) m_deviceNotifications->Release();
        m_deviceNotifications = nullptr;
        if (m_enumerator) m_enumerator->Release();
        m_enumerator = nullptr;
        m_deviceChanges.reset();
    }

    void refreshDevice()
    {
        if (!m_running) return;
        if (!m_comReady) {
            const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            // Qt may already own a different apartment. Use it without owning
            // its COM initialization; balance only our successful call.
            m_ownsCom = SUCCEEDED(result);
            m_comReady = m_ownsCom || result == RPC_E_CHANGED_MODE;
            if (!m_comReady) {
                reportFailure("CoInitializeEx", result);
                publish(-1);
                return;
            }
        }
        if (!m_enumerator) {
            const HRESULT result = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr,
                CLSCTX_ALL, IID_IMMDeviceEnumerator, reinterpret_cast<void**>(&m_enumerator));
            if (FAILED(result)) {
                reportFailure("CoCreateInstance(MMDeviceEnumerator)", result);
                publish(-1);
                return;
            }
            m_deviceChanges = std::make_shared<AudioChanges>();
            m_deviceNotifications = new AudioNotifications(m_deviceChanges);
        }
        if (!m_deviceSubscribed) {
            const HRESULT result = m_enumerator->RegisterEndpointNotificationCallback(m_deviceNotifications);
            m_deviceSubscribed = SUCCEEDED(result);
            if (FAILED(result)) reportFailure("RegisterEndpointNotificationCallback", result);
        }

        IMMDevice* device = nullptr;
        HRESULT result = m_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        // If the multimedia role is absent, try the console default as well.
        if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            result = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(result)) {
            reportFailure("GetDefaultAudioEndpoint", result);
            releaseEndpoint();
            // Recreate stale service connections on the next retry. Retain
            // notifications when the only problem is an absent output device.
            if (result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) releaseEnumerator();
            publish(-1);
            return;
        }
        LPWSTR nativeId = nullptr;
        result = device->GetId(&nativeId);
        if (FAILED(result)) {
            device->Release();
            reportFailure("IMMDevice::GetId", result);
            releaseEndpoint();
            publish(-1);
            return;
        }
        const QString id = QString::fromWCharArray(nativeId);
        CoTaskMemFree(nativeId);
        if (!m_endpoint || id != m_endpointId) {
            releaseEndpoint();
            result = device->Activate(IID_IAudioEndpointVolume, CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&m_endpoint));
            if (SUCCEEDED(result)) {
                m_endpointId = id;
                m_volumeChanges = std::make_shared<AudioChanges>();
                m_volumeNotifications = new AudioNotifications(m_volumeChanges);
            }
        }
        device->Release();
        if (FAILED(result)) {
            reportFailure("IMMDevice::Activate(IAudioEndpointVolume)", result);
            publish(-1);
            return;
        }
        if (!m_volumeSubscribed) {
            result = m_endpoint->RegisterControlChangeNotify(m_volumeNotifications);
            m_volumeSubscribed = SUCCEEDED(result);
            if (FAILED(result)) reportFailure("RegisterControlChangeNotify", result);
        }
        readVolume();
    }

    void readVolume()
    {
        if (!m_endpoint) return;
        float scalar = 0.0f;
        const HRESULT result = m_endpoint->GetMasterVolumeLevelScalar(&scalar);
        if (FAILED(result) || !std::isfinite(scalar)) {
            reportFailure("GetMasterVolumeLevelScalar", FAILED(result) ? result : E_UNEXPECTED);
            releaseEndpoint();
            publish(-1);
            return;
        }
        if (m_volumeSubscribed && m_deviceSubscribed) m_lastError.clear();
        publish(static_cast<int>(std::lround(std::clamp(scalar, 0.0f, 1.0f) * 100.0f)));
    }

    QTimer m_events;
    QTimer m_recovery;
    bool m_running = false;
    bool m_comReady = false;
    bool m_ownsCom = false;
    bool m_deviceSubscribed = false;
    bool m_volumeSubscribed = false;
    IMMDeviceEnumerator* m_enumerator = nullptr;
    IAudioEndpointVolume* m_endpoint = nullptr;
    AudioNotifications* m_deviceNotifications = nullptr;
    AudioNotifications* m_volumeNotifications = nullptr;
    std::shared_ptr<AudioChanges> m_deviceChanges;
    std::shared_ptr<AudioChanges> m_volumeChanges;
    QString m_endpointId;
    QString m_lastError;
};
}

std::unique_ptr<SystemVolumeMonitorBackend>
createSystemVolumeMonitorBackend(SystemVolumeMonitorBackend::Publish publish)
{
    return std::make_unique<WindowsVolumeMonitor>(std::move(publish));
}

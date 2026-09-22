#include "backend/audiosharing/SystemAudioCapture.h"
#include "backend/audiosharing/MediaCaptureClock.h"
#include "backend/audiosharing/AudioCaptureTiming.h"
#include <QSysInfo>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <thread>
#include <qt_windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#endif
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

namespace {
// MinGW does not yet ship this SDK header. Older NTDDI targets can also
// hide its declarations; the API itself is still discovered at runtime.
// ABI: Microsoft win32metadata, WinSDK/um/audioclientactivationparams.h.
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
constexpr wchar_t VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK[] = L"VAD\\Process_Loopback";
enum PROCESS_LOOPBACK_MODE : int {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
};
struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
};
enum AUDIOCLIENT_ACTIVATION_TYPE : int {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
};
struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union { AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams; };
};
#endif
static_assert(sizeof(AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS) == 8);
static_assert(sizeof(AUDIOCLIENT_ACTIVATION_PARAMS) == 12);
static_assert(offsetof(AUDIOCLIENT_ACTIVATION_PARAMS, ProcessLoopbackParams) == 4);

template<class T> class Com {
public:
    T* value = nullptr;
    ~Com() { reset(); }
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* operator->() const { return value; }
    T** put() { reset(); return &value; }
    void reset() { if (value) value->Release(); value = nullptr; }
};
class Completion final : public IActivateAudioInterfaceCompletionHandler {
public:
    std::atomic<ULONG> refs{1};
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_PENDING;
    HRESULT marshalerResult = E_PENDING;
    Com<IAudioClient> client;
    Com<IUnknown> marshaler;
    // Activation is asynchronous even when stop/timeout returns to the caller.
    // Keep the blob alive with the completion handler retained by Windows.
    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    PROPVARIANT activation{};
    Completion() {
        marshalerResult = CoCreateFreeThreadedMarshaler(this, marshaler.put());
        parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        parameters.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
        parameters.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
        activation.vt = VT_BLOB;
        activation.blob.cbSize = sizeof(parameters);
        activation.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);
    }
    ~Completion() { if (event) CloseHandle(event); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IActivateAudioInterfaceCompletionHandler)
            || id == __uuidof(IAgileObject)) {
            *output = static_cast<IActivateAudioInterfaceCompletionHandler*>(this); AddRef(); return S_OK;
        }
        if (id == __uuidof(IMarshal) && marshaler.value) return marshaler->QueryInterface(id, output);
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto remaining = --refs; if (!remaining) delete this; return remaining; }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activated = E_FAIL;
        Com<IUnknown> unknown;
        result = operation->GetActivateResult(&activated, unknown.put());
        if (SUCCEEDED(result)) result = activated;
        if (SUCCEEDED(result)) result = unknown.value
            ? unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(client.put())) : E_POINTER;
        SetEvent(event); return S_OK;
    }
};
QString describe(const char* stage, HRESULT code) {
    wchar_t* native = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, DWORD(code), 0, reinterpret_cast<wchar_t*>(&native), 0, nullptr);
    const QString detail = native ? QString::fromWCharArray(native).trimmed() : QString();
    if (native) LocalFree(native);
    return QStringLiteral("%1 (0x%2) on %3, kernel %4. %5")
        .arg(QString::fromLatin1(stage)).arg(quint32(code), 8, 16, QLatin1Char('0'))
        .arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion(), detail).trimmed();
}
class WindowsAudioCapture final : public SystemAudioCapture {
public:
    HANDLE stopped = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread thread;
    ~WindowsAudioCapture() override { stop(); if (stopped) CloseHandle(stopped); }
    void start(Pcm pcm, State state) override {
        stop();
        if (!stopped) { state(false, QStringLiteral("Cannot create audio capture event")); return; }
        ResetEvent(stopped);
        thread = std::thread([this, pcm = std::move(pcm), state = std::move(state)] { capture(pcm, state); });
    }
    void stop() override { if (stopped) SetEvent(stopped); if (thread.joinable()) thread.join(); }
    void capture(const Pcm& pcm, const State& state) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized)) { state(false, describe("Audio COM initialization failed", initialized)); return; }
        // The scope releases all interfaces before uninitializing their apartment.
        run(pcm, state);
        CoUninitialize();
    }
    void run(const Pcm& pcm, const State& state) {
        using Activate = HRESULT(WINAPI*)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*, IActivateAudioInterfaceAsyncOperation**);
        // A cancelled asynchronous activation may still call back after run()
        // returns. Keep its implementation loaded for the worker's lifetime.
        static HMODULE module = LoadLibraryW(L"Mmdevapi.dll");
        auto activate = module ? reinterpret_cast<Activate>(GetProcAddress(module, "ActivateAudioInterfaceAsync")) : nullptr;
        if (!activate) { state(false, QStringLiteral("Process audio capture is unavailable on this Windows installation")); return; }
        auto* completion = new Completion;
        struct CompletionGuard { Completion* value; ~CompletionGuard() { value->Release(); } } completionGuard{completion};
        if (!completion->event) { state(false, QStringLiteral("Cannot create audio activation event")); return; }
        if (FAILED(completion->marshalerResult)) {
            state(false, describe("Audio callback marshaling failed", completion->marshalerResult)); return;
        }
        Com<IActivateAudioInterfaceAsyncOperation> operation;
        HRESULT result = activate(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &completion->activation,
                                  completion, operation.put());
        if (FAILED(result)) { state(false, describe("Process audio capture activation failed", result)); return; }
        HANDLE activationEvents[]{stopped, completion->event};
        const DWORD activated = WaitForMultipleObjects(2, activationEvents, FALSE, 5000);
        if (activated == WAIT_OBJECT_0) return;
        if (activated == WAIT_FAILED) { state(false, describe("Audio activation wait failed", HRESULT_FROM_WIN32(GetLastError()))); return; }
        if (activated != WAIT_OBJECT_0 + 1) { state(false, QStringLiteral("Process audio capture activation timed out")); return; }
        if (FAILED(completion->result)) { state(false, describe("Process audio capture is unavailable", completion->result)); return; }
        // Process loopback has no physical mix format; GetMixFormat is not
        // implemented. Windows converts application streams to this fixed PCM.
        WAVEFORMATEXTENSIBLE format{};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = 2; format.Format.nSamplesPerSec = 48000;
        format.Format.wBitsPerSample = 32; format.Format.nBlockAlign = 8; format.Format.nAvgBytesPerSec = 384000;
        format.Format.cbSize = sizeof(format) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        // Use the SDK's GUID initializer: MinGW declares the named GUID as an
        // external symbol that is not supplied by ole32/uuid at link time.
        format.SubFormat = GUID{STATIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT};
        auto* client = completion->client.value;
        // Explicit float layout, as used by native process-loopback clients.
        // There is no physical endpoint mix format to request conversion from.
        result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            200000, 0, &format.Format, nullptr);
        if (FAILED(result)) { state(false, describe("Process audio format initialization failed", result)); return; }
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        struct EventGuard { HANDLE value; ~EventGuard() { if (value) CloseHandle(value); } } eventGuard{ready};
        if (!ready) { state(false, QStringLiteral("Cannot create audio buffer event")); return; }
        result = client->SetEventHandle(ready);
        Com<IAudioCaptureClient> captureClient;
        if (SUCCEEDED(result)) result = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(captureClient.put()));
        if (SUCCEEDED(result)) result = client->Start();
        if (FAILED(result)) { state(false, describe("Process audio capture startup failed", result)); return; }
        state(true, {});
        AudioCaptureTiming timing;
        HANDLE events[]{stopped, ready};
        bool running = true;
        while (running) {
            const DWORD event = WaitForMultipleObjects(2, events, FALSE, 100);
            if (event == WAIT_OBJECT_0) break;
            if (event != WAIT_OBJECT_0 + 1 && event != WAIT_TIMEOUT) {
                state(false, describe("Audio buffer wait failed", HRESULT_FROM_WIN32(GetLastError()))); break;
            }
            UINT32 frames = 0;
            while (SUCCEEDED(result = captureClient->GetNextPacketSize(&frames)) && frames) {
                BYTE* bytes = nullptr; DWORD flags = 0; UINT64 devicePosition = 0, qpcPosition = 0;
                result = captureClient->GetBuffer(&bytes, &frames, &flags, &devicePosition, &qpcPosition);
                if (FAILED(result)) break;
                if (frames <= 48000) {
                    QByteArray samples(qsizetype(frames) * 2 * sizeof(float), Qt::Uninitialized);
                    auto* output = reinterpret_cast<float*>(samples.data());
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !bytes) std::fill(output, output + frames * 2, 0.0f);
                    else std::memcpy(output, bytes, size_t(samples.size()));
                    // WASAPI's QPC timestamp and MediaCaptureClock share the
                    // native epoch. Keep the sample's age instead of concealing
                    // capture backlog behind a first-arrival clock offset.
                    qint64 native = qint64(qpcPosition / 10);
                    const auto now = MediaCaptureClock::nowUs();
                    if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || !native) native = now;
                    result = captureClient->ReleaseBuffer(frames);
                    if (FAILED(result)) break;
                    timing.observe("wasapi-process-loopback", now, native, native, int(frames));
                    if (WaitForSingleObject(stopped, 0) == WAIT_OBJECT_0) { running = false; break; }
                    pcm(std::move(samples), native);
                } else {
                    result = captureClient->ReleaseBuffer(frames);
                    if (FAILED(result)) break;
                }
            }
            if (FAILED(result)) { state(false, describe("Process audio capture stopped", result)); break; }
        }
        client->Stop();
    }
};
}
std::unique_ptr<SystemAudioCapture> createSystemAudioCapture() { return std::make_unique<WindowsAudioCapture>(); }
void initializeAudioWorkerPlatform() {}

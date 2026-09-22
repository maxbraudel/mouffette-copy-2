#include "backend/audiosharing/SystemAudioCapture.h"
#include "backend/audiosharing/MediaCaptureClock.h"
#include "backend/audiosharing/AudioCaptureTiming.h"
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
    Com<IAudioClient> client;
    Com<IUnknown> marshaler;
    Completion() { CoCreateFreeThreadedMarshaler(this, marshaler.put()); }
    ~Completion() { if (event) CloseHandle(event); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
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
        if (SUCCEEDED(result)) result = unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(client.put()));
        SetEvent(event); return S_OK;
    }
};
QString describe(const char* stage, HRESULT code) {
    return QStringLiteral("%1 (0x%2). System audio sharing requires an updated Windows 10 version 2004 or later, or Windows 11.")
        .arg(QString::fromLatin1(stage), QString::number(quint32(code), 16));
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
        HMODULE module = LoadLibraryW(L"Mmdevapi.dll");
        struct ModuleGuard { HMODULE value; ~ModuleGuard() { if (value) FreeLibrary(value); } } moduleGuard{module};
        auto activate = module ? reinterpret_cast<Activate>(GetProcAddress(module, "ActivateAudioInterfaceAsync")) : nullptr;
        if (!activate) { state(false, QStringLiteral("Process audio capture is unavailable on this Windows installation")); return; }
        AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
        parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        parameters.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
        parameters.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT activation{};
        activation.vt = VT_BLOB; activation.blob.cbSize = sizeof(parameters);
        activation.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);
        auto* completion = new Completion;
        struct CompletionGuard { Completion* value; ~CompletionGuard() { value->Release(); } } completionGuard{completion};
        if (!completion->event) { state(false, QStringLiteral("Cannot create audio activation event")); return; }
        Com<IActivateAudioInterfaceAsyncOperation> operation;
        HRESULT result = activate(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activation,
                                  completion, operation.put());
        if (FAILED(result)) { state(false, describe("Process audio capture activation failed", result)); return; }
        HANDLE activationEvents[]{stopped, completion->event};
        const DWORD activated = WaitForMultipleObjects(2, activationEvents, FALSE, 5000);
        if (activated == WAIT_OBJECT_0) return;
        if (activated != WAIT_OBJECT_0 + 1) { state(false, QStringLiteral("Process audio capture activation timed out")); return; }
        if (FAILED(completion->result)) { state(false, describe("Process audio capture is unavailable", completion->result)); return; }
        // Process loopback has no physical mix format; GetMixFormat is not
        // implemented. Windows converts application streams to this fixed PCM.
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; format.nChannels = 2; format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 32; format.nBlockAlign = 8; format.nAvgBytesPerSec = 384000;
        auto* client = completion->client.value;
        result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
            0, 0, &format, nullptr);
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
            if (event != WAIT_OBJECT_0 + 1 && event != WAIT_TIMEOUT) break;
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
                    captureClient->ReleaseBuffer(frames);
                    timing.observe("wasapi-process-loopback", now, native, native, int(frames));
                    if (WaitForSingleObject(stopped, 0) == WAIT_OBJECT_0) { running = false; break; }
                    pcm(std::move(samples), native);
                } else captureClient->ReleaseBuffer(frames);
            }
            if (FAILED(result)) { state(false, describe("Process audio capture stopped", result)); break; }
        }
        client->Stop();
    }
};
}
std::unique_ptr<SystemAudioCapture> createSystemAudioCapture() { return std::make_unique<WindowsAudioCapture>(); }
void initializeAudioWorkerPlatform() {}

#include "backend/screensharing/WindowsScreenCapture.h"
#include "backend/audiosharing/MediaCaptureClock.h"
#include <QDebug>
#include <QScreen>
#include <QScopeGuard>
#include <QtGui/qscreen_platform.h>
#include <unknwn.h>
#include <winrt/base.h>
// Required by some Windows SDK/C++/WinRT combinations (also used by Qt).
namespace winrt::impl {
template <typename Async>
auto wait_for(Async const& async, Windows::Foundation::TimeSpan const& timeout);
}
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

namespace {
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

struct CaptureSession {
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    ~CaptureSession() {
        // Close can throw after device removal; teardown must still release
        // both objects and must never terminate the application.
        try { if (session) session.Close(); } catch (const winrt::hresult_error&) {}
        try { if (pool) pool.Close(); } catch (const winrt::hresult_error&) {}
    }
};
}

struct WindowsScreenCapture::Private {
    std::atomic<bool> stopped{true};
    std::atomic<int> fps{30};
    std::thread worker;

    void run(HMONITOR monitor, const FrameCallback& deliver) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const auto apartment = qScopeGuard([] { winrt::uninit_apartment(); });
        if (!GraphicsCaptureSession::IsSupported())
            winrt::throw_hresult(E_NOTIMPL);

        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> context;
        HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            device.put(), nullptr, context.put());
        if (FAILED(result)) {
            device = nullptr; context = nullptr;
            winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                device.put(), nullptr, context.put()));
        }
        const auto dxgi = device.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        const auto captureDevice = inspectable.as<IDirect3DDevice>();
        const auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(factory->CreateForMonitor(monitor, winrt::guid_of<GraphicsCaptureItem>(),
                                                      winrt::put_abi(item)));
        auto size = item.Size();
        if (size.Width <= 0 || size.Height <= 0) winrt::throw_hresult(E_INVALIDARG);
        CaptureSession capture;
        capture.pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        capture.session = capture.pool.CreateCaptureSession(item);
        // Mouffette draws its remote cursor independently.
        if (const auto cursor = capture.session.try_as<IGraphicsCaptureSession2>())
            cursor.IsCursorCaptureEnabled(false);
        capture.session.StartCapture();
        qInfo() << "[ScreenSharing] Windows Graphics Capture started";

        winrt::com_ptr<ID3D11Texture2D> staging;
        Direct3D11CaptureFrame pending{nullptr};
        const auto closePending = qScopeGuard([&] {
            try { if (pending) pending.Close(); } catch (const winrt::hresult_error&) {}
        });
        qint64 lastDeliveredUs = 0;
        const auto startedAtUs = MediaCaptureClock::nowUs();
        while (!stopped.load()) {
            if (!lastDeliveredUs && MediaCaptureClock::nowUs() - startedAtUs > 5000000)
                winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            if (auto next = capture.pool.TryGetNextFrame()) {
                if (pending) pending.Close();
                pending = std::move(next);
            }
            // Retain the newest surface until it is due. Dropping an early
            // update outright would lose the final image of a now-idle desktop.
            const auto now = MediaCaptureClock::nowUs();
            if (!pending || now - lastDeliveredUs < 1000000 / fps.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            auto frame = std::exchange(pending, nullptr);
            auto close = qScopeGuard([&] {
                try { frame.Close(); } catch (const winrt::hresult_error&) {}
            });
            const auto nextSize = frame.ContentSize();
            if (nextSize.Width <= 0 || nextSize.Height <= 0) continue;
            if (nextSize.Width != size.Width || nextSize.Height != size.Height) {
                size = nextSize;
                staging = nullptr;
                frame.Close();
                close.dismiss();
                capture.pool.Recreate(captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
                deliver({}); // fence encoders across a native size discontinuity
                continue;
            }
            const auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            // A resize can arrive before the pool has produced its new surface.
            if (description.Width != UINT(size.Width) || description.Height != UINT(size.Height)) continue;
            if (!staging) {
                description.Usage = D3D11_USAGE_STAGING;
                description.BindFlags = 0;
                description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                description.MiscFlags = 0;
                winrt::check_hresult(device->CreateTexture2D(&description, nullptr, staging.put()));
            }
            QVideoFrame output(QVideoFrameFormat(QSize(size.Width, size.Height), QVideoFrameFormat::Format_BGRA8888));
            if (!output.map(QVideoFrame::WriteOnly)) winrt::throw_hresult(E_OUTOFMEMORY);
            auto unmapOutput = qScopeGuard([&] { output.unmap(); });
            context->CopyResource(staging.get(), texture.get());
            D3D11_MAPPED_SUBRESOURCE pixels{};
            winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &pixels));
            {
                const auto unmap = qScopeGuard([&] { context->Unmap(staging.get(), 0); });
                for (int y = 0; y < size.Height; ++y)
                    std::memcpy(output.bits(0) + y * output.bytesPerLine(0),
                        static_cast<const char*>(pixels.pData) + size_t(y) * pixels.RowPitch, size_t(size.Width) * 4);
            }
            output.unmap();
            unmapOutput.dismiss();
            // SystemRelativeTime is QPC expressed in 100 ns units, like WASAPI.
            output.setStartTime(frame.SystemRelativeTime().count() / 10);
            lastDeliveredUs = now;
            if (!stopped.load()) deliver(output);
        }
    }
};

WindowsScreenCapture::WindowsScreenCapture() : d(std::make_unique<Private>()) {}
WindowsScreenCapture::~WindowsScreenCapture() { stop(); }
bool WindowsScreenCapture::start(QScreen* screen, FrameCallback frame, ErrorCallback error) {
    stop();
    auto* native = screen ? screen->nativeInterface<QNativeInterface::QWindowsScreen>() : nullptr;
    const auto monitor = native ? native->handle() : nullptr;
    if (!monitor) {
        error(ScreenCaptureError::CaptureFailed, QStringLiteral("Windows Graphics Capture could not find the selected monitor"));
        return false;
    }
    d->stopped = false;
    d->worker = std::thread([state = d.get(), monitor, frame = std::move(frame), error = std::move(error)] {
        try { state->run(monitor, frame); }
        catch (const winrt::hresult_error& failure) {
            if (!state->stopped.exchange(true))
                error(ScreenCaptureError::CaptureFailed,
                    QStringLiteral("Windows Graphics Capture failed (0x%1): %2")
                        .arg(quint32(failure.code().value), 8, 16, QLatin1Char('0'))
                        .arg(QString::fromWCharArray(failure.message().c_str())));
        } catch (const std::exception& failure) {
            if (!state->stopped.exchange(true))
                error(ScreenCaptureError::CaptureFailed, QString::fromUtf8(failure.what()));
        }
    });
    return true;
}
void WindowsScreenCapture::stop() {
    d->stopped = true;
    if (d->worker.joinable()) d->worker.join();
}
bool WindowsScreenCapture::isActive() const { return !d->stopped.load(); }
void WindowsScreenCapture::setProfile(const ScreenStreamProfile& profile) {
    d->fps = std::clamp(profile.framesPerSecond, 1, 60);
}

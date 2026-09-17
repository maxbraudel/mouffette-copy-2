#include "backend/platform/windows/WindowsWindowManager.h"

#include <QDebug>
#include <QGuiApplication>
#include <QWindow>
#include <QVariant>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <servprov.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace {
using Microsoft::WRL::ComPtr;

// Windows exposes no public pin-window API. These shell interfaces implement
// Task View's "Show this window on all desktops" on Windows 10/11. Keep the
// private ABI isolated and query it dynamically; never pin the entire app.
// Interface layout: https://github.com/Ciantic/VirtualDesktopAccessor
const CLSID immersiveShell = {0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};
const GUID viewCollectionId = {0x1841C6D7, 0x4F9D, 0x42C0, {0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5}};
const GUID pinnedAppsService = {0xB5A399E7, 0x1C87, 0x46B8, {0x88, 0xE9, 0xFC, 0x57, 0x47, 0xB1, 0x71, 0xBD}};
const GUID pinnedAppsId = {0x4CE81583, 0x1E4C, 0x4632, {0xA6, 0x21, 0x07, 0xA5, 0x35, 0x43, 0x14, 0x8F}};

struct ApplicationViewCollection : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetViews(IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByZOrder(IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(PCWSTR, IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND, IUnknown**) = 0;
};

struct VirtualDesktopPinnedApps : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsAppIdPinned(PCWSTR, BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE PinAppID(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnpinAppID(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsViewPinned(IUnknown*, BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE PinView(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnpinView(IUnknown*) = 0;
};

bool pinWindow(HWND hwnd)
{
    // Reacquiring the services also recovers from an Explorer restart.
    ComPtr<IServiceProvider> shell;
    HRESULT result = CoCreateInstance(immersiveShell, nullptr, CLSCTX_LOCAL_SERVER,
                                     IID_IServiceProvider, reinterpret_cast<void**>(shell.GetAddressOf()));
    if (FAILED(result)) return false;
    ComPtr<ApplicationViewCollection> views;
    result = shell->QueryService(viewCollectionId, viewCollectionId,
                                reinterpret_cast<void**>(views.GetAddressOf()));
    if (FAILED(result)) return false;
    ComPtr<VirtualDesktopPinnedApps> pins;
    result = shell->QueryService(pinnedAppsService, pinnedAppsId,
                                reinterpret_cast<void**>(pins.GetAddressOf()));
    if (FAILED(result)) return false;
    ComPtr<IUnknown> view;
    if (FAILED(views->GetViewForHwnd(hwnd, view.GetAddressOf())) || !view) return false;
    BOOL pinned = FALSE;
    if (FAILED(pins->IsViewPinned(view.Get(), &pinned))) return false;
    return pinned || SUCCEEDED(pins->PinView(view.Get()));
}

void followCurrentDesktop(HWND hwnd)
{
    // Best effort with the documented API if a future shell drops the pin ABI.
    // Only use a window confirmed to be on the current desktop as the source.
    ComPtr<IVirtualDesktopManager> desktops;
    if (FAILED(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(desktops.GetAddressOf())))) return;
    BOOL current = TRUE;
    if (FAILED(desktops->IsWindowOnCurrentVirtualDesktop(hwnd, &current)) || current) return;
    const HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == hwnd) return;
    if (FAILED(desktops->IsWindowOnCurrentVirtualDesktop(foreground, &current)) || !current) return;
    GUID desktopId{};
    if (SUCCEEDED(desktops->GetWindowDesktopId(foreground, &desktopId))
        && !IsEqualGUID(desktopId, GUID_NULL)) {
        desktops->MoveWindowToDesktop(hwnd, desktopId);
    }
}
}

void WindowsWindowManager::keepAboveAndOnAllDesktops(QWindow* window, QWindow* preceding,
                                                   bool preserveOrderBelow)
{
    if (!window || QGuiApplication::platformName() != QLatin1String("windows")) return;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!IsWindow(hwnd) || IsIconic(hwnd) || !IsWindowVisible(hwnd)) return;

    // Do not take keyboard focus or disturb the current bounds when enforcing.
    const HWND after = preceding ? reinterpret_cast<HWND>(preceding->winId()) : HWND_TOPMOST;
    bool correctlyOrdered = false;
    if (GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) {
        for (HWND candidate = GetWindow(hwnd, GW_HWNDPREV); candidate;
             candidate = GetWindow(candidate, GW_HWNDPREV)) {
            if (!IsWindowVisible(candidate)) continue;
            correctlyOrdered = candidate == after;
            if (correctlyOrdered || !preserveOrderBelow) break;
        }
        if (!preceding) {
            correctlyOrdered = true;
            for (HWND candidate = GetWindow(hwnd, GW_HWNDPREV); candidate;
                 candidate = GetWindow(candidate, GW_HWNDPREV)) {
                if (IsWindowVisible(candidate)) { correctlyOrdered = false; break; }
            }
        }
    }
    if (!correctlyOrdered
        && !SetWindowPos(hwnd, after, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
        // Rate-limit errors by native handle; a new handle gets a fresh check.
        const qulonglong failedHandle = reinterpret_cast<qulonglong>(hwnd);
        if (window->property("mouffetteStackingFailure").toULongLong() != failedHandle) {
            window->setProperty("mouffetteStackingFailure", failedHandle);
            qWarning() << "Window priority failed" << window->objectName() << GetLastError();
        }
    }

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE) {
        if (!pinWindow(hwnd)) {
            static bool warned = false;
            if (!warned) {
                qWarning() << "Window pinning unavailable; following the active virtual desktop instead.";
                warned = true;
            }
            followCurrentDesktop(hwnd);
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

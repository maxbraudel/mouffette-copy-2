#include "backend/platform/windows/WindowsWindowManager.h"
#include "AppBuildConfig.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
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
#include <wtypes.h>
#include <propkey.h>
#include <propvarutil.h>
#include <servprov.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace {
using Microsoft::WRL::ComPtr;

HRESULT setWindowStringProperty(IPropertyStore* store, const PROPERTYKEY& key, const QString& value)
{
    PROPVARIANT variant{};
    const HRESULT initialized = InitPropVariantFromString(
        reinterpret_cast<PCWSTR>(value.utf16()), &variant);
    if (FAILED(initialized)) return initialized;
    const HRESULT result = store->SetValue(key, variant);
    PropVariantClear(&variant);
    return result;
}

void configureTaskbarRelaunch(QWindow* window, HWND hwnd)
{
    const qulonglong handle = reinterpret_cast<qulonglong>(hwnd);
    if (window->property("mouffetteTaskbarHandle").toULongLong() == handle) return;

    const QString exe = QDir::toNativeSeparators(QGuiApplication::applicationFilePath());
    const QString launcher = QDir(QGuiApplication::applicationDirPath()).filePath(
        QStringLiteral("Mouffette-taskbar-launch.ps1"));
    QString command = QStringLiteral("\"%1\"").arg(exe);
    if (QFileInfo::exists(launcher)) {
        command = QStringLiteral("powershell.exe -NoProfile -WindowStyle Hidden "
                                 "-ExecutionPolicy Bypass -File \"%1\"")
                      .arg(QDir::toNativeSeparators(launcher));
    }

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) return;
    ComPtr<IPropertyStore> store;
    HRESULT configured = SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(store.GetAddressOf()));
    if (SUCCEEDED(configured)) {
        configured = setWindowStringProperty(store.Get(), PKEY_AppUserModel_RelaunchCommand, command);
        if (SUCCEEDED(configured)) configured = setWindowStringProperty(store.Get(),
            PKEY_AppUserModel_RelaunchDisplayNameResource, QStringLiteral("Mouffette"));
        if (SUCCEEDED(configured)) configured = setWindowStringProperty(store.Get(),
            PKEY_AppUserModel_ID, QStringLiteral(MOUFFETTE_BUNDLE_IDENTIFIER));
    }
    if (SUCCEEDED(configured)) window->setProperty("mouffetteTaskbarHandle", handle);
    else if (window->property("mouffetteTaskbarFailureHandle").toULongLong() != handle) {
        window->setProperty("mouffetteTaskbarFailureHandle", handle);
        qWarning() << "Could not configure Mouffette taskbar relaunch command" << Qt::hex << configured;
    }
    store.Reset();
    if (SUCCEEDED(com)) CoUninitialize();
}

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

bool setWindowPinned(HWND hwnd, bool enabled)
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
    return bool(pinned) == enabled || SUCCEEDED(enabled
        ? pins->PinView(view.Get()) : pins->UnpinView(view.Get()));
}

bool followCurrentDesktop(HWND hwnd, bool allowReferenceWindow = false)
{
    ComPtr<IVirtualDesktopManager> desktops;
    if (FAILED(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(desktops.GetAddressOf())))) return false;
    BOOL current = FALSE;
    if (SUCCEEDED(desktops->IsWindowOnCurrentVirtualDesktop(hwnd, &current)) && current) return true;
    GUID desktopId{};
    const HWND foreground = GetForegroundWindow();
    if (foreground && foreground != hwnd
        && SUCCEEDED(desktops->IsWindowOnCurrentVirtualDesktop(foreground, &current)) && current) {
        if (FAILED(desktops->GetWindowDesktopId(foreground, &desktopId))) desktopId = GUID_NULL;
    }
    const auto moveAndVerify = [&](const GUID& id) {
        BOOL onCurrent = FALSE;
        return !IsEqualGUID(id, GUID_NULL)
            && SUCCEEDED(desktops->MoveWindowToDesktop(hwnd, id))
            && SUCCEEDED(desktops->IsWindowOnCurrentVirtualDesktop(hwnd, &onCurrent))
            && onCurrent;
    };
    if (moveAndVerify(desktopId)) return true;
    if (allowReferenceWindow) {
        // Explorer's taskbar has no desktop ID. A shown ordinary Qt window
        // receives one, even on an otherwise empty virtual desktop.
        QWindow reference;
        reference.setFlags(Qt::Window);
        reference.setGeometry(-32000, -32000, 1, 1);
        reference.show();
        for (int attempt = 0; attempt < 10; ++attempt) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
            if (SUCCEEDED(desktops->GetWindowDesktopId(
                    reinterpret_cast<HWND>(reference.winId()), &desktopId))
                && !IsEqualGUID(desktopId, GUID_NULL)) break;
            desktopId = GUID_NULL;
            Sleep(10);
        }
        reference.hide();
    }
    return moveAndVerify(desktopId);
}
}

void WindowsWindowManager::configureControlWindow(QWindow* window, bool alwaysOnTop)
{
    if (!window || QGuiApplication::platformName() != QLatin1String("windows")) return;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!IsWindow(hwnd)) return;
    configureTaskbarRelaunch(window, hwnd);
    if (alwaysOnTop) return;
    if (GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void WindowsWindowManager::moveToCurrentDesktop(QWindow* window)
{
    if (!window || QGuiApplication::platformName() != QLatin1String("windows")) return;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE) {
        const HWND hwnd = reinterpret_cast<HWND>(window->winId());
        // View pinning prevents the public move API from selecting one desktop.
        setWindowPinned(hwnd, false);
        if (!followCurrentDesktop(hwnd, true) && IsWindowVisible(hwnd)
            && window->property("mouffetteDesktopMoveFailureHandle").toULongLong()
                != reinterpret_cast<qulonglong>(hwnd)) {
            window->setProperty("mouffetteDesktopMoveFailureHandle",
                                reinterpret_cast<qulonglong>(hwnd));
            qWarning() << "Could not move Mouffette to the current virtual desktop";
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

bool WindowsWindowManager::isOnCurrentDesktop(QWindow* window)
{
    if (!window || !window->handle()) return false;
    if (QGuiApplication::platformName() != QLatin1String("windows")) return window->isVisible();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    BOOL current = FALSE;
    ComPtr<IVirtualDesktopManager> desktops;
    if ((SUCCEEDED(com) || com == RPC_E_CHANGED_MODE)
        && SUCCEEDED(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(desktops.GetAddressOf()))))
        desktops->IsWindowOnCurrentVirtualDesktop(reinterpret_cast<HWND>(window->winId()), &current);
    desktops.Reset();
    if (SUCCEEDED(com)) CoUninitialize();
    return current;
}

void WindowsWindowManager::keepAbove(QWindow* window, QWindow* preceding,
                                     bool preserveOrderBelow, bool pinOnAllDesktops)
{
    if (!window || QGuiApplication::platformName() != QLatin1String("windows")) return;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!IsWindow(hwnd) || IsIconic(hwnd) || !IsWindowVisible(hwnd)) return;

    // Do not take keyboard focus or disturb the current bounds when enforcing.
    const HWND after = preceding ? reinterpret_cast<HWND>(preceding->winId()) : HWND_TOPMOST;
    bool correctlyOrdered = false;
    if (GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) {
        if (preserveOrderBelow) {
            // Raising a control window here can put it above its own dialogs.
            // The scene pass already keeps scenes above this process's UI.
            correctlyOrdered = true;
        } else if (preceding) {
            for (HWND candidate = GetWindow(hwnd, GW_HWNDPREV); candidate;
                 candidate = GetWindow(candidate, GW_HWNDPREV)) {
                if (!IsWindowVisible(candidate)) continue;
                correctlyOrdered = candidate == after;
                if (correctlyOrdered || !preserveOrderBelow) break;
            }
        } else {
            // Unrelated topmost windows are allowed to remain above this one.
            // Otherwise two Mouffette processes promote themselves every tick.
            correctlyOrdered = true;
            if (!preserveOrderBelow) {
                // A scene still belongs above its own control window/dialogs.
                DWORD processId = 0;
                GetWindowThreadProcessId(hwnd, &processId);
                for (HWND candidate = GetWindow(hwnd, GW_HWNDPREV); candidate;
                     candidate = GetWindow(candidate, GW_HWNDPREV)) {
                    DWORD candidateProcessId = 0;
                    GetWindowThreadProcessId(candidate, &candidateProcessId);
                    if (IsWindowVisible(candidate) && candidateProcessId == processId) {
                        correctlyOrdered = false;
                        break;
                    }
                }
            }
        }
    }
    if (!correctlyOrdered
        && !SetWindowPos(hwnd, after, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
        // Rate-limit errors by native handle; a new handle gets a fresh check.
        const DWORD error = GetLastError();
        const qulonglong failedHandle = reinterpret_cast<qulonglong>(hwnd);
        if (window->property("mouffetteStackingFailure").toULongLong() != failedHandle) {
            window->setProperty("mouffetteStackingFailure", failedHandle);
            qWarning() << "Window priority failed" << window->objectName() << error;
        }
    }

    if (preserveOrderBelow) {
        // Qt can leave a transient dialog behind its topmost owner after the
        // control window is reopened. Repair only this process's owner group.
        EnumWindows([](HWND candidate, LPARAM value) -> BOOL {
            const HWND owner = reinterpret_cast<HWND>(value);
            if (GetWindow(candidate, GW_OWNER) != owner || !IsWindowVisible(candidate)) return TRUE;
            bool ownerAboveDialog = false;
            for (HWND above = GetWindow(candidate, GW_HWNDPREV); above;
                 above = GetWindow(above, GW_HWNDPREV)) {
                if (above == owner) { ownerAboveDialog = true; break; }
            }
            if (ownerAboveDialog) {
                const HWND previous = GetWindow(owner, GW_HWNDPREV);
                SetWindowPos(candidate, previous ? previous : HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(hwnd));
    }

    if (!pinOnAllDesktops) return;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE) {
        if (!setWindowPinned(hwnd, true)) {
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

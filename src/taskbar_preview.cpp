#include "appearance.h"
#include "geometry.h"
#include <ocidl.h>
#undef GetCurrentTime
#include "taskbar_preview.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <atomic>
#include <cmath>
#include <cwchar>
#include <oleacc.h>
#include <string_view>

namespace fullwintb::appearance {
namespace {

namespace xaml = winrt::Windows::UI::Xaml;
constexpr UINT updatePreview = WM_APP + 1;
constexpr wchar_t previewClass[] = L"FullWinTB.ThumbnailPosition.v" FULLWINTB_VERSION_WIDE;
constexpr wchar_t surfaceProperty[] = L"FullWinTB.ThumbnailSurface";
constexpr wchar_t islandClass[] = L"XamlExplorerHostIslandWindow";
constexpr wchar_t legacyClass[] = L"TaskListThumbnailWnd";

std::atomic<HWND> previewControl = nullptr;
std::atomic<HWND> taskbarWindow = nullptr;
std::atomic<LONG> appDisplacement = 0;
std::atomic<bool> previewPending = false;

bool ClassIs(HWND window, const wchar_t* expected) {
    wchar_t name[64]{};
    return GetClassNameW(window, name, ARRAYSIZE(name)) && wcscmp(name, expected) == 0;
}

HWND FindSurface(HWND taskbar);
void QueuePreview(HWND control);

xaml::FrameworkElement NamedChild(const xaml::FrameworkElement& parent, std::wstring_view name) {
    if (!parent) return nullptr;
    const int count = xaml::Media::VisualTreeHelper::GetChildrenCount(parent);
    for (int index = 0; index < count; ++index) {
        const auto child = xaml::Media::VisualTreeHelper::GetChild(parent, index).try_as<xaml::FrameworkElement>();
        if (child && child.Name() == name) return child;
    }
    return nullptr;
}

struct PreviewThread {
    HWND control = nullptr;
    HWND surface = nullptr;
    InstanceHandle handle = 0;
    winrt::weak_ref<xaml::FrameworkElement> frame;
    winrt::weak_ref<xaml::FrameworkElement> canvas;
    winrt::weak_ref<xaml::FrameworkElement> grid;
    winrt::event_token layout{};
    std::optional<winrt::Windows::Foundation::Numerics::float3> original;
    winrt::Windows::Foundation::Numerics::float3 applied{};
    std::optional<bool> opensBelow;

    void ResetPosition() noexcept {
        if (original) {
            try {
                if (const auto carrier = canvas.get(); carrier && carrier.Translation() == applied)
                    carrier.Translation(*original);
            } catch (const winrt::hresult_error&) {
                // Explorer can dispose of a flyout during this update.
            }
            original.reset();
        }
        opensBelow.reset();
    }

    void Clear() noexcept {
        if (const auto element = frame.get(); element && layout) element.LayoutUpdated(layout);
        ResetPosition();
        frame = {};
        canvas = {};
        grid = {};
        layout = {};
        handle = 0;
        surface = nullptr;
        if (control) RemovePropW(control, surfaceProperty);
    }

    void Apply() noexcept try {
        const HWND taskbar = taskbarWindow.load();
        const LONG offset = appDisplacement.load();
        if (!taskbar || !offset) {
            if (original) ResetPosition();
            return;
        }
        const auto flyout = frame.get();
        if (!flyout) return;
        auto carrier = canvas.get();
        auto element = grid.get();
        if (!carrier || !element) {
            ResetPosition();
            carrier = NamedChild(flyout, L"HoverFlyoutCanvas");
            element = NamedChild(carrier, L"HoverFlyoutGrid");
            if (!carrier || !element) return;
            canvas = winrt::make_weak(carrier);
            grid = winrt::make_weak(element);
        }
        if (!surface || !IsWindow(surface)) {
            surface = FindSurface(taskbar);
            if (surface) SetPropW(control, surfaceProperty, surface);
            else RemovePropW(control, surfaceProperty);
        }
        RECT dock{}, host{};
        MONITORINFO monitor{sizeof(monitor)};
        if (!surface || !ReadDockBounds(taskbar, dock) ||
            !GetWindowRect(surface, &host) ||
            !GetMonitorInfoW(MonitorFromWindow(surface, MONITOR_DEFAULTTONEAREST), &monitor)) return;

        const UINT dpi = GetDpiForWindow(surface);
        const double scale = dpi / 96.0;
        const auto nativeTranslation = carrier.Translation();
        if (!original || nativeTranslation != applied) original = nativeTranslation;
        const auto canvasOffset = carrier.ActualOffset();
        const auto gridOffset = element.ActualOffset();
        const auto size = element.ActualSize();
        // These are arranged bounds, independent of the shell's thumbnail
        // animation and of the translation we applied on the previous frame.
        const auto shape = flyout.TransformToVisual(nullptr).TransformBounds({
            canvasOffset.x + gridOffset.x + original->x, canvasOffset.y + gridOffset.y + original->y,
            size.x, size.y});
        const LONG height = std::lround(shape.Height * scale);
        if (!dpi || height <= 0 || height >= host.bottom - host.top) return;

        RECT available{};
        if (!IntersectRect(&available, &host, &monitor.rcMonitor)) return;
        const bool nativeBelow = Near(dock.top, monitor.rcMonitor.top);
        OffsetRect(&dock, 0, offset);
        if (!IsWindowVisible(surface)) opensBelow.reset();
        const auto placement = PlacePopup(dock, available, height, MulDiv(8, dpi, 96),
                                          opensBelow.value_or(nativeBelow));
        opensBelow = placement.below;
        auto target = *original;
        target.y += static_cast<float>((placement.top - host.top) / scale - shape.Y);
        if (std::abs(target.y - nativeTranslation.y) >= 0.25f) {
            carrier.Translation(target);
            applied = target;
        } else {
            applied = nativeTranslation;
        }
    } catch (const winrt::hresult_error&) {
        // A thumbnail can be removed while Explorer arranges its flyout.
    }
};

thread_local PreviewThread preview;

HWND FindSurface(HWND taskbar) {
    const HMONITOR monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONULL);
    if (!monitor) return nullptr;
    const DWORD explorer = GetCurrentProcessId();
    HWND candidate = nullptr;
    // Explorer can host the native island window on a different thread from its XAML controls.
    for (HWND window = nullptr; (window = FindWindowExW(nullptr, window, islandClass, nullptr));) {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process != explorer || MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != monitor ||
            !FindWindowExW(window, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)) continue;
        if (IsWindowVisible(window)) return window;
        if (!candidate) candidate = window;
    }
    return candidate;
}

LRESULT CALLBACK PreviewProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == updatePreview) {
        previewPending = false;
        preview.Apply();
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

HWND OpenPreviewControl() {
    if (preview.control) return preview.control;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&PreviewProc), &module)) return nullptr;
    const WNDCLASSW spec{.lpfnWndProc = PreviewProc, .hInstance = module, .lpszClassName = previewClass};
    if (!RegisterClassW(&spec) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    preview.control = CreateWindowExW(0, previewClass, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE,
                                      nullptr, module, nullptr);
    return preview.control;
}

void QueuePreview(HWND control) {
    if (control && !previewPending.exchange(true) && !PostMessageW(control, updatePreview, 0, 0)) {
        previewPending = false;
        previewControl.compare_exchange_strong(control, nullptr);
    }
}

struct LegacyPreview {
    HWINEVENTHOOK hook = nullptr;
    HWND window = nullptr;
    LONG movedTop = 0;
    LONG oldTop = 0;
    std::optional<bool> opensBelow;

    void Reset() {
        RECT current{};
        if (window && IsWindowVisible(window) && ClassIs(window, legacyClass) &&
            GetWindowRect(window, &current) && current.top == movedTop)
            Position(window, current.left, oldTop);
        window = nullptr;
        opensBelow.reset();
    }

    static void Position(HWND window, LONG x, LONG y) {
        SetWindowPos(window, nullptr, x, y, 0, 0,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOOWNERZORDER |
                     SWP_NOSENDCHANGING | SWP_ASYNCWINDOWPOS);
    }

    void Apply(HWND taskbar, LONG offset) {
        RECT popup{}, dock{};
        MONITORINFO monitor{sizeof(monitor)};
        if (!window || !IsWindowVisible(window) || !ClassIs(window, legacyClass) ||
            !GetWindowRect(window, &popup) || !ReadDockBounds(taskbar, dock) ||
            !GetMonitorInfoW(MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        const bool below = Near(dock.top, monitor.rcMonitor.top);
        OffsetRect(&dock, 0, offset);
        const auto placement = PlacePopup(dock, monitor.rcMonitor, popup.bottom - popup.top,
                                          MulDiv(8, GetDpiForWindow(window), 96), opensBelow.value_or(below));
        opensBelow = placement.below;
        if (popup.top != placement.top) {
            Position(window, popup.left, placement.top);
            movedTop = placement.top;
        }
    }
};

LegacyPreview legacy;

void CALLBACK LegacyShown(HWINEVENTHOOK hook, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD) {
    if (hook != legacy.hook || object != OBJID_WINDOW || child != CHILDID_SELF ||
        !ClassIs(window, legacyClass) || !IsWindowVisible(window)) return;
    const bool different = legacy.window != window;
    legacy.opensBelow.reset();
    if (different) {
        legacy.Reset();
        legacy.window = window;
    }
    RECT original{};
    if (GetWindowRect(window, &original) && (different || original.top != legacy.movedTop))
        legacy.oldTop = legacy.movedTop = original.top;
    legacy.Apply(taskbarWindow.load(), appDisplacement.load());
}

} // namespace

void SetThumbnailFlyout(InstanceHandle handle, const xaml::FrameworkElement& frame) {
    if (preview.handle == handle || !OpenPreviewControl()) return;
    preview.Clear();
    preview.handle = handle;
    preview.frame = winrt::make_weak(frame);
    preview.layout = frame.LayoutUpdated([](const auto&, const auto&) {
        if (appDisplacement.load()) QueuePreview(preview.control);
    });
    previewControl = preview.control;
    QueuePreview(preview.control);
}

void RemoveThumbnailFlyout(InstanceHandle handle) {
    if (preview.handle == handle) preview.Clear();
}

void FollowTaskbarThumbnails(HWND taskbar, LONG offset) {
    const HWND previousBar = taskbarWindow.exchange(taskbar);
    const LONG previousOffset = appDisplacement.exchange(offset);
    if (taskbar == previousBar && previousOffset == offset) return;
    QueuePreview(previewControl.load());
    if (!offset) {
        if (legacy.hook) UnhookWinEvent(std::exchange(legacy.hook, nullptr));
        legacy.Reset();
        return;
    }
    if (!previousOffset) {
        legacy.hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, LegacyShown,
                                     GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
        for (HWND window = nullptr; (window = FindWindowExW(nullptr, window, legacyClass, nullptr));) {
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(window)) {
                LegacyShown(legacy.hook, EVENT_OBJECT_SHOW, window, OBJID_WINDOW, CHILDID_SELF, 0, 0);
                break;
            }
        }
    } else {
        legacy.Apply(taskbar, offset);
    }
}

} // namespace fullwintb::appearance

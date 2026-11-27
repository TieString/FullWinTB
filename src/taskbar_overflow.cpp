#include "geometry.h"

#include <commctrl.h>
#include <oleacc.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include "taskbar_overflow.h"

#include <cmath>
#include <cwchar>
#include <utility>

namespace fullwintb::appearance {
namespace {

namespace xaml = winrt::Windows::UI::Xaml;
constexpr wchar_t modernOverflow[] = L"TopLevelWindowForOverflowXamlIsland";
constexpr wchar_t legacyOverflow[] = L"NotifyIconOverflowWindow";
constexpr UINT_PTR subclassId = 1;
constexpr UINT moveFlags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                          SWP_NOOWNERZORDER | SWP_NOSENDCHANGING;

bool IsOverflowWindow(HWND window) {
    wchar_t name[64]{};
    return GetClassNameW(window, name, ARRAYSIZE(name)) &&
        (wcscmp(name, modernOverflow) == 0 || wcscmp(name, legacyOverflow) == 0);
}

template<class Match>
xaml::FrameworkElement FindElement(const xaml::DependencyObject& node, const Match& matches) {
    if (!node) return nullptr;
    if (const auto element = node.try_as<xaml::FrameworkElement>(); element && matches(element))
        return element;
    const auto count = xaml::Media::VisualTreeHelper::GetChildrenCount(node);
    for (int i = 0; i < count; ++i) {
        if (auto element = FindElement(xaml::Media::VisualTreeHelper::GetChild(node, i), matches)) return element;
    }
    return nullptr;
}

struct VerticalMirror {
    winrt::weak_ref<xaml::FrameworkElement> target;
    xaml::Media::TransformGroup applied{nullptr};
    xaml::Media::Transform original{nullptr};
    xaml::Media::ScaleTransform reflection{nullptr};
    winrt::Windows::Foundation::IInspectable originalLocal{nullptr};

    void Restore() noexcept {
        if (!applied) return;
        try {
            if (const auto element = target.get(); element && element.RenderTransform() == applied) {
                const auto property = xaml::UIElement::RenderTransformProperty();
                try {
                    if (originalLocal == xaml::DependencyProperty::UnsetValue()) element.ClearValue(property);
                    else element.SetValue(property, originalLocal);
                } catch (const winrt::hresult_error&) {
                    element.RenderTransform(original);
                }
            }
        } catch (const winrt::hresult_error&) {
            // The shell can unload its visual during a layout change.
        }
        target = {};
        applied = nullptr;
        original = nullptr;
        reflection = nullptr;
        originalLocal = nullptr;
    }

    bool Apply(const xaml::FrameworkElement& element, double axis) noexcept try {
        const double center = axis - element.ActualHeight() * element.RenderTransformOrigin().Y;
        if (target.get() == element && element.RenderTransform() == applied) {
            if (reflection.CenterY() != center) reflection.CenterY(center);
            return true;
        }
        Restore();
        auto local = element.ReadLocalValue(xaml::UIElement::RenderTransformProperty());
        auto originalTransform = element.RenderTransform();
        xaml::Media::ScaleTransform flip;
        flip.ScaleY(-1);
        flip.CenterY(center);
        xaml::Media::TransformGroup group;
        if (originalTransform) group.Children().Append(originalTransform);
        group.Children().Append(flip);
        auto weak = winrt::make_weak(element);
        element.RenderTransform(group);
        original = std::move(originalTransform);
        originalLocal = std::move(local);
        applied = std::move(group);
        reflection = std::move(flip);
        target = std::move(weak);
        return true;
    } catch (const winrt::hresult_error&) {
        return false;
    }
};

struct Chevron {
    winrt::weak_ref<xaml::FrameworkElement> trayFrame;
    winrt::weak_ref<xaml::FrameworkElement> button;
    winrt::weak_ref<xaml::FrameworkElement> glyph;
    VerticalMirror mirror;

    void Bind(const xaml::FrameworkElement& tray) {
        if (trayFrame.get() == tray) return;
        mirror.Restore();
        button = {};
        glyph = {};
        trayFrame = tray ? winrt::make_weak(tray) : winrt::weak_ref<xaml::FrameworkElement>{};
    }

    xaml::FrameworkElement Button() {
        if (auto known = button.get(); known && known.IsLoaded()) return known;
        const auto tray = trayFrame.get();
        const auto stack = FindElement(tray, [](const auto& element) { return element.Name() == L"NotifyIconStack"; });
        auto chevron = FindElement(stack ? stack : tray, [](const auto& element) {
            return winrt::get_class_name(element) == L"SystemTray.ChevronIconView";
        });
        if (!chevron && stack) {
            chevron = FindElement(stack, [](const auto& element) {
                return winrt::get_class_name(element) == L"SystemTray.NormalButton";
            });
        }
        if (chevron) button = winrt::make_weak(chevron);
        return chevron;
    }

    void SetDirection(bool reverse) noexcept try {
        if (!reverse) {
            mirror.Restore();
            return;
        }
        auto text = glyph.get();
        if (!text || !text.IsLoaded()) {
            mirror.Restore();
            const auto chevron = Button();
            text = FindElement(chevron, [](const auto& element) {
                return winrt::get_class_name(element) == L"SystemTray.TextIconContent";
            });
            if (!text) {
                text = FindElement(chevron, [](const auto& element) { return element.Name() == L"InnerTextBlock"; });
            }
            if (!text) return;
            glyph = winrt::make_weak(text);
        }
        if (text.ActualHeight() <= 0) return;
        mirror.Apply(text, text.ActualHeight() * 0.5);
    } catch (const winrt::hresult_error&) {
        // The next layout can provide a new chevron.
    }

    std::optional<LONG> Center(HWND bar, UINT dpi) noexcept try {
        const auto chevron = Button();
        RECT bounds{};
        if (!chevron || chevron.ActualWidth() <= 0 || !GetWindowRect(bar, &bounds)) return std::nullopt;
        const auto center = chevron.TransformToVisual(nullptr).TransformPoint(
            {static_cast<float>(chevron.ActualWidth() * 0.5), 0});
        return bounds.left + std::lround(center.X * dpi / 96.0);
    } catch (const winrt::hresult_error&) {
        return std::nullopt;
    }
};

struct OverflowAnimation {
    InstanceHandle handle = 0;
    winrt::weak_ref<xaml::FrameworkElement> frame;
    winrt::weak_ref<xaml::FrameworkElement> grid;
    winrt::event_token sized{};
    VerticalMirror outer;
    VerticalMirror inner;
    bool reversed = false;

    void Restore() {
        outer.Restore();
        inner.Restore();
    }

    void Update() noexcept try {
        const auto panel = frame.get();
        const auto content = grid.get();
        if (!reversed || !panel || !content || content.ActualHeight() <= 0) {
            Restore();
            return;
        }
        const double center = content.ActualHeight() * 0.5;
        double offset = 0;
        xaml::DependencyObject node = content;
        while (node && node != panel) {
            offset += node.as<xaml::UIElement>().ActualOffset().y;
            node = xaml::Media::VisualTreeHelper::GetParent(node);
        }
        if (!node) {
            Restore();
            return;
        }
        // Equal reflections leave the resting contents in their original
        // positions and reverse the shell's motion between these two layers.
        if (!inner.Apply(content, center) || !outer.Apply(panel, offset + center)) Restore();
    } catch (const winrt::hresult_error&) {
        Restore();
    }

    void Clear() {
        if (const auto content = grid.get(); content && sized) content.SizeChanged(sized);
        Restore();
        handle = 0;
        frame = {};
        grid = {};
        sized = {};
    }
};

struct Overflow {
    Chevron chevron;
    OverflowAnimation animation;
    HWND bar = nullptr;
    HWND popup = nullptr;
    HWINEVENTHOOK events = nullptr;
    RECT dock{};
    RECT screen{};
    RECT row{};
    UINT dpi = 0;
    LONG panelHeight = 0;
    bool nativeBelow = false;
    bool subclassed = false;
    std::optional<bool> openBelow;

    PopupPlacement Placement(LONG height) const {
        return PlacePopup(row, screen, height, MulDiv(8, dpi, 96), openBelow.value_or(nativeBelow));
    }

    void SetDirection(bool reverse) {
        chevron.SetDirection(reverse);
        if (animation.reversed != reverse) {
            animation.reversed = reverse;
            animation.Update();
        }
    }

    void RefreshDirection() {
        if (bar) SetDirection(Placement(panelHeight).below != nativeBelow);
    }

    POINT Position(LONG left, LONG width, LONG height, bool opening) {
        panelHeight = height;
        const auto placement = Placement(height);
        if (opening || IsWindowVisible(popup)) openBelow = placement.below;
        SetDirection(placement.below != nativeBelow);
        if (const auto center = chevron.Center(bar, dpi)) left = *center - width / 2;
        return {std::clamp(left, screen.left, std::max(screen.left, screen.right - width)), placement.top};
    }

    void Move(bool evenIfHidden = false) {
        RECT rectangle{};
        const bool visible = popup && IsWindowVisible(popup);
        if (!visible) openBelow.reset();
        if (!popup || (!visible && !evenIfHidden) || !GetWindowRect(popup, &rectangle) || IsRectEmpty(&rectangle))
            return;
        const auto target = Position(rectangle.left, rectangle.right - rectangle.left,
                                     rectangle.bottom - rectangle.top, false);
        if (target.x == rectangle.left && target.y == rectangle.top) return;
        SetWindowPos(popup, nullptr, target.x, target.y, 0, 0,
                     moveFlags | (subclassed ? 0 : SWP_ASYNCWINDOWPOS));
    }

    void Detach() {
        if (subclassed) RemoveWindowSubclass(popup, WindowProc, subclassId);
        subclassed = false;
        popup = nullptr;
        openBelow.reset();
    }

    void Attach(HWND window) {
        if (popup == window) return;
        Detach();
        popup = window;
        if (!popup) return;
        if (GetWindowThreadProcessId(popup, nullptr) == GetCurrentThreadId())
            subclassed = SetWindowSubclass(popup, WindowProc, subclassId, reinterpret_cast<DWORD_PTR>(this)) != FALSE;
        RECT rectangle{};
        if (GetWindowRect(popup, &rectangle) && !IsRectEmpty(&rectangle))
            panelHeight = rectangle.bottom - rectangle.top;
    }

    void Reset() {
        if (events) UnhookWinEvent(std::exchange(events, nullptr));
        if (bar) {
            row = dock;
            openBelow.reset();
            Move(true);
        }
        Detach();
        SetDirection(false);
        bar = nullptr;
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                       UINT_PTR, DWORD_PTR data) {
        auto& state = *reinterpret_cast<Overflow*>(data);
        if (message == WM_NCDESTROY) {
            state.Detach();
            return DefSubclassProc(window, message, wp, lp);
        }
        const auto result = DefSubclassProc(window, message, wp, lp);
        if (message != WM_WINDOWPOSCHANGING || !lp || window != state.popup) return result;
        auto& change = *reinterpret_cast<WINDOWPOS*>(lp);
        if (change.flags & SWP_HIDEWINDOW) {
            state.openBelow.reset();
            return result;
        }
        constexpr UINT noGeometry = SWP_NOMOVE | SWP_NOSIZE;
        if ((change.flags & noGeometry) == noGeometry && !(change.flags & SWP_SHOWWINDOW)) return result;
        RECT existing{};
        if (!GetWindowRect(window, &existing)) return result;
        const LONG width = change.flags & SWP_NOSIZE ? existing.right - existing.left : change.cx;
        const LONG height = change.flags & SWP_NOSIZE ? existing.bottom - existing.top : change.cy;
        if (width <= 0 || height <= 0) return result;
        const LONG left = change.flags & SWP_NOMOVE ? existing.left : change.x;
        const auto target = state.Position(left, width, height, (change.flags & SWP_SHOWWINDOW) != 0);
        change.x = target.x;
        change.y = target.y;
        change.flags &= ~SWP_NOMOVE;
        return result;
    }
};

Overflow overflow;

void CALLBACK OnPopupShown(HWINEVENTHOOK hook, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD) {
    if (hook != overflow.events || !overflow.bar || object != OBJID_WINDOW ||
        child != CHILDID_SELF || !IsOverflowWindow(window)) return;
    overflow.Attach(window);
    overflow.Move();
    overflow.RefreshDirection();
}

HWND FindOverflow() {
    for (const auto name : {modernOverflow, legacyOverflow}) {
        for (HWND window = nullptr; (window = FindWindowExW(nullptr, window, name, nullptr));) {
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            if (process == GetCurrentProcessId()) return window;
        }
    }
    return nullptr;
}

} // namespace

std::optional<RECT> TrayChevronBounds(HWND bar, UINT dpi, const xaml::FrameworkElement& tray) try {
    overflow.chevron.Bind(tray);
    const auto chevron = overflow.chevron.Button();
    RECT window{};
    if (!chevron || chevron.Visibility() != xaml::Visibility::Visible || !GetWindowRect(bar, &window))
        return std::nullopt;
    const float width = static_cast<float>(chevron.ActualWidth());
    const float height = static_cast<float>(chevron.ActualHeight());
    if (width <= 0 || height <= 0) return std::nullopt;

    const auto displayed = chevron.TransformToVisual(nullptr).TransformBounds({0, 0, width, height});
    const double scale = dpi / 96.0;
    const RECT bounds{window.left + std::lround(displayed.X * scale),
                      window.top + std::lround(displayed.Y * scale),
                      window.left + std::lround((displayed.X + displayed.Width) * scale),
                      window.top + std::lround((displayed.Y + displayed.Height) * scale)};
    if (IsRectEmpty(&bounds) || !Overlaps(bounds, window)) return std::nullopt;
    return bounds;
} catch (const winrt::hresult_error&) {
    return std::nullopt;
}

void SetTrayOverflowGrid(InstanceHandle handle, const xaml::FrameworkElement& grid) {
    auto& animation = overflow.animation;
    if (animation.handle == handle) return;
    auto parent = xaml::Media::VisualTreeHelper::GetParent(grid);
    while (parent && winrt::get_class_name(parent) != L"SystemTray.NotificationAreaOverflow")
        parent = xaml::Media::VisualTreeHelper::GetParent(parent);
    const auto frame = parent.try_as<xaml::FrameworkElement>();
    if (!frame) return;
    animation.Clear();
    animation.handle = handle;
    animation.frame = winrt::make_weak(frame);
    animation.grid = winrt::make_weak(grid);
    animation.sized = grid.SizeChanged([](const auto&, const auto&) { overflow.animation.Update(); });
    animation.Update();
}

void RemoveTrayOverflowGrid(InstanceHandle handle) {
    if (overflow.animation.handle == handle) overflow.animation.Clear();
}

void FollowTrayOverflow(HWND bar, const RECT& dock, LONG x, LONG y, const xaml::FrameworkElement& tray) {
    if (!bar || (!x && !y)) {
        if (overflow.bar) overflow.Reset();
        return;
    }
    if (overflow.bar != bar || !SameRect(dock, overflow.dock)) {
        overflow.Reset();
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(bar, MONITOR_DEFAULTTONULL), &monitor)) return;
        overflow.bar = bar;
        overflow.dock = dock;
        overflow.screen = monitor.rcMonitor;
        overflow.dpi = GetDpiForWindow(bar);
        overflow.nativeBelow = Near(dock.top, monitor.rcMonitor.top);
        overflow.panelHeight = MulDiv(200, overflow.dpi, 96);
        overflow.Attach(FindOverflow());
        overflow.events = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnPopupShown,
                                          GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    }
    overflow.chevron.Bind(tray);
    overflow.row = dock;
    OffsetRect(&overflow.row, x, y);
    overflow.Move();
    overflow.RefreshDirection();
}

} // namespace fullwintb::appearance

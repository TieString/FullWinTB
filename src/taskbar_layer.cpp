#include "taskbar_layer.h"
#include "geometry.h"

#include <commctrl.h>
#include <utility>

namespace fullwintb::appearance {
namespace {

constexpr UINT_PTR subclassId = 1;
constexpr UINT positionFlags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
constexpr wchar_t attachedProperty[] = L"FullWinTB.LayerAttached";
constexpr wchar_t demotionsProperty[] = L"FullWinTB.LayerDemotions";
constexpr wchar_t modeProperty[] = L"FullWinTB.LayerMode";
thread_local HWND regionWriteWindow = nullptr;
thread_local bool animatedRegion = false;

bool Topmost(HWND window) {
    return (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

bool LowersBand(HWND after) {
    return after == HWND_NOTOPMOST || after == HWND_BOTTOM || (IsWindow(after) && !Topmost(after));
}

} // namespace

bool WriteTaskbarRegion(HWND bar, HRGN region, bool animated) {
    const HWND previous = std::exchange(regionWriteWindow, bar);
    const bool previousAnimation = std::exchange(animatedRegion, animated);
    // A moving XAML island supplies its own frames. Forcing a Win32 repaint at
    // every clip update also asks Explorer to erase the newly exposed pixels.
    const bool applied = SetWindowRgn(bar, region, animated ? FALSE : TRUE) != 0;
    animatedRegion = previousAnimation;
    regionWriteWindow = previous;
    return applied;
}

void TaskbarLayer::Update(HWND bar, LayerMode mode, HWND orderObserver) {
    if (bar_ != bar) {
        Detach();
        if (!bar || !SetWindowSubclass(bar, WindowProc, subclassId, reinterpret_cast<DWORD_PTR>(this))) return;
        bar_ = bar;
        wasTopmost_ = Topmost(bar);
        SetPropW(bar, attachedProperty, reinterpret_cast<HANDLE>(1));
    }
    if (!bar_) return;
    orderObserver_ = orderObserver;
    if (mode == mode_) return;
    mode_ = mode;
    SetPropW(bar_, modeProperty, reinterpret_cast<HANDLE>(1 + static_cast<ULONG_PTR>(mode_)));
    ApplyPosition();
}

void TaskbarLayer::HoldPresentation(bool held) {
    if (presentationHeld_ == held) return;
    presentationHeld_ = held;
    ApplyPosition();
}

void TaskbarLayer::Detach() {
    CloseCanvas();
    const HWND bar = std::exchange(bar_, nullptr);
    orderObserver_ = nullptr;
    if (!bar) return;
    RemoveWindowSubclass(bar, WindowProc, subclassId);
    RemovePropW(bar, attachedProperty);
    RemovePropW(bar, demotionsProperty);
    RemovePropW(bar, modeProperty);
    const bool lower = !wasTopmost_ && Topmost(bar);
    if (hidBar_ || lower) {
        const UINT flags = positionFlags | (lower ? 0 : SWP_NOZORDER) | (hidBar_ ? SWP_SHOWWINDOW : 0);
        SetWindowPos(bar, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    }
    hidBar_ = false;
    mode_ = LayerMode::native;
}

bool TaskbarLayer::EnsureCanvas(LONG travel, MotionAxis axis, bool towardEnd) {
    if (!bar_ || mode_ != LayerMode::floating || travel <= 0) return false;
    const bool growing = bridge_ != nullptr;
    HWND bridge = bridge_;
    if (growing) {
        if (!CanvasIntact()) return false;
    } else {
        if (!GetWindowRect(bar_, &dock_)) return false;
        bridge = FindWindowExW(bar_, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr);
        RECT actual{};
        if (!bridge || !GetWindowRect(bridge, &actual) || !SameRect(actual, dock_)) return false;
    }
    const HMONITOR monitor = MonitorFromWindow(bar_, MONITOR_DEFAULTTONULL);
    MONITORINFO before{sizeof(before)}, after{sizeof(after)};
    if (!monitor || !GetMonitorInfoW(monitor, &before)) return false;
    const RECT previous = growing ? viewport_ : dock_;
    const LONG thickness = AxisSize(dock_, axis);
    if (!growing && !SetPropW(bar_, DockThicknessProperty(axis),
                             reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(towardEnd ? -thickness : thickness))))
        return false;
    bridge_ = bridge;
    viewport_ = dock_;
    if (towardEnd) AxisEnd(viewport_, axis) += travel;
    else AxisStart(viewport_, axis) -= travel;
    if (SizeCanvas(viewport_) && CanvasIntact() && GetMonitorInfoW(monitor, &after) &&
        SameRect(before.rcWork, after.rcWork)) return true;
    if (growing) {
        RECT actual{};
        if (GetWindowRect(bar_, &actual) && (SameRect(actual, viewport_) || SameRect(actual, previous))) {
            const bool restored = SizeCanvas(previous);
            if (GetWindowRect(bar_, &actual) && SameRect(actual, previous)) viewport_ = previous;
            if (restored && CanvasIntact()) return false;
        }
    }
    CloseCanvas();
    return false;
}

bool TaskbarLayer::SizeCanvas(const RECT& rectangle) {
    constexpr UINT flags = SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING |
                           SWP_NOCOPYBITS | SWP_DEFERERASE;
    const int width = rectangle.right - rectangle.left;
    const int height = rectangle.bottom - rectangle.top;
    // Both size changes run in the same dispatch with the island still visible.
    // Hiding it here removes the entire row from the compositor for a frame.
    sizingCanvas_ = true;
    const bool resized = SetWindowPos(bar_, nullptr, rectangle.left, rectangle.top, width, height, flags) &&
        SetWindowPos(bridge_, nullptr, 0, 0, width, height, flags);
    sizingCanvas_ = false;
    if (resized) RedrawWindow(bridge_, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_NOCHILDREN);
    return resized;
}

bool TaskbarLayer::CanvasIntact() const {
    RECT bar{}, child{};
    return bridge_ && GetWindowRect(bar_, &bar) && GetWindowRect(bridge_, &child) &&
        SameRect(bar, viewport_) && SameRect(child, viewport_);
}

void TaskbarLayer::CloseCanvas() {
    if (!bridge_) return;
    RECT actual{};
    if (GetWindowRect(bar_, &actual)) {
        if (SameRect(actual, viewport_)) {
            SizeCanvas(dock_);
        } else if (SameRect(actual, dock_) && IsWindow(bridge_)) {
            SetWindowPos(bridge_, nullptr, 0, 0, dock_.right - dock_.left, dock_.bottom - dock_.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        }
    }
    RemovePropW(bar_, dockWidthProperty);
    RemovePropW(bar_, dockHeightProperty);
    bridge_ = nullptr;
}

void TaskbarLayer::ApplyPosition() {
    if (!bar_ || updating_) return;
    const bool blocked = mode_ == LayerMode::covered || presentationHeld_;
    const bool hide = blocked && IsWindowVisible(bar_);
    const bool show = !blocked && hidBar_;
    const bool raise = mode_ == LayerMode::floating && !Topmost(bar_);
    if (!hide && !show && !raise) return;

    UINT flags = positionFlags | (raise ? 0 : SWP_NOZORDER);
    if (hide) flags |= SWP_HIDEWINDOW;
    if (show) flags |= SWP_SHOWWINDOW;
    updating_ = true;
    if (SetWindowPos(bar_, HWND_TOPMOST, 0, 0, 0, 0, flags)) hidBar_ = hide;
    updating_ = false;
}

LRESULT CALLBACK TaskbarLayer::WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                         UINT_PTR id, DWORD_PTR context) {
    auto& layer = *reinterpret_cast<TaskbarLayer*>(context);
    // The composition island paints the content over the enlarged viewport.
    // The legacy parent brush would fill gaps in that viewport with solid color.
    if (message == WM_ERASEBKGND && layer.bridge_ && layer.mode_ != LayerMode::native) return TRUE;
    // Resize the composition surface without telling Explorer to redock the taskbar.
    if (message == WM_WINDOWPOSCHANGED &&
        (layer.sizingCanvas_ || (regionWriteWindow == window && (animatedRegion || layer.bridge_)))) return 0;
    if (message == WM_NCDESTROY) {
        layer.bar_ = nullptr;
        layer.orderObserver_ = nullptr;
        layer.bridge_ = nullptr;
        layer.mode_ = LayerMode::native;
        layer.hidBar_ = false;
        RemoveWindowSubclass(window, WindowProc, id);
        RemovePropW(window, attachedProperty);
        RemovePropW(window, demotionsProperty);
        RemovePropW(window, modeProperty);
        RemovePropW(window, dockWidthProperty);
        RemovePropW(window, dockHeightProperty);
    }
    const LRESULT result = DefSubclassProc(window, message, wp, lp);
    if (layer.bar_ == window && message == WM_WINDOWPOSCHANGING && lp) {
        auto& change = *reinterpret_cast<WINDOWPOS*>(lp);
        if (layer.bridge_ && ((change.flags & SWP_NOMOVE) ||
            (change.x == layer.dock_.left && change.y == layer.dock_.top)) &&
            ((change.flags & SWP_NOSIZE) || (change.cx == layer.dock_.right - layer.dock_.left &&
                                            change.cy == layer.dock_.bottom - layer.dock_.top)))
            change.flags |= SWP_NOMOVE | SWP_NOSIZE;
        if (layer.mode_ == LayerMode::covered || layer.presentationHeld_) {
            if (layer.presentationHeld_ && (change.flags & SWP_SHOWWINDOW)) layer.hidBar_ = true;
            change.flags &= ~SWP_SHOWWINDOW;
        } else if (layer.mode_ == LayerMode::floating && !(change.flags & SWP_NOZORDER) &&
                   LowersBand(change.hwndInsertAfter)) {
            // Preserve the current position inside the topmost band. Menus can
            // still be placed above the bar and visibility requests pass through.
            change.flags |= SWP_NOZORDER;
            const auto count = reinterpret_cast<ULONG_PTR>(GetPropW(window, demotionsProperty));
            SetPropW(window, demotionsProperty, reinterpret_cast<HANDLE>(count + 1));
        }
    } else if (layer.bar_ == window && message == WM_WINDOWPOSCHANGED) {
        layer.ApplyPosition();
        if (layer.mode_ == LayerMode::floating && layer.orderObserver_ && lp) {
            const UINT flags = reinterpret_cast<const WINDOWPOS*>(lp)->flags;
            if (!(flags & SWP_NOZORDER) || (flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW)))
                PostMessageW(layer.orderObserver_, taskbarOrderChanged, reinterpret_cast<WPARAM>(window), 0);
        }
    }
    return result;
}

} // namespace fullwintb::appearance

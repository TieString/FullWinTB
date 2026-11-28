#include "overlay.h"
#include "appearance.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <cwchar>
#include <utility>

namespace fullwintb {
namespace {

// Out-of-context WinEvents arrive on the thread that registered them.
Overlay* activeOverlay = nullptr;

constexpr UINT resizeFlags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                             SWP_ASYNCWINDOWPOS;
constexpr UINT orderFlags = SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOMOVE |
                            SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
constexpr UINT inkFrameDelay = 20;
constexpr UINT inkBurstDuration = 160;
constexpr UINT inkIdleDelay = 250;

bool Topmost(HWND window) {
    return (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

bool VisibleTopLevel(HWND window) {
    if (!window || !IsWindowVisible(window) || IsIconic(window) ||
        GetAncestor(window, GA_ROOT) != window) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return !cloaked;
}

bool InMenu(DWORD thread) {
    GUITHREADINFO info{sizeof(GUITHREADINFO)};
    return thread && GetGUIThreadInfo(thread, &info) &&
        (info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE));
}

bool ReadFrame(HWND window, RECT& rect) {
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                            &rect, sizeof(rect))) &&
           rect.left < rect.right && rect.top < rect.bottom;
}

struct WindowSurface {
    RECT frame;
    RECT client;
    DWORD style;
    bool optsOutOfFullscreen;

    WindowMode Mode(const RECT& monitor) const {
        return ClassifyWindow(frame, client, monitor, style, optsOutOfFullscreen);
    }
};

std::optional<WindowSurface> ReadSurface(HWND window) {
    WINDOWINFO info{sizeof(WINDOWINFO)};
    if (!VisibleTopLevel(window) || window == GetShellWindow() || !GetWindowInfo(window, &info))
        return std::nullopt;
    RECT frame{};
    if (!ReadFrame(window, frame)) frame = info.rcWindow;
    return WindowSurface{frame, info.rcClient, info.dwStyle, GetPropW(window, L"NonRudeHWND") != nullptr};
}

bool Above(HWND above, HWND below) {
    HWND window = below;
    for (int i = 0; i < 512 && (window = GetWindow(window, GW_HWNDPREV)); ++i) {
        if (window == above) return true;
    }
    return false;
}

bool CaptureProcess(HWND window, bool clippingSurface) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH]{};
    DWORD size = ARRAYSIZE(path);
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    if (!ok) return false;

    const wchar_t* separator = wcsrchr(path, L'\\');
    const wchar_t* name = separator ? separator + 1 : path;
    if (clippingSurface && _wcsicmp(name, L"SnippingTool.exe") == 0) return true;
    constexpr const wchar_t* clients[]{
        L"ScreenClippingHost.exe", L"ScreenSketch.exe",
        L"PixPin.exe", L"Snipaste.exe", L"ShareX.exe", L"QQScreenShot.exe",
        L"QQ.exe", L"QQNT.exe", L"Weixin.exe", L"WeChat.exe",
    };
    for (const auto* client : clients)
        if (_wcsicmp(name, client) == 0) return true;
    return false;
}

bool ScreenOverlay(HWND window, HWND foreground, const RECT& monitor, DWORD explorer) {
    // Do the cheap style/geometry checks before asking DWM or opening a process.
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    if (!(style & WS_VISIBLE) || (style & (WS_CHILD | WS_MINIMIZE | WS_MAXIMIZE))) return false;
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (!(extended & WS_EX_TOPMOST) && window != foreground) return false;
    RECT bounds{};
    if (!GetWindowRect(window, &bounds) || !Covers(bounds, monitor) || !VisibleTopLevel(window))
        return false;

    wchar_t name[64]{};
    GetClassNameW(window, name, ARRAYSIZE(name));
    if (wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0) return false;
    if (wcscmp(name, L"SnipOverlayRootWindow") == 0 || wcscmp(name, L"ScreenClippingHost") == 0)
        return true;

    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    // Full-size hosts also exist for previews and menus.
    if (process == explorer) return false;
    // Windows can leave the snapshot in a tool surface while the root is cloaked.
    const bool clippingSurface = (extended & WS_EX_TOOLWINDOW) && wcscmp(name, L"XamlWindow") == 0;
    return CaptureProcess(window, clippingSurface);
}

HWND FindScreenOverlay(HWND foreground, const RECT& monitor, DWORD explorer) {
    HWND window = GetTopWindow(nullptr);
    for (int i = 0; window && i < 512; ++i, window = GetWindow(window, GW_HWNDNEXT)) {
        if (ScreenOverlay(window, foreground, monitor, explorer)) return window;
    }
    return nullptr;
}

struct ResizeRequest {
    HMONITOR monitor;
    const RECT& screen;
    const RECT& from;
    const RECT& to;
};

bool ResizeMaximized(HWND window, const ResizeRequest& request) {
    if (!IsZoomed(window) || !VisibleTopLevel(window) || GetWindow(window, GW_OWNER) ||
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) || IsHungAppWindow(window) ||
        MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != request.monitor) return false;

    WINDOWINFO info{sizeof(WINDOWINFO)};
    if (!GetWindowInfo(window, &info)) return false;
    if (ClassifyWindow(info.rcWindow, info.rcClient, request.screen, info.dwStyle,
                       GetPropW(window, L"NonRudeHWND") != nullptr) != WindowMode::maximized) return false;
    const SIZE borders{static_cast<LONG>(info.cxWindowBorders), static_cast<LONG>(info.cyWindowBorders)};
    if (!Fits(info.rcWindow, request.from, borders) || Fits(info.rcWindow, request.to, borders))
        return false;

    // Recheck maximize before posting the resize. Preserve the application's
    // show state, restore rectangle, z-order and activation.
    const RECT target = TransferBounds(info.rcWindow, request.from, request.to);
    return IsZoomed(window) && !IsIconic(window) && SetWindowPos(window, nullptr, target.left, target.top,
        target.right - target.left, target.bottom - target.top, resizeFlags);
}

void ResizeMonitor(HMONITOR monitor, const RECT& screen, const RECT& from, const RECT& to, HWND except = nullptr) {
    struct MonitorRequest { ResizeRequest resize; HWND except; };
    const MonitorRequest request{{monitor, screen, from, to}, except};
    EnumWindows([](HWND window, LPARAM context) -> BOOL {
        const auto& request = *reinterpret_cast<const MonitorRequest*>(context);
        if (window != request.except) ResizeMaximized(window, request.resize);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&request));
}

} // namespace

Overlay::WindowId::WindowId(HWND window) : hwnd(window) {
    if (hwnd) thread = GetWindowThreadProcessId(hwnd, &process);
}

bool Overlay::WindowId::Alive() const {
    return thread && *this == WindowId(hwnd);
}

bool Overlay::Start(HWND host) {
    if (Enabled()) return true;
    host_ = host;
    activeOverlay = this;
    constexpr DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    hooks_[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                               nullptr, WinEvent, 0, 0, flags);
    hooks_[1] = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                               nullptr, WinEvent, 0, 0, flags);
    hooks_[2] = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE,
                               nullptr, WinEvent, 0, 0, flags);
    hooks_[3] = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED,
                               nullptr, WinEvent, 0, 0, flags);
    hooks_[4] = SetWinEventHook(EVENT_SYSTEM_MENUSTART, EVENT_SYSTEM_MENUPOPUPEND,
                               nullptr, WinEvent, 0, 0, flags);
    hooks_[5] = SetWinEventHook(EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_STATECHANGE,
                               nullptr, WinEvent, 0, 0, flags);
    if (std::any_of(hooks_.begin(), hooks_.end(), [](auto hook) { return !hook; })) {
        Stop();
        return false;
    }
    autoHide_ = WorkArea::TaskbarAutoHides();
    ShellRestarted();
    return true;
}

void Overlay::Stop() {
    if (!Enabled()) return;
    CancelInk();
    const HWND host = std::exchange(host_, nullptr);
    activeOverlay = nullptr;
    appearance::Update(host, false, appearance::Ink::system);
    for (auto& hook : hooks_) {
        if (hook) UnhookWinEvent(hook);
        hook = nullptr;
    }
    KillTimer(host, updateTimer);
    KillTimer(host, settleTimer);
    KillTimer(host, resumeTimer);
    autoHide_ = WorkArea::TaskbarAutoHides();
    RestoreWorkAreas();
    if (appBarRegistered_) {
        APPBARDATA request{sizeof(APPBARDATA)};
        request.hWnd = host;
        SHAppBarMessage(ABM_REMOVE, &request);
    }
    appBarRegistered_ = false;
    RestoreOrder();
    bars_.clear();
    queuedDelay_ = 0;
    topologyDirty_ = false;
    taskbarLayoutDirty_ = false;
    retryWorkAreas_ = false;
    resumeQueued_ = false;
    primaryLayer_ = appearance::LayerMode::native;
}

void Overlay::ReadTaskbars() {
    const WindowId shell(GetShellWindow());
    const bool previousAutoHide = autoHide_;
    autoHide_ = WorkArea::TaskbarAutoHides();
    const bool retryClaims = std::exchange(retryWorkAreas_, false) || previousAutoHide != autoHide_;
    const HWND foreground = GetForegroundWindow();
    if (previousAutoHide != autoHide_) {
        if (autoHide_) {
            CancelInk();
            SetPrimaryLayer(appearance::LayerMode::native);
        } else {
            WakeInk();
        }
    }
    if (autoHide_) {
        for (auto& bar : bars_) RestoreAfterCover(bar);
        RestoreWorkAreas();
    }

    std::vector<Taskbar> current;
    const auto add = [&](HWND hwnd, bool primary) {
        if (!hwnd || !shell.process) return;
        const WindowId window(hwnd);
        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
        MONITORINFOEXW display{};
        RECT bounds{};
        if (window.process != shell.process || !MonitorInfo(monitor, display) ||
            !appearance::ReadDockBounds(hwnd, bounds)) return;
        const auto edge = TaskbarEdge(bounds, display.rcMonitor);
        if (!edge) return;

        Taskbar bar{window, monitor, bounds, display, *edge, primary};
        for (const auto& previous : bars_) {
            if (previous.monitor == monitor && SameRect(previous.work.monitor, display.rcMonitor) &&
                SameRect(previous.work.expanded, display.rcWork) && !autoHide_) {
                bar.work = previous.work;
            }
            if (previous.window == window) {
                if (!retryClaims && SameRect(previous.bounds, bounds) && SameRect(previous.display.rcMonitor, display.rcMonitor)) {
                    bar.requestedWorkArea = previous.requestedWorkArea;
                    bar.resizeAttempt = previous.resizeAttempt;
                }
                bar.raisedOver = previous.raisedOver;
                bar.cover = previous.cover;
                bar.lastMaximized = previous.lastMaximized;
                bar.wasTopmost = previous.wasTopmost;
                bar.wasVisible = previous.wasVisible;
                bar.hiddenByHost = previous.hiddenByHost;
                bar.topmostBeforeCover = previous.topmostBeforeCover;
                bar.lastOrder = previous.lastOrder;
                bar.lastOrderAt = previous.lastOrderAt;
                bar.lastOrderFlags = previous.lastOrderFlags;
                if (!SameRect(previous.bounds, bounds)) taskbarLayoutDirty_ = true;
            }
        }
        if (!autoHide_ && !bar.work.Owned() && !bar.requestedWorkArea) {
            const RECT desired = ExpandedWork(display.rcMonitor, display.rcWork, bounds, *edge);
            // Each dock layout can request the work area once. Explorer may
            // put its reservation back and broadcast another position change.
            bar.requestedWorkArea = !SameRect(desired, display.rcWork);
            if (bar.requestedWorkArea && bar.work.Acquire(display, desired)) {
                MONITORINFOEXW applied{};
                if (MonitorInfo(monitor, applied) && SameRect(applied.rcWork, bar.work.expanded))
                    ResizeMonitor(monitor, display.rcMonitor, bar.work.original, bar.work.expanded, foreground);
            }
        }
        current.push_back(bar);
    };
    add(FindWindowW(L"Shell_TrayWnd", nullptr), true);
    if (shell.process) {
        for (HWND bar = nullptr;
             (bar = FindWindowExW(nullptr, bar, L"Shell_SecondaryTrayWnd", nullptr));) add(bar, false);
    }
    for (const auto& previous : bars_) {
        if (!previous.work.Owned() ||
            std::any_of(current.begin(), current.end(), [&](const Taskbar& bar) {
                return bar.monitor == previous.monitor;
            })) continue;
        MONITORINFOEXW display{};
        if (MonitorInfo(previous.monitor, display) && previous.work.Restore(display, autoHide_))
            ResizeMonitor(previous.monitor, display.rcMonitor, previous.work.expanded, previous.work.original);
    }
    bars_.swap(current);
    if (primaryLayer_ == appearance::LayerMode::suspended &&
        (autoHide_ || std::none_of(bars_.begin(), bars_.end(), [](const Taskbar& bar) { return bar.primary; })))
        SetPrimaryLayer(appearance::LayerMode::native);
}

void Overlay::Refresh(bool retryWorkAreas) {
    if (!Enabled()) return;
    retryWorkAreas_ |= retryWorkAreas;
    topologyDirty_ = true;
    taskbarLayoutDirty_ = true;
    if (!session_.Active()) {
        QueueResume();
        return;
    }
    Queue();
    SetTimer(host_, settleTimer, 260, nullptr);
    WakeInk();
}

void Overlay::ShellRestarted() {
    if (!Enabled()) return;
    APPBARDATA request{sizeof(APPBARDATA)};
    request.hWnd = host_;
    request.uCallbackMessage = appBarMessage;
    if (SHAppBarMessage(ABM_NEW, &request)) appBarRegistered_ = true;
    if (primaryLayer_ != appearance::LayerMode::suspended) {
        primaryLayer_ = appearance::LayerMode::native;
        UpdateAppearance();
    }
    Refresh(true);
}

void Overlay::Queue(UINT delay) {
    if (session_.Active() && (!queuedDelay_ || delay < queuedDelay_) && SetTimer(host_, updateTimer, delay, nullptr))
        queuedDelay_ = delay;
}

void Overlay::SessionLocked() {
    if (!session_.Lock() || !Enabled()) return;
    KillTimer(host_, updateTimer);
    KillTimer(host_, settleTimer);
    KillTimer(host_, resumeTimer);
    resumeQueued_ = false;
    queuedDelay_ = 0;
    CancelInk();
    primaryLayer_ = appearance::LayerMode::suspended;
    appearance::Update(host_, true, appearance::Ink::system, appearance::LayerMode::suspended);
    for (auto& bar : bars_) RestoreAfterCover(bar);
}

void Overlay::SessionUnlocked() {
    if (session_.Unlock(GetTickCount64())) QueueResume();
}

void Overlay::QueueResume() {
    if (!Enabled() || !session_.Settling()) return;
    const ULONGLONG now = GetTickCount64();
    session_.ObserveChange(now);
    if (!resumeQueued_)
        resumeQueued_ = SetTimer(host_, resumeTimer, std::max<UINT>(USER_TIMER_MINIMUM, session_.Delay(now)), nullptr) != 0;
}

void Overlay::SetInk(appearance::Ink ink) {
    if (ink == ink_) return;
    ink_ = ink;
    autoInk_ = {};
    autoAppsInk_ = {};
    if (Enabled()) {
        CancelInk();
        UpdateAppearance();
        WakeInk();
    }
}

void Overlay::UpdateAppearance() {
    if (!Enabled() || !session_.Active() || primaryLayer_ == appearance::LayerMode::suspended) return;
    const bool automatic = ink_ == appearance::Ink::automatic;
    appearance::Update(host_, true, automatic ? autoInk_.Current() : ink_, primaryLayer_,
                       automatic ? autoAppsInk_.Current() : appearance::Ink::system);
}

void Overlay::SetPrimaryLayer(appearance::LayerMode mode) {
    if (primaryLayer_ == mode) return;
    primaryLayer_ = mode;
    UpdateAppearance();
    if (mode == appearance::LayerMode::covered) CancelInk();
    else if (mode == appearance::LayerMode::floating) WakeInk();
}

void Overlay::WakeInk() {
    if (!session_.Active() || ink_ != appearance::Ink::automatic || autoHide_ ||
        primaryLayer_ == appearance::LayerMode::covered) return;
    autoInk_.Pause();
    autoAppsInk_.Pause();
    inkBurstUntil_ = GetTickCount64() + inkBurstDuration;
    QueueInk(inkFrameDelay);
}

void Overlay::CancelInk() {
    KillTimer(host_, inkTimer);
    inkDue_ = 0;
    inkBurstUntil_ = 0;
    autoInk_.Pause();
    autoAppsInk_.Pause();
}

void Overlay::QueueInk(UINT delay) {
    const ULONGLONG due = GetTickCount64() + delay;
    if ((!inkDue_ || due < inkDue_) && SetTimer(host_, inkTimer, delay, nullptr)) inkDue_ = due;
}

void Overlay::SampleInk() {
    if (ink_ != appearance::Ink::automatic || autoHide_ || primaryLayer_ == appearance::LayerMode::covered) return;
    const HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
    const auto bar = std::find_if(bars_.begin(), bars_.end(), [primary](const Taskbar& item) {
        return item.window.hwnd == primary;
    });
    const HWND foreground = GetForegroundWindow();
    const auto foregroundStyle = GetWindowLongPtrW(foreground, GWL_EXSTYLE);
    const bool standardSurface = VisibleTopLevel(foreground) &&
        (foreground == GetShellWindow() || !(foregroundStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)));
    appearance::TaskbarInkSample ink;
    if (standardSurface && bar != bars_.end() && bar->window.Alive() && !bar->cover.hwnd && IsWindowVisible(primary) &&
        !InMenu(bar->window.thread) && !InMenu(GetWindowThreadProcessId(foreground, nullptr))) {
        const bool horizontal = bar->edge == Edge::top || bar->edge == Edge::bottom;
        ink = appearance::ReadTaskbarInk(primary, bar->display.rcMonitor, horizontal);
    }
    const ULONGLONG now = GetTickCount64();
    const bool notificationChanged = autoInk_.Observe(ink.notification, now);
    const bool appsChanged = autoAppsInk_.Observe(ink.apps, now);
    if (notificationChanged || appsChanged) UpdateAppearance();
    // The first reading can still be from the previous compositor frame.
    const bool settling = autoInk_.Settling() || autoAppsInk_.Settling() || now < inkBurstUntil_;
    QueueInk(settling ? inkFrameDelay : inkIdleDelay);
}

void CALLBACK Overlay::WinEvent(HWINEVENTHOOK, DWORD event, HWND window,
                               LONG object, LONG child, DWORD, DWORD) {
    const bool menu = event >= EVENT_SYSTEM_MENUSTART && event <= EVENT_SYSTEM_MENUPOPUPEND;
    if (activeOverlay && (menu || (window && object == OBJID_WINDOW && child == CHILDID_SELF)))
        activeOverlay->OnEvent(event, window);
}

void Overlay::OnEvent(DWORD event, HWND window) {
    if (!session_.Active()) return;
    if ((event == EVENT_OBJECT_LOCATIONCHANGE || event == EVENT_OBJECT_STATECHANGE) && !IsZoomed(window)) {
        for (auto& bar : bars_)
            if (bar.resizeAttempt.hwnd == window) bar.resizeAttempt = WindowId(nullptr);
    }
    if (event >= EVENT_SYSTEM_MENUSTART && event <= EVENT_SYSTEM_MENUPOPUPEND) {
        Queue();
        WakeInk();
        return;
    }
    if (event == EVENT_SYSTEM_FOREGROUND) {
        Queue(USER_TIMER_MINIMUM);
        SetTimer(host_, settleTimer, 260, nullptr);
        WakeInk();
        return;
    }
    const HWND foreground = GetForegroundWindow();
    if (event == EVENT_OBJECT_STATECHANGE) {
        if (window == foreground || window == GetAncestor(foreground, GA_ROOTOWNER) ||
            std::any_of(bars_.begin(), bars_.end(), [window](const Taskbar& bar) {
                return bar.cover.hwnd == window;
            })) Queue(USER_TIMER_MINIMUM);
        return;
    }
    const bool covering = std::any_of(bars_.begin(), bars_.end(), [&](const Taskbar& item) {
        return item.cover.hwnd == window ||
               ((event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_LOCATIONCHANGE ||
                 event == EVENT_OBJECT_UNCLOAKED) &&
                ScreenOverlay(window, foreground, item.display.rcMonitor, item.window.process));
    });
    if (covering) {
        Queue(USER_TIMER_MINIMUM);
        WakeInk();
        return;
    }
    if (event != EVENT_OBJECT_LOCATIONCHANGE) {
        const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (GetAncestor(window, GA_ROOT) == window && !(style & WS_EX_TOOLWINDOW) &&
            (!GetWindow(window, GW_OWNER) || (style & WS_EX_APPWINDOW))) {
            taskbarLayoutDirty_ = true;
            Queue();
        }
        return;
    }
    const auto bar = std::find_if(bars_.begin(), bars_.end(), [window](const Taskbar& item) {
        return item.window.hwnd == window;
    });
    if (bar != bars_.end()) {
        RECT bounds{};
        if (appearance::ReadDockBounds(window, bounds) && !SameRect(bounds, bar->bounds)) Refresh();
        else if (bar->primary && (GetPropW(window, appearance::dockHeightProperty) ||
                                  GetPropW(window, appearance::dockWidthProperty))) {
            RECT actual{};
            if (GetWindowRect(window, &actual) && SameRect(actual, bounds)) appearance::RefreshLayout(host_);
        }
    } else if (window == foreground || window == GetAncestor(foreground, GA_ROOTOWNER)) Queue(USER_TIMER_MINIMUM);
}

void Overlay::OnAppBar(WPARAM notification) {
    if (!Enabled()) return;
    if (!session_.Active()) {
        if (notification == ABN_POSCHANGED || notification == ABN_STATECHANGE) QueueResume();
        return;
    }
    if (notification == ABN_POSCHANGED || notification == ABN_STATECHANGE) topologyDirty_ = true;
    if (notification == ABN_FULLSCREENAPP) Queue(USER_TIMER_MINIMUM);
    else if (topologyDirty_) Queue();
}

void Overlay::OnTimer(WPARAM timer) {
    if (!Enabled()) return;
    KillTimer(host_, timer);
    if (timer == resumeTimer) {
        resumeQueued_ = false;
        const auto now = GetTickCount64();
        if (session_.Resume(now)) {
            Refresh(true);
        } else if (session_.Settling()) {
            resumeQueued_ = SetTimer(host_, resumeTimer, std::max<UINT>(USER_TIMER_MINIMUM, session_.Delay(now)), nullptr) != 0;
        }
        return;
    }
    if (!session_.Active()) return;
    if (timer == inkTimer) {
        inkDue_ = 0;
        SampleInk();
        return;
    }
    if (timer == updateTimer) {
        queuedDelay_ = 0;
        if (topologyDirty_) {
            topologyDirty_ = false;
            ReadTaskbars();
        }
        if (std::exchange(taskbarLayoutDirty_, false)) appearance::RefreshLayout(host_);
        SizeMaximized(GetForegroundWindow());
    }
    FloatTaskbar();
    if (timer == settleTimer && primaryLayer_ == appearance::LayerMode::floating) {
        // The shell can rebuild its materials after the first layout update.
        UpdateAppearance();
        appearance::RefreshLatchOrder(host_);
    }
}

void Overlay::SizeMaximized(HWND hwnd) {
    if (autoHide_ || !hwnd || !IsZoomed(hwnd)) return;
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    const auto bar = std::find_if(bars_.begin(), bars_.end(), [monitor](const Taskbar& item) {
        return item.monitor == monitor && item.work.Owned();
    });
    const WindowId candidate(hwnd);
    if (bar == bars_.end() || bar->resizeAttempt == candidate) return;
    MONITORINFOEXW display{};
    if (!MonitorInfo(monitor, display)) return;
    if (!SameRect(display.rcWork, bar->work.expanded)) {
        Refresh();
        return;
    }
    if (ResizeMaximized(hwnd, ResizeRequest{monitor, display.rcMonitor, bar->work.original, bar->work.expanded}))
        bar->resizeAttempt = candidate;
}

void Overlay::RestoreWorkAreas() {
    for (auto& bar : bars_) {
        if (!bar.work.Owned()) continue;
        MONITORINFOEXW display{};
        if (MonitorInfo(bar.monitor, display) && bar.work.Restore(display, autoHide_))
            ResizeMonitor(bar.monitor, display.rcMonitor, bar.work.expanded, bar.work.original);
        bar.work = {};
    }
}

void Overlay::FloatTaskbar() {
    if (autoHide_) return;
    const HWND window = GetAncestor(GetForegroundWindow(), GA_ROOT);
    const HWND owner = GetAncestor(window, GA_ROOTOWNER);
    const DWORD foregroundThread = GetWindowThreadProcessId(window, nullptr);
    const auto foreground = window != host_ ? ReadSurface(window) : std::nullopt;
    const auto ownerSurface = owner != window && owner != host_ ? ReadSurface(owner) : std::nullopt;

    for (auto& bar : bars_) {
        const HWND tray = bar.window.hwnd;
        if (!bar.window.Alive() || IsHungAppWindow(tray)) continue;
        const RECT& monitor = bar.display.rcMonitor;
        const bool trayHasFocus = window == tray || owner == tray;
        const auto mode = foreground && !trayHasFocus ? foreground->Mode(monitor) : WindowMode::windowed;
        HWND cover = FindScreenOverlay(window, monitor, bar.window.process);
        if (!cover && mode == WindowMode::fullscreen) cover = window;
        if (!cover && !trayHasFocus && ownerSurface && ownerSurface->Mode(monitor) == WindowMode::fullscreen)
            cover = owner;

        const bool coverEnded = bar.cover.hwnd && !cover;
        bar.cover = WindowId(cover);
        if (bar.primary)
            SetPrimaryLayer(cover ? appearance::LayerMode::covered : appearance::LayerMode::floating);
        if (cover) {
            if (!bar.primary) HideSecondaryBar(bar);
            continue;
        }
        RestoreAfterCover(bar);
        if (InMenu(bar.window.thread) || InMenu(foregroundThread)) continue;

        const bool visible = IsWindowVisible(tray) != FALSE;
        if (bar.primary && !visible) continue;
        HWND targetWindow = nullptr;
        if (mode == WindowMode::maximized && window != tray && Overlaps(foreground->frame, bar.bounds)) {
            targetWindow = window;
            bar.lastMaximized = WindowId(window);
        } else if (coverEnded && bar.lastMaximized.Alive()) {
            // Capture may hand focus to a different window as it closes.
            const HWND previous = bar.lastMaximized.hwnd;
            const auto surface = ReadSurface(previous);
            if (surface && surface->Mode(monitor) == WindowMode::maximized && Overlaps(surface->frame, bar.bounds))
                targetWindow = previous;
        }
        if (!targetWindow) continue;
        if (visible && Above(tray, targetWindow)) {
            bar.lastOrder = nullptr;
            continue;
        }

        const bool wasTopmost = Topmost(tray);
        const UINT flags = visible ? orderFlags : orderFlags | SWP_SHOWWINDOW;
        const WindowId target(targetWindow);
        HWND insertAfter = HWND_TOPMOST;
        if (Topmost(targetWindow)) {
            const HWND higher = GetWindow(targetWindow, GW_HWNDPREV);
            if (higher && higher != tray && Topmost(higher)) insertAfter = higher;
        }
        if (OrderTaskbar(bar, insertAfter, flags)) {
            if (bar.raisedOver != target) {
                bar.wasTopmost = wasTopmost;
                bar.wasVisible = visible;
            }
            bar.raisedOver = target;
        }
    }
}

void Overlay::HideSecondaryBar(Taskbar& bar) {
    const HWND tray = bar.window.hwnd;
    if (!IsWindowVisible(tray)) return;
    const bool topmost = Topmost(tray);
    if (OrderTaskbar(bar, nullptr, orderFlags | SWP_NOZORDER | SWP_HIDEWINDOW)) {
        if (!bar.hiddenByHost) bar.topmostBeforeCover = topmost;
        bar.hiddenByHost = true;
    }
}

bool Overlay::OrderTaskbar(Taskbar& bar, HWND insertAfter, UINT flags) {
    const ULONGLONG now = GetTickCount64();
    if (insertAfter == bar.lastOrder && flags == bar.lastOrderFlags && now - bar.lastOrderAt < 160)
        return false;
    if (!SetWindowPos(bar.window.hwnd, insertAfter, 0, 0, 0, 0, flags)) return false;
    bar.lastOrder = insertAfter;
    bar.lastOrderFlags = flags;
    bar.lastOrderAt = now;
    return true;
}

void Overlay::RestoreAfterCover(Taskbar& bar) {
    if (bar.hiddenByHost) {
        const HWND insertAfter = bar.topmostBeforeCover ? HWND_TOPMOST : nullptr;
        const UINT flags = orderFlags | SWP_SHOWWINDOW |
            (bar.topmostBeforeCover ? 0 : SWP_NOZORDER);
        if (OrderTaskbar(bar, insertAfter, flags)) bar.hiddenByHost = false;
    }
    bar.cover = WindowId(nullptr);
}

void Overlay::RestoreOrder() {
    const HWND foreground = GetForegroundWindow();
    for (auto& bar : bars_) {
        const HWND tray = bar.window.hwnd;
        if (!bar.window.Alive() || IsHungAppWindow(tray)) continue;
        RestoreAfterCover(bar);
        if (FindScreenOverlay(foreground, bar.display.rcMonitor, bar.window.process)) continue;
        if (bar.raisedOver.hwnd != foreground || !bar.raisedOver.Alive() || !Topmost(tray))
            continue;
        const UINT flags = bar.wasVisible ? orderFlags : orderFlags | SWP_HIDEWINDOW;
        if (!bar.wasTopmost)
            SetWindowPos(tray, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
        else if (Topmost(foreground) && Above(tray, foreground))
            SetWindowPos(tray, foreground, 0, 0, 0, 0, flags);
    }
}

} // namespace fullwintb

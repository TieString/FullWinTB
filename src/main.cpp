#include "overlay.h"
#include "tray.h"
#include "appearance.h"

#include <windowsx.h>
#include <wtsapi32.h>

namespace {

using fullwintb::appearance::hostClass;
// Share the lock with the previous name so two releases cannot own the taskbar.
constexpr wchar_t instanceName[] = L"Global\\AuraBar_SingleInstance";

class Application {
public:
    int Run(HINSTANCE instance) {
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        WNDCLASSW spec{};
        spec.hInstance = instance;
        spec.lpszClassName = hostClass;
        spec.lpfnWndProc = WindowProc;
        if (!RegisterClassW(&spec)) return 1;
        fullwintb::WorkArea::Recover();

        // TaskbarCreated and display changes are broadcast to top-level windows.
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, hostClass,
                                      L"FullWinTB", WS_POPUP, 0, 0, 0, 0, nullptr,
                                      nullptr, instance, this);
        if (!window) return 1;
        sessionNotification_ = WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION) != FALSE;
        tray_.Start(window);
        overlay_.SetInk(tray_.Ink());
        fullwintb::appearance::SetWheelAxis(window, tray_.WheelAxis());
        fullwintb::appearance::SetMotionSettings(window, tray_.Motion());
        fullwintb::appearance::SetReturnStyle(window, tray_.ReturnStyle());
        if (fullwintb::Tray::EnableOnLaunch()) tray_.SetEnabled(overlay_.Start(window));

        MSG msg{};
        int result = 0;
        while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (IsWindow(window)) DestroyWindow(window);
        return result < 0 ? 1 : static_cast<int>(msg.wParam);
    }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return app ? app->Dispatch(window, msg, wp, lp) : DefWindowProcW(window, msg, wp, lp);
    }

    LRESULT Dispatch(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        if (taskbarCreated_ && msg == taskbarCreated_) {
            tray_.Recreate();
            overlay_.ShellRestarted();
            return 0;
        }
        switch (msg) {
        case fullwintb::Tray::message:
            if (LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == NIN_SELECT ||
                LOWORD(lp) == NIN_KEYSELECT) {
                const POINT anchor{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)};
                Command(window, tray_.ShowMenu(anchor));
            }
            return 0;
        case WM_HOTKEY:
            if (tray_.IsToggleHotkey(wp)) Command(window, fullwintb::Tray::toggle);
            return 0;
        case WM_COMMAND:
            Command(window, LOWORD(wp));
            return 0;
        case fullwintb::Overlay::appBarMessage:
            overlay_.OnAppBar(wp);
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_LOCK) overlay_.SessionLocked();
            else if (wp == WTS_SESSION_UNLOCK) overlay_.SessionUnlocked();
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            overlay_.Refresh();
            if (overlay_.Enabled() && (msg == WM_THEMECHANGED ||
                (msg == WM_SETTINGCHANGE && (wp == SPI_SETHIGHCONTRAST ||
                 (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0))))) {
                overlay_.UpdateAppearance();
            }
            return 0;
        case WM_TIMER:
            if (wp == fullwintb::Overlay::updateTimer || wp == fullwintb::Overlay::settleTimer ||
                wp == fullwintb::Overlay::inkTimer || wp == fullwintb::Overlay::resumeTimer)
                overlay_.OnTimer(wp);
            return 0;
        case WM_ENDSESSION:
            if (wp) DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            if (sessionNotification_) WTSUnRegisterSessionNotification(window);
            overlay_.Stop();
            tray_.Stop();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, msg, wp, lp);
        }
    }

    void Command(HWND window, UINT command) {
        switch (command) {
        case fullwintb::Tray::toggle:
            if (overlay_.Enabled()) overlay_.Stop();
            else if (!overlay_.Start(window))
                MessageBoxW(window, fullwintb::Localized(tray_.Language(), L"无法监听窗口变化。",
                            L"Could not monitor window changes."), L"FullWinTB", MB_OK | MB_ICONWARNING);
            tray_.SetEnabled(overlay_.Enabled());
            tray_.SaveEnabled();
            break;
        case fullwintb::Tray::refresh:
            overlay_.Refresh(true);
            overlay_.UpdateAppearance();
            break;
        case fullwintb::Tray::resetIcons:
            fullwintb::appearance::ResetIcons(window);
            break;
        case fullwintb::Tray::horizontalMotion:
        case fullwintb::Tray::verticalMotion:
            tray_.SetWheelAxis(static_cast<fullwintb::appearance::MotionAxis>(command - fullwintb::Tray::horizontalMotion));
            fullwintb::appearance::SetWheelAxis(window, tray_.WheelAxis());
            break;
        case fullwintb::Tray::configureHotkey:
            tray_.ShowHotkeyDialog(window, tray_.Language());
            break;
        case fullwintb::Tray::configureSettings:
            tray_.ShowSettingsDialog();
            overlay_.SetInk(tray_.Ink());
            break;
        case fullwintb::Tray::systemInk:
        case fullwintb::Tray::whiteInk:
        case fullwintb::Tray::blackInk:
        case fullwintb::Tray::automaticInk:
            tray_.SetInk(static_cast<fullwintb::appearance::Ink>(command - fullwintb::Tray::systemInk));
            overlay_.SetInk(tray_.Ink());
            break;
        case fullwintb::Tray::quit:
            DestroyWindow(window);
            break;
        }
    }

    fullwintb::Overlay overlay_;
    fullwintb::Tray tray_;
    UINT taskbarCreated_ = 0;
    bool sessionNotification_ = false;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HANDLE single = CreateMutexW(nullptr, FALSE, instanceName);
    if (!single) return 1;
    const bool exists = GetLastError() == ERROR_ALREADY_EXISTS;
    const int result = exists ? 0 : Application{}.Run(instance);
    CloseHandle(single);
    return result;
}

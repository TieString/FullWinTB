#include "appearance.h"
#include "embedded_component.h"
#include "taskbar_layer.h"

namespace fullwintb::appearance {

void RefreshLayout(HWND host) {
    static const UINT message = RegisterWindowMessageW(layoutMessage);
    if (const HWND control = FindWindowExW(HWND_MESSAGE, nullptr, controlClass, nullptr))
        PostMessageW(control, message, reinterpret_cast<WPARAM>(host), 0);
}

void ResetIcons(HWND host) {
    static const UINT message = RegisterWindowMessageW(resetIconsMessage);
    if (const HWND control = FindWindowExW(HWND_MESSAGE, nullptr, controlClass, nullptr))
        PostMessageW(control, message, reinterpret_cast<WPARAM>(host), 0);
}

void RefreshLatchOrder(HWND host) {
    if (const HWND control = FindWindowExW(HWND_MESSAGE, nullptr, controlClass, nullptr))
        PostMessageW(control, taskbarOrderChanged, 0, reinterpret_cast<LPARAM>(host));
}

void SetWheelAxis(HWND host, MotionAxis axis) {
    static const UINT message = RegisterWindowMessageW(motionAxisMessage);
    if (!SetPropW(host, requestedMotionAxis, reinterpret_cast<HANDLE>(1 + static_cast<UINT_PTR>(axis)))) return;
    if (const HWND control = FindWindowExW(HWND_MESSAGE, nullptr, controlClass, nullptr))
        PostMessageW(control, message, reinterpret_cast<WPARAM>(host), static_cast<LPARAM>(axis));
}

void SetReturnStyle(HWND host, ReturnStyle style) {
    static const UINT message = RegisterWindowMessageW(motionSettingsMessage);
    if (!SetPropW(host, requestedReturnStyle, reinterpret_cast<HANDLE>(1 + static_cast<UINT_PTR>(style)))) return;
    if (const HWND control = FindWindowExW(HWND_MESSAGE, nullptr, controlClass, nullptr))
        PostMessageW(control, message, reinterpret_cast<WPARAM>(host), 0);
}

void SetMotionSettings(HWND host, const MotionSettings& settings) {
    static const UINT message = RegisterWindowMessageW(motionSettingsMessage);
    const auto value = PackMotionSettings(settings);
    if (value == ReadMotionPacket(host)) return;
    // A reader must see both halves from the same update, including on x86.
    const auto writing = reinterpret_cast<UINT_PTR>(GetPropW(host, motionPacketSequence)) | 1;
    const auto committed = writing + 1;
    if (!SetPropW(host, motionPacketSequence, reinterpret_cast<HANDLE>(writing)) ||
        !SetPropW(host, motionPacketLow, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(value & 0xffffffff))) ||
        !SetPropW(host, motionPacketHigh, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(value >> 32))) ||
        !SetPropW(host, motionPacketSequence, reinterpret_cast<HANDLE>(committed ? committed : 2))) return;
    if (const HWND control = FindWindowExW(HWND_MESSAGE, nullptr, controlClass, nullptr))
        PostMessageW(control, message, reinterpret_cast<WPARAM>(host), 0);
}

void Update(HWND host, bool enabled, Ink ink, LayerMode layer) {
    DWORD explorer = 0, taskbarProcess = 0;
    GetWindowThreadProcessId(GetShellWindow(), &explorer);
    const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    const DWORD thread = GetWindowThreadProcessId(tray, &taskbarProcess);
    if (!thread || !explorer || taskbarProcess != explorer) return;

    const UINT message = RegisterWindowMessageW(updateMessage);
    if (!message) return;
    const WPARAM owner = reinterpret_cast<WPARAM>(host);
    const LPARAM mode = enabled ? ((1 + static_cast<LPARAM>(ink)) |
                                  (static_cast<LPARAM>(layer) << layerShift)) : 0;
    if (!SetPropW(host, requestedMode, reinterpret_cast<HANDLE>(mode + 1))) return;
    HWND control = FindWindowExW(HWND_MESSAGE, nullptr, controlClass, nullptr);
    if (control && GetWindowThreadProcessId(control, nullptr) == thread) {
        if (enabled) {
            PostMessageW(control, message, owner, mode);
        } else {
            DWORD_PTR result = 0;
            SendMessageTimeoutW(control, message, owner, mode, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                150, &result);
        }
        return;
    }
    if (!enabled) return;

    const auto path = PrepareAppearanceModule();
    if (path.empty()) return;
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return;
    const auto proc = reinterpret_cast<HOOKPROC>(GetProcAddress(module, "OpenAppearance"));
    if (proc) {
        HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, proc, module, thread);
        if (hook) {
            DWORD_PTR result = 0;
            SendMessageTimeoutW(tray, message, owner, mode,
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 150, &result);
            UnhookWindowsHookEx(hook);
        }
    }
    FreeLibrary(module);
}

} // namespace fullwintb::appearance

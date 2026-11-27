#include "tray.h"
#include "motion_dialog.h"
#include "resource.h"

#include <commctrl.h>
#include <cwchar>
#include <iterator>

namespace fullwintb {
namespace {

constexpr wchar_t settingsKey[] = L"Software\\FullWinTB";
constexpr wchar_t legacySettingsKey[] = L"Software\\AuraBar";
constexpr UINT modifierMask = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;

template<typename T>
bool ReadSetting(const wchar_t* setting, DWORD type, T& value) {
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, settingsKey, setting, type, nullptr, &value, &size);
    if (status == ERROR_SUCCESS) return true;
    if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) return false;
    size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, legacySettingsKey, setting, type, nullptr, &value, &size) == ERROR_SUCCESS;
}

bool IsSecondOption(const wchar_t* setting) {
    DWORD value = 0;
    return ReadSetting(setting, RRF_RT_REG_DWORD, value) && value == 1;
}

bool SaveOption(const wchar_t* setting, DWORD value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, setting, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}

HICON CircleIcon(DWORD rgb) {
    constexpr int size = 16;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) return nullptr;

    constexpr WORD emptyMask[size]{};
    HBITMAP mask = CreateBitmap(size, size, 1, 1, emptyMask);
    HICON icon = nullptr;
    if (mask) {
        auto* pixels = static_cast<DWORD*>(bits);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const int dx = 2 * x - size + 1;
                const int dy = 2 * y - size + 1;
                pixels[y * size + x] = (dx * dx + dy * dy <= 13 * 13) ? (0xff000000 | rgb) : 0;
            }
        }
        ICONINFO iconInfo{TRUE, 0, 0, mask, color};
        icon = CreateIconIndirect(&iconInfo);
        DeleteObject(mask);
    }
    DeleteObject(color);
    return icon;
}

} // namespace

void Tray::Start(HWND owner) {
    DWORD color = 0;
    if (ReadSetting(L"TaskbarInk", RRF_RT_REG_DWORD, color) &&
        color <= static_cast<DWORD>(appearance::Ink::automatic))
        ink_ = static_cast<appearance::Ink>(color);
    DWORD direction = 0;
    if (ReadSetting(L"MotionAxis", RRF_RT_REG_DWORD, direction) &&
        direction == static_cast<DWORD>(appearance::MotionAxis::vertical))
        axis_ = appearance::MotionAxis::vertical;
    ULONGLONG motion = 0;
    if (ReadSetting(L"MotionFeel", RRF_RT_REG_QWORD, motion) ||
        ReadSetting(L"MagnetFeel", RRF_RT_REG_QWORD, motion))
        motion_ = appearance::UnpackMotionSettings(motion);
    if (IsSecondOption(L"ReturnStyle")) returnStyle_ = appearance::ReturnStyle::simple;
    if (IsSecondOption(L"Language")) language_ = UiLanguage::english;
    enabledIcon_ = CircleIcon(0x50c850);
    disabledIcon_ = CircleIcon(0x6482c8);
    icon_.cbSize = sizeof(icon_);
    icon_.hWnd = owner;
    icon_.uID = 1;
    icon_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    icon_.uCallbackMessage = message;
    hotkey_ = LoadHotkey();
    if (RegisterHotKey(owner, 1, hotkey_.modifiers | MOD_NOREPEAT, hotkey_.key))
        hotkeyId_ = 1;
    Recreate();
}

void Tray::Stop() {
    Shell_NotifyIconW(NIM_DELETE, &icon_);
    if (hotkeyId_) UnregisterHotKey(icon_.hWnd, hotkeyId_);
    if (enabledIcon_) DestroyIcon(enabledIcon_);
    if (disabledIcon_) DestroyIcon(disabledIcon_);
}

void Tray::Recreate() {
    UpdateTip();
    if (Shell_NotifyIconW(NIM_ADD, &icon_)) {
        icon_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon_);
    }
}

void Tray::UpdateTip() {
    icon_.hIcon = enabled_ ? enabledIcon_ : disabledIcon_;
    if (!icon_.hIcon) icon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wchar_t key[64]{};
    if (hotkeyId_) KeyLabel(hotkey_, key);
    else wcscpy_s(key, Localized(language_, L"快捷键不可用", L"Shortcut unavailable"));
    swprintf_s(icon_.szTip, L"FullWinTB %ls · %ls", enabled_
        ? Localized(language_, L"已启用", L"enabled") : Localized(language_, L"已禁用", L"disabled"), key);
}

void Tray::SetEnabled(bool enabled) {
    enabled_ = enabled;
    UpdateTip();
    Shell_NotifyIconW(NIM_MODIFY, &icon_);
}

bool Tray::EnableOnLaunch() {
    DWORD enabled = 0;
    return !ReadSetting(L"Enabled", RRF_RT_REG_DWORD, enabled) || enabled != 0;
}

void Tray::SaveEnabled() const {
    const DWORD enabled = enabled_ ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, L"Enabled", REG_DWORD, &enabled, sizeof(enabled));
}

void Tray::SetInk(appearance::Ink ink) {
    ink_ = ink;
    const DWORD value = static_cast<DWORD>(ink);
    RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, L"TaskbarInk", REG_DWORD,
                    &value, sizeof(value));
}

void Tray::SetWheelAxis(appearance::MotionAxis axis) {
    axis_ = axis;
    const DWORD value = static_cast<DWORD>(axis);
    RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, L"MotionAxis", REG_DWORD, &value, sizeof(value));
}

Tray::Command Tray::ShowMenu(POINT cursor) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return {};
    wchar_t key[64]{}, label[96]{};
    if (hotkeyId_) KeyLabel(hotkey_, key);
    swprintf_s(label, L"%ls FullWinTB\t%ls", enabled_
        ? Localized(language_, L"禁用", L"Disable") : Localized(language_, L"启用", L"Enable"), key);
    AppendMenuW(menu, MF_STRING, toggle, label);
    AppendMenuW(menu, MF_STRING | (enabled_ ? MF_ENABLED : MF_GRAYED), refresh, Localized(language_, L"刷新", L"Refresh"));
    AppendMenuW(menu, MF_STRING, configureSettings, Localized(language_, L"设置…", L"Settings…"));
    AppendMenuW(menu, MF_STRING, quit, Localized(language_, L"退出", L"Quit"));
    SetForegroundWindow(icon_.hWnd);
    const auto command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                        cursor.x, cursor.y, 0, icon_.hWnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(icon_.hWnd, WM_NULL, 0, 0);
    return static_cast<Command>(command);
}

Tray::Hotkey Tray::LoadHotkey() {
    Hotkey key;
    DWORD value = 0;
    if (ReadSetting(L"Hotkey", RRF_RT_REG_DWORD, value)) {
        key = Hotkey{HIWORD(value), LOWORD(value)};
    } else {
        if (ReadSetting(L"HotkeyMod", RRF_RT_REG_DWORD, value)) key.modifiers = value;
        if (ReadSetting(L"HotkeyVk", RRF_RT_REG_DWORD, value)) key.key = value;
    }
    if (!key.key || key.key > 0xff || (key.modifiers & ~modifierMask)) return {};
    return key;
}

bool Tray::ChangeHotkey(Hotkey key) {
    if (key == hotkey_ && hotkeyId_) return true;
    const UINT next = hotkeyId_ == 1 ? 2 : 1;
    if (!RegisterHotKey(icon_.hWnd, next, key.modifiers | MOD_NOREPEAT, key.key)) return false;
    const DWORD value = MAKELONG(key.key, key.modifiers);
    if (RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, L"Hotkey",
                       REG_DWORD, &value, sizeof(value)) != ERROR_SUCCESS) {
        UnregisterHotKey(icon_.hWnd, next);
        return false;
    }
    if (hotkeyId_) UnregisterHotKey(icon_.hWnd, hotkeyId_);
    hotkeyId_ = next;
    hotkey_ = key;
    SetEnabled(enabled_);
    return true;
}

void Tray::KeyLabel(const Hotkey& key, wchar_t (&text)[64]) {
    wchar_t name[32]{};
    const UINT scan = MapVirtualKeyW(key.key, MAPVK_VK_TO_VSC_EX);
    const LONG keyInfo = static_cast<LONG>((scan & 0xff) << 16 | (scan & 0xff00 ? 1 << 24 : 0));
    if (!GetKeyNameTextW(keyInfo, name, static_cast<int>(std::size(name))))
        swprintf_s(name, L"0x%02X", key.key);
    swprintf_s(text, L"%ls%ls%ls%ls%ls",
               key.modifiers & MOD_CONTROL ? L"Ctrl+" : L"",
               key.modifiers & MOD_ALT ? L"Alt+" : L"",
               key.modifiers & MOD_SHIFT ? L"Shift+" : L"",
               key.modifiers & MOD_WIN ? L"Win+" : L"", name);
}

LRESULT CALLBACK Tray::CaptureKey(HWND window, UINT msg, WPARAM wp, LPARAM lp,
                                   UINT_PTR, DWORD_PTR data) {
    if (msg == WM_GETDLGCODE && lp && wp != VK_TAB && wp != VK_ESCAPE && wp != VK_RETURN)
        return DLGC_WANTMESSAGE;
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        switch (wp) {
        case VK_CONTROL:
        case VK_SHIFT:
        case VK_MENU:
        case VK_LWIN:
        case VK_RWIN:
            return 0;
        default:
            break;
        }
        auto& edit = *reinterpret_cast<HotkeyEdit*>(data);
        UINT mods = 0;
        if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
        if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
        if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
        if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
        edit.candidate = Hotkey{mods, static_cast<UINT>(wp)};
        wchar_t text[64]{};
        KeyLabel(edit.candidate, text);
        SetWindowTextW(window, text);
        return 0;
    }
    if (msg == WM_CHAR || msg == WM_SYSCHAR) return 0;
    return DefSubclassProc(window, msg, wp, lp);
}

INT_PTR CALLBACK Tray::HotkeyDialog(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        auto& edit = *reinterpret_cast<HotkeyEdit*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, Localized(edit.language, L"FullWinTB 快捷键", L"FullWinTB Shortcut"));
        SetDlgItemTextW(dialog, IDC_HOTKEY_HINT,
                        Localized(edit.language, L"在输入框中按下新的快捷键", L"Press the new shortcut in the field."));
        SetDlgItemTextW(dialog, IDOK, Localized(edit.language, L"确定", L"OK"));
        SetDlgItemTextW(dialog, IDCANCEL, Localized(edit.language, L"取消", L"Cancel"));
        HWND field = GetDlgItem(dialog, IDC_CAPTURE);
        SetWindowSubclass(field, CaptureKey, 1, reinterpret_cast<DWORD_PTR>(&edit));
        wchar_t text[64]{};
        KeyLabel(edit.candidate, text);
        SetWindowTextW(field, text);
        return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        auto& edit = *reinterpret_cast<HotkeyEdit*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (edit.tray.ChangeHotkey(edit.candidate)) EndDialog(dialog, IDOK);
        else MessageBoxW(dialog, Localized(edit.language, L"快捷键已被占用或无法保存，原设置仍然有效。",
                         L"The shortcut is unavailable. Your previous shortcut is still active."),
                         L"FullWinTB", MB_OK | MB_ICONWARNING);
        return TRUE;
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void Tray::ShowHotkeyDialog(HWND owner, UiLanguage language) {
    HotkeyEdit edit{*this, hotkey_, language};
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_HOTKEY),
                    owner ? owner : icon_.hWnd, HotkeyDialog, reinterpret_cast<LPARAM>(&edit));
}

std::wstring Tray::HotkeyText(UiLanguage language) const {
    if (!hotkeyId_) return Localized(language, L"未注册", L"Unavailable");
    wchar_t text[64]{};
    KeyLabel(hotkey_, text);
    return text;
}

void Tray::ShowSettingsDialog() {
    const auto edited = EditTaskbarSettings(icon_.hWnd, *this, {motion_, axis_, ink_, returnStyle_, language_});
    if (!edited) return;
    if (edited->wheelAxis != axis_) SetWheelAxis(edited->wheelAxis);
    if (edited->ink != ink_) SetInk(edited->ink);
    bool saved = true;
    if (edited->returnStyle != returnStyle_) {
        if (SaveOption(L"ReturnStyle", static_cast<DWORD>(edited->returnStyle))) returnStyle_ = edited->returnStyle;
        else {
            appearance::SetReturnStyle(icon_.hWnd, returnStyle_);
            saved = false;
        }
    }
    if (edited->language != language_) {
        if (SaveOption(L"Language", static_cast<DWORD>(edited->language))) {
            language_ = edited->language;
            SetEnabled(enabled_);
        } else saved = false;
    }
    if (edited->motion != motion_) {
        const ULONGLONG value = appearance::PackMotionSettings(edited->motion);
        if (RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, L"MotionFeel", REG_QWORD, &value, sizeof(value)) ==
            ERROR_SUCCESS) motion_ = edited->motion;
        else {
            appearance::SetMotionSettings(icon_.hWnd, motion_);
            saved = false;
        }
    }
    if (!saved) MessageBoxW(icon_.hWnd, Localized(language_, L"部分设置无法保存，已恢复对应的上次设置。",
        L"Some settings could not be saved. Their previous values have been restored."), L"FullWinTB", MB_OK | MB_ICONWARNING);
}

} // namespace fullwintb

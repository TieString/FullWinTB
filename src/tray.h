#pragma once

#include "appearance.h"
#include "ui_language.h"
#include <windows.h>
#include <shellapi.h>
#include <string>

namespace fullwintb {

class Tray {
public:
    static constexpr UINT message = WM_APP + 1;
    enum Command : UINT {
        toggle = 200, refresh, quit, configureHotkey, systemInk, whiteInk, blackInk, automaticInk,
        resetIcons, horizontalMotion, verticalMotion, configureSettings
    };

    void Start(HWND owner);
    void Stop();
    void Recreate();
    void SetEnabled(bool enabled);
    static bool EnableOnLaunch();
    void SaveEnabled() const;
    void SetInk(appearance::Ink ink);
    void SetWheelAxis(appearance::MotionAxis axis);
    appearance::Ink Ink() const { return ink_; }
    appearance::MotionAxis WheelAxis() const { return axis_; }
    const appearance::MotionSettings& Motion() const { return motion_; }
    appearance::ReturnStyle ReturnStyle() const { return returnStyle_; }
    UiLanguage Language() const { return language_; }
    bool IsToggleHotkey(WPARAM id) const { return hotkeyId_ != 0 && id == hotkeyId_; }
    Command ShowMenu(POINT cursor);
    void ShowHotkeyDialog(HWND owner, UiLanguage language);
    std::wstring HotkeyText(UiLanguage language) const;
    void ShowSettingsDialog();

private:
    struct Hotkey {
        UINT modifiers = MOD_ALT | MOD_SHIFT;
        UINT key = 'A';
        bool operator==(const Hotkey&) const = default;
    };

    struct HotkeyEdit {
        Tray& tray;
        Hotkey candidate;
        UiLanguage language;
    };

    static INT_PTR CALLBACK HotkeyDialog(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK CaptureKey(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static void KeyLabel(const Hotkey& hotkey, wchar_t (&text)[64]);
    static Hotkey LoadHotkey();

    bool ChangeHotkey(Hotkey hotkey);
    void UpdateTip();

    NOTIFYICONDATAW icon_{};
    HICON enabledIcon_ = nullptr;
    HICON disabledIcon_ = nullptr;
    Hotkey hotkey_;
    UINT hotkeyId_ = 0;
    bool enabled_ = false;
    appearance::Ink ink_ = appearance::Ink::automatic;
    appearance::MotionAxis axis_ = appearance::MotionAxis::horizontal;
    appearance::MotionSettings motion_{};
    appearance::ReturnStyle returnStyle_ = appearance::ReturnStyle::elastic;
    UiLanguage language_ = UiLanguage::chinese;
};

} // namespace fullwintb

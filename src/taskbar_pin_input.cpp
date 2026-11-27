#include "taskbar_pin_input.h"

#include <algorithm>
#include <cstdlib>

namespace fullwintb::appearance {
namespace {

bool Near(POINT a, POINT b, LONG x, LONG y) {
    return std::abs(a.x - b.x) <= x && std::abs(a.y - b.y) <= y;
}

bool ModifiedClick() {
    for (const int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN, VK_RBUTTON, VK_MBUTTON}) {
        if (GetAsyncKeyState(key) & 0x8000) return true;
    }
    return false;
}

} // namespace

void PinInput::Arm(IconGroup group, Origin origin, POINT anchor, ULONGLONG now, UINT duration) {
    if (Active() && !Armed()) return;
    group_ = group;
    origin_ = origin;
    anchor_ = anchor;
    deadline_ = now + duration;
    phase_ = Phase::armed;
}

UINT PinInput::Remaining(ULONGLONG now) const {
    return deadline_ > now ? static_cast<UINT>(deadline_ - now) : 0;
}

void PinInput::Expire(ULONGLONG now) {
    if (!deadline_ || now < deadline_) return;
    if (phase_ == Phase::firstDown || phase_ == Phase::between) Replay(phase_ == Phase::between);
    Reset();
}

PinInput::Result PinInput::Route(UINT message, POINT cursor, bool eligible, UINT dpi, ULONGLONG now) {
    if (!Active()) return {};
    const LONG toleranceX = std::max(1, GetSystemMetricsForDpi(SM_CXDOUBLECLK, dpi) / 2);
    const LONG toleranceY = std::max(1, GetSystemMetricsForDpi(SM_CYDOUBLECLK, dpi) / 2);
    const bool nearFirst = Near(cursor, first_, toleranceX, toleranceY);
    if (message == WM_MOUSEMOVE) {
        if (Armed() && origin_ == Origin::wheel && !Near(cursor, anchor_, MulDiv(2, dpi, 96), MulDiv(2, dpi, 96))) {
            Reset();
            return {false, true};
        }
        if (!Armed() && !nearFirst && phase_ != Phase::discardUp) {
            Replay(phase_ != Phase::firstDown);
            if (phase_ == Phase::secondDown) phase_ = Phase::discardUp;
            else Reset();
            return {false, true};
        }
        return {};
    }
    if (message == WM_LBUTTONDOWN) {
        if (Armed() && eligible && Remaining(now) && !ModifiedClick()) {
            target_ = WindowFromPhysicalPoint(cursor);
            targetThread_ = GetWindowThreadProcessId(target_, nullptr);
            if (targetThread_) {
                first_ = cursor;
                deadline_ = now + GetDoubleClickTime();
                phase_ = Phase::firstDown;
                return {true, true};
            }
        }
        if (phase_ == Phase::between && eligible && nearFirst && Remaining(now) && !ModifiedClick()) {
            phase_ = Phase::secondDown;
            deadline_ = 0;
            return {true, true};
        }
    }
    if (message == WM_LBUTTONUP) {
        if (phase_ == Phase::firstDown) {
            if (Remaining(now)) phase_ = Phase::between;
            else {
                Replay(true);
                Reset();
            }
            return {true, true};
        }
        if (phase_ == Phase::secondDown || phase_ == Phase::discardUp) {
            const auto pin = phase_ == Phase::secondDown && eligible && nearFirst ? std::optional(group_) : std::nullopt;
            if (!pin && phase_ == Phase::secondDown) Replay(true);
            Reset();
            return {true, true, pin};
        }
        return {};
    }
    if (phase_ == Phase::between) Replay(true);
    if (Armed() || phase_ == Phase::between) {
        Reset();
        return {false, true};
    }
    return {};
}

void PinInput::Replay(bool released) const {
    if (GetWindowThreadProcessId(target_, nullptr) != targetThread_ ||
        WindowFromPhysicalPoint(first_) != target_) return;
    POINT cursor{};
    if (GetPhysicalCursorPos(&cursor) && cursor.x == first_.x && cursor.y == first_.y) {
        INPUT input[2]{};
        input[0].type = input[1].type = INPUT_MOUSE;
        input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        input[0].mi.dwExtraInfo = input[1].mi.dwExtraInfo = replayTag;
        if (SendInput(released ? 2 : 1, input, sizeof(INPUT))) return;
    }

    // The user moved on while the tap was pending. Deliver it at its original
    // client position without moving the pointer or picking a different window.
    POINT client = first_;
    const auto dpi = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(target_));
    const bool mapped = PhysicalToLogicalPointForPerMonitorDPI(target_, &client) && ScreenToClient(target_, &client);
    if (dpi) SetThreadDpiAwarenessContext(dpi);
    if (!mapped) return;
    const LPARAM position = MAKELPARAM(client.x, client.y);
    PostMessageW(target_, WM_LBUTTONDOWN, MK_LBUTTON, position);
    if (released) PostMessageW(target_, WM_LBUTTONUP, 0, position);
}

} // namespace fullwintb::appearance

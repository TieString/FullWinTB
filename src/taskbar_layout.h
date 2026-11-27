#pragma once

#include "appearance.h"
#include <windows.h>
#include <vector>

namespace fullwintb::appearance {

enum class IconGroup { apps, notification };

struct IconOffset {
    LONG x = 0;
    LONG y = 0;
    LONG& Along(MotionAxis axis) { return axis == MotionAxis::horizontal ? x : y; }
    LONG Along(MotionAxis axis) const { return axis == MotionAxis::horizontal ? x : y; }
    explicit operator bool() const { return x || y; }
    bool operator==(const IconOffset&) const = default;
};

struct IconOffsets {
    IconOffset apps;
    IconOffset notification;
    IconOffset& For(IconGroup group) { return group == IconGroup::apps ? apps : notification; }
    IconOffset For(IconGroup group) const { return group == IconGroup::apps ? apps : notification; }
    bool operator==(const IconOffsets&) const = default;
};

struct TaskbarLayout {
    HWND bar = nullptr;
    RECT bounds{};
    RECT dock{};
    RECT notification{};
    UINT dpi = 96;
    RECT startButton{};
    RECT monitor{};
    bool widgets = false;
    std::vector<RECT> buttons;
};

TaskbarLayout ReadTaskbarGeometry(HWND bar);
TaskbarLayout ReadTaskbarLayout(HWND bar, IconOffset appsOffset);

} // namespace fullwintb::appearance

#pragma once

#include "appearance.h"
#include "ui_language.h"

#include <optional>

namespace fullwintb {

class Tray;

struct TaskbarSettings {
    appearance::MotionSettings motion;
    appearance::MotionAxis wheelAxis;
    appearance::Ink ink;
    appearance::ReturnStyle returnStyle;
    UiLanguage language;
};

std::optional<TaskbarSettings> EditTaskbarSettings(HWND owner, Tray& tray, TaskbarSettings settings);

} // namespace fullwintb

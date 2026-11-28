#pragma once

#include "appearance.h"

#include <array>

namespace fullwintb::appearance {

inline constexpr int taskbarInkGridSide = 16;
using TaskbarInkGrid = std::array<RGBQUAD, taskbarInkGridSide * taskbarInkGridSide>;

Ink ClassifyTaskbarInk(const TaskbarInkGrid& pixels, bool horizontal);

class InkSamples {
public:
    void Add(unsigned brightness);
    Ink Preferred() const;

private:
    unsigned count_ = 0;
    unsigned dark_ = 0;
    unsigned light_ = 0;
};

class AutomaticInk {
public:
    bool Observe(Ink wanted, ULONGLONG now);
    void Pause() { pending_ = Ink::system; }
    Ink Current() const { return current_; }
    bool Settling() const { return pending_ != Ink::system; }

private:
    Ink current_ = Ink::system;
    Ink pending_ = Ink::system;
    ULONGLONG pendingSince_ = 0;
};

struct TaskbarInkSample {
    Ink notification = Ink::system;
    Ink apps = Ink::system;
};

TaskbarInkSample ReadTaskbarInk(HWND bar, const RECT& monitor, bool horizontal);

} // namespace fullwintb::appearance

#pragma once

#include "taskbar_layout.h"

namespace fullwintb::appearance {

enum class LatchGesture : UINT { toggle, dragStart, drag, dragEnd, peelStart, peelEnd, pin, peelRemove };
inline constexpr UINT latchMessage = WM_APP + 5;

class IconLatch {
public:
    void Show(HINSTANCE module, HWND bar, HWND actions, IconGroup group,
              const RECT& anchor, UINT dpi, const RECT& monitor, bool corner, bool pinned);
    void Hide();
    void Destroy();
    void KeepAbove(HWND bar) const;

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT message, WPARAM wp, LPARAM lp);
    bool Present(const RECT& bounds) const;
    void PullPaper(POINT pointer);
    void EndPress(bool released);
    void Hover(bool hot);
    double PeelTarget() const;
    void RetargetPeel();
    void StepPeel();
    void StopPeel();
    void Send(LatchGesture gesture) const;

    HWND window_ = nullptr;
    HWND actions_ = nullptr;
    RECT bounds_{};
    UINT dpi_ = 96;
    IconGroup group_ = IconGroup::apps;
    bool visible_ = false;
    bool corner_ = false;
    bool below_ = false;
    bool pinned_ = false;
    bool hot_ = false;
    bool down_ = false;
    bool dragged_ = false;
    POINT peelStart_{};
    float peelPull_ = 0;
    int peelSide_ = -1;
    double peel_ = 0;
    double peelVelocity_ = 0;
    ULONGLONG peelTick_ = 0;
};

} // namespace fullwintb::appearance

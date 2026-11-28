#pragma once

#include "appearance.h"

namespace fullwintb::appearance {

bool WriteTaskbarRegion(HWND bar, HRGN region, bool animated = false);
inline constexpr UINT taskbarOrderChanged = WM_APP + 7;

class TaskbarLayer {
public:
    void Update(HWND bar, LayerMode mode, HWND orderObserver = nullptr);
    void HoldPresentation(bool held);
    bool EnsureCanvas(LONG travel, MotionAxis axis, bool towardEnd);
    void CloseCanvas();
    bool CanvasOpen() const { return bridge_ != nullptr; }
    bool CanvasIntact() const;

private:
    static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void ApplyPosition();
    void Detach();
    bool SizeCanvas(const RECT& rectangle);

    HWND bar_ = nullptr;
    HWND orderObserver_ = nullptr;
    LayerMode mode_ = LayerMode::native;
    bool wasTopmost_ = false;
    bool hidBar_ = false;
    bool presentationHeld_ = false;
    bool updating_ = false;
    HWND bridge_ = nullptr;
    RECT dock_{};
    RECT viewport_{};
    bool sizingCanvas_ = false;
};

} // namespace fullwintb::appearance

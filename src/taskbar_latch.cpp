#include "taskbar_latch.h"
#include "resource.h"
#include "sticker_atlas.h"
#include "taskbar_motion.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <windowsx.h>

namespace fullwintb::appearance {
namespace {

constexpr wchar_t latchClass[] = L"FullWinTB.IconLatch.v" FULLWINTB_APPEARANCE_VERSION_WIDE;
constexpr int latchHeight = 12;
constexpr int cornerWidth = 66;
constexpr int cornerHeight = 50;
constexpr UINT_PTR peelTimer = 1;
constexpr double loosePeel = 0.60;
constexpr double looseHoverPeel = 0.52;
constexpr double pinnedHoverPeel = 0.38;
constexpr float removalFadeSpan = 0.24f;
const Spring peelSpring{1, 740, 0.82};

bool PaperHit(float x, float y, float left, float top, float right, float bottom, float radius) {
    const float dx = std::max(left + radius - x, x - (right - radius));
    const float dy = std::max(top + radius - y, y - (bottom - radius));
    const float outsideX = std::max(0.0f, dx);
    const float outsideY = std::max(0.0f, dy);
    return std::sqrt(outsideX * outsideX + outsideY * outsideY) + std::min(0.0f, std::max(dx, dy)) <= radius;
}

const std::uint8_t* ReadStickerAtlas() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ReadStickerAtlas), &module)) return nullptr;
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_STICKER_ATLAS), RT_RCDATA);
    if (!resource || SizeofResource(module, resource) != stickerAtlasBytes) return nullptr;
    return static_cast<const std::uint8_t*>(LockResource(LoadResource(module, resource)));
}

struct StickerPose {
    StickerPose(const std::uint8_t* atlas, float peel) : palette(atlas) {
        const float frame = std::clamp(peel, 0.0f, 1.0f) * (stickerFrames - 1);
        const int earlier = std::min(stickerFrames - 2, static_cast<int>(frame));
        blend = frame - earlier;
        first = atlas ? atlas + stickerPaletteBytes + earlier * stickerFrameBytes : nullptr;
        second = first ? first + stickerFrameBytes : nullptr;
    }

    std::uint32_t Pixel(float x, float y) const {
        if (!palette || x < 0 || x > 1 || y < 0 || y > 1) return 0;
        const float u = std::clamp(x * stickerWidth - .5f, 0.0f, stickerWidth - 1.0f);
        const float v = std::clamp(y * stickerHeight - .5f, 0.0f, stickerHeight - 1.0f);
        const int left = static_cast<int>(u);
        const int top = static_cast<int>(v);
        const int right = std::min(left + 1, stickerWidth - 1);
        const int bottom = std::min(top + 1, stickerHeight - 1);
        const auto color = [&](const std::uint8_t* frame, int col, int row) {
            return palette + frame[row * stickerWidth + col] * 4;
        };
        const auto* firstTL = color(first, left, top);
        const auto* firstTR = color(first, right, top);
        const auto* firstBL = color(first, left, bottom);
        const auto* firstBR = color(first, right, bottom);
        const auto* secondTL = color(second, left, top);
        const auto* secondTR = color(second, right, top);
        const auto* secondBL = color(second, left, bottom);
        const auto* secondBR = color(second, right, bottom);
        const float fractionX = u - left;
        const float fractionY = v - top;
        const auto sample = [&](float tl, float tr, float bl, float br) {
            return std::lerp(std::lerp(tl, tr, fractionX), std::lerp(bl, br, fractionX), fractionY);
        };
        std::uint32_t pixel = 0;
        for (int channel = 0; channel < 4; ++channel) {
            const float resting = sample(firstTL[channel], firstTR[channel], firstBL[channel], firstBR[channel]);
            const float next = sample(secondTL[channel], secondTR[channel], secondBL[channel], secondBR[channel]);
            const auto value = static_cast<std::uint32_t>(std::lerp(resting, next, blend) + .5f);
            pixel |= value << (8 * channel);
        }
        return pixel;
    }

    const std::uint8_t* palette;
    const std::uint8_t* first;
    const std::uint8_t* second;
    float blend;
};

std::uint32_t StickerPixel(const StickerPose& sticker, float x, float y, bool hit) {
    // Mirror the atlas so the same corner curls over the icon throughout the gesture.
    const auto pixel = sticker.Pixel(1.0f - x, y);
    return (pixel & 0xff000000) ? pixel : (hit ? 0x01000000 : 0);
}

void Rasterize(std::uint32_t* bitmap, SIZE size, const std::uint8_t* atlas,
               bool below, float peel, bool corner, const RECT& gripClearance) {
    const StickerPose sticker(atlas, peel);
    for (LONG y = 0; y < size.cy; ++y) {
        const LONG row = below ? size.cy - 1 - y : y;
        const bool crossesGrip = y >= gripClearance.top && y < gripClearance.bottom;
        for (LONG x = 0; x < size.cx; ++x) {
            // A zero-alpha pixel lets the real taskbar receive the pointer.
            if (crossesGrip && x >= gripClearance.left && x < gripClearance.right) {
                *bitmap++ = 0;
                continue;
            }
            if (corner) {
                const float dx = ((x + .5f) / size.cx - .5f) * cornerWidth;
                const float dy = ((y + .5f) / size.cy - .5f) * cornerHeight;
                // Sample the artwork after rotating the paper 18 degrees counterclockwise.
                constexpr float cosine = 0.9510565f;
                constexpr float sine = 0.3090170f;
                const float paperX = cosine * dx - sine * dy;
                const float paperY = sine * dx + cosine * dy;
                const bool hit = PaperHit(paperX, paperY, -22.0f, -10.0f, 22.0f, 5.0f, 4.0f);
                *bitmap++ = StickerPixel(sticker, .5f + paperX / 64.0f, .5f + paperY / 36.0f, hit);
            } else {
                // Use the group edge if the shell has no hidden-icons arrow.
                const float localX = ((x + .5f) / size.cx - .5f) * 26.0f;
                const float localY = (row + 0.5f) * latchHeight / size.cy;
                const bool hit = PaperHit(localX, localY, -11.9f, 0.45f, 11.9f, 11.55f, 2.7f);
                *bitmap++ = StickerPixel(sticker, (x + .5f) / size.cx, (row + .5f) / size.cy, hit);
            }
        }
    }
}

} // namespace

void IconLatch::Show(HINSTANCE module, HWND bar, HWND actions, IconGroup group,
                     const RECT& anchor, UINT dpi, const RECT& monitor, bool corner, bool pinned) {
    const LONG width = MulDiv(corner ? cornerWidth : 26, dpi, 96);
    const LONG height = MulDiv(corner ? cornerHeight : latchHeight, dpi, 96);
    if (width <= 0 || height <= 0 || width > monitor.right - monitor.left || height > monitor.bottom - monitor.top)
        return;
    const LONG overlap = MulDiv(5, dpi, 96);
    const LONG above = anchor.top - height + overlap;
    const bool below = !corner && above < monitor.top;
    const LONG centerX = anchor.left + (anchor.right - anchor.left) / 2;
    const LONG x = std::clamp(centerX - width / 2, monitor.left, monitor.right - width);
    const LONG y = std::clamp(below ? anchor.bottom - overlap : above, monitor.top, monitor.bottom - height);
    RECT next{x, y, x + width, y + height};
    RECT gripClearance{};
    if (corner) {
        const LONG centerY = anchor.top + (anchor.bottom - anchor.top) / 2;
        const LONG radius = std::max(1, MulDiv(8, dpi, 96));
        const RECT center{centerX - radius, centerY - radius, centerX + radius, centerY + radius};
        IntersectRect(&gripClearance, &center, &anchor);
        const LONG wantedX = centerX - MulDiv(12, dpi, 96);
        const LONG paperX = std::max(wantedX, monitor.left + MulDiv(25, dpi, 96));
        // Keep the paper registered to the icon if an empty margin extends beyond the screen.
        const LONG paperY = centerY - MulDiv(10, dpi, 96) - (paperX - wantedX) / 3;
        const LONG left = paperX - width / 2;
        const LONG top = paperY - height / 2;
        next = {left, top, left + width, top + height};
        if (IntersectRect(&gripClearance, &gripClearance, &next))
            OffsetRect(&gripClearance, -left, -top);
    }
    if (!window_) {
        WNDCLASSW spec{};
        spec.hInstance = module;
        spec.lpfnWndProc = Proc;
        spec.hCursor = LoadCursorW(nullptr, IDC_HAND);
        spec.lpszClassName = latchClass;
        if (!RegisterClassW(&spec) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        actions_ = actions;
        group_ = group;
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, latchClass, L"",
                                  WS_POPUP, 0, 0, 0, 0, bar, nullptr, module, this);
        if (!window_) return;
    }
    if (visible_ && !IsWindowVisible(window_)) visible_ = false;
    const bool pinChanged = pinned != pinned_;
    const bool rename = pinChanged || !visible_;
    const bool newSurface = corner != corner_ || !visible_;
    const bool repaint = dpi != dpi_ || newSurface || below != below_ || rename ||
                         !EqualRect(&gripClearance_, &gripClearance);
    if (repaint) {
        dpi_ = dpi;
        corner_ = corner;
        below_ = below;
        gripClearance_ = gripClearance;
        if (pinChanged) flattenUntilLeave_ = pinned && hot_;
        pinned_ = pinned;
        if (newSurface && !removing_) {
            StopPeel();
            peel_ = PeelTarget();
        }
        if (!Present(next)) {
            Hide();
            return;
        }
    } else if ((bounds_.left != next.left || bounds_.top != next.top) &&
               !SetWindowPos(window_, nullptr, next.left, next.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER)) {
        return;
    }
    bounds_ = next;
    if (rename) SetWindowTextW(window_, pinned ? L"松开图标组" : L"固定图标组");
    if (!visible_)
        visible_ = SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0,
                                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW) != FALSE;
    if (rename && !newSurface) RetargetPeel();
}

void IconLatch::Hide() {
    StopPeel();
    if (down_ || removing_) {
        down_ = false;
        removing_ = false;
        if (GetCapture() == window_) ReleaseCapture();
        Send(LatchGesture::peelEnd);
    }
    if (window_ && visible_) ShowWindow(window_, SW_HIDE);
    visible_ = false;
    hot_ = false;
    flattenUntilLeave_ = false;
}

void IconLatch::Destroy() {
    if (window_) DestroyWindow(window_);
    window_ = nullptr;
}

void IconLatch::PeelOff() {
    if (removing_) return;
    removing_ = true;
    if (visible_) RetargetPeel();
    else FinishRemoval();
}

void IconLatch::KeepAbove(HWND bar) const {
    if (!bar || !window_ || !visible_) return;
    const HWND higher = GetWindow(bar, GW_HWNDPREV);
    HWND peer = higher;
    for (int remaining = 512; peer && remaining--; peer = GetWindow(peer, GW_HWNDPREV)) {
        if (peer == window_) return;
    }
    const HWND after = higher && (GetWindowLongPtrW(higher, GWL_EXSTYLE) & WS_EX_TOPMOST) ? higher : HWND_TOPMOST;
    SetWindowPos(window_, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

bool IconLatch::ContainsPointer(POINT pointer) const {
    return visible_ && PtInRect(&bounds_, pointer) != FALSE;
}

void IconLatch::Send(LatchGesture gesture) const {
    const WPARAM tag = MAKEWPARAM(static_cast<UINT>(group_), static_cast<UINT>(gesture));
    PostMessageW(actions_, latchMessage, tag, 0);
}

LRESULT CALLBACK IconLatch::Proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        auto* latch = static_cast<IconLatch*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        latch->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(latch));
    }
    const auto latch = reinterpret_cast<IconLatch*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return latch ? latch->Handle(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
}

LRESULT IconLatch::Handle(UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SHOWWINDOW:
        visible_ = wp != FALSE;
        return DefWindowProcW(window_, message, wp, lp);
    case WM_TIMER:
        if (wp == peelTimer) StepPeel();
        return 0;
    case WM_LBUTTONDOWN: {
        if (down_ || removing_) return 0;
        SetCapture(window_);
        if (GetCapture() != window_) return 0;
        const DWORD point = GetMessagePos();
        peelStart_ = {GET_X_LPARAM(point), GET_Y_LPARAM(point)};
        peelPull_ = 0;
        Hover(true);
        down_ = true;
        dragged_ = false;
        Send(LatchGesture::peelStart);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (down_) {
            const DWORD point = GetMessagePos();
            PullPaper({GET_X_LPARAM(point), GET_Y_LPARAM(point)});
        } else {
            Hover(true);
        }
        return 0;
    case WM_MOUSELEAVE:
        Hover(false);
        return 0;
    case WM_LBUTTONUP: {
        if (down_) {
            const DWORD point = GetMessagePos();
            PullPaper({GET_X_LPARAM(point), GET_Y_LPARAM(point)});
        }
        EndPress(true);
        return 0;
    }
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        EndPress(false);
        return 0;
    case WM_NCDESTROY: {
        const HWND window = window_;
        StopPeel();
        if (down_ || removing_) Send(LatchGesture::peelEnd);
        window_ = nullptr;
        bounds_ = {};
        visible_ = false;
        hot_ = false;
        flattenUntilLeave_ = false;
        down_ = false;
        removing_ = false;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wp, lp);
    }
    default:
        return DefWindowProcW(window_, message, wp, lp);
    }
}

void IconLatch::PullPaper(POINT pointer) {
    const float distance = std::hypot(static_cast<float>(pointer.x - peelStart_.x),
                                      static_cast<float>(pointer.y - peelStart_.y));
    if (!dragged_ && distance < MulDiv(3, dpi_, 96)) return;
    const float pull = std::clamp(distance * 96.0f / dpi_ / 27.0f, 0.0f, 1.0f);
    if (dragged_ && std::abs(pull - peelPull_) < .015f) return;
    dragged_ = true;
    peelPull_ = pull;
    RetargetPeel();
}

void IconLatch::EndPress(bool released) {
    if (!down_) return;
    down_ = false;
    if (GetCapture() == window_) ReleaseCapture();
    RetargetPeel();
    if (released) Send(dragged_ ? LatchGesture::peelRemove : LatchGesture::toggle);
    else Send(LatchGesture::peelEnd);
}

void IconLatch::Hover(bool hot) {
    if (hot == hot_) return;
    if (hot) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
        if (!TrackMouseEvent(&tracking)) return;
    } else {
        flattenUntilLeave_ = false;
    }
    hot_ = hot;
    RetargetPeel();
}

double IconLatch::PeelTarget() const {
    if (removing_) return 1.0;
    if (down_ && dragged_) return 0.57 + 0.43 * peelPull_;
    if (pinned_) return hot_ && !flattenUntilLeave_ ? pinnedHoverPeel : 0.0;
    return hot_ ? looseHoverPeel : loosePeel;
}

void IconLatch::RetargetPeel() {
    if (!visible_ || peelTick_ || (!removing_ && peel_ == PeelTarget())) return;
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    if (animations && SetTimer(window_, peelTimer, 12, nullptr)) {
        peelTick_ = GetTickCount64();
    } else if (removing_) {
        FinishRemoval();
    } else {
        StopPeel();
        peel_ = PeelTarget();
        Present(bounds_);
    }
}

void IconLatch::StepPeel() {
    if (!peelTick_) return;
    const auto now = GetTickCount64();
    const double target = PeelTarget();
    peelSpring.Advance(peel_, peelVelocity_, target, (now - peelTick_) / 1000.0);
    peelTick_ = now;
    if (removing_ && peel_ >= 0.995) {
        FinishRemoval();
        return;
    }
    if (std::abs(peel_ - target) < 0.006 && std::abs(peelVelocity_) < 0.14) {
        peel_ = target;
        StopPeel();
    }
    Present(bounds_);
}

void IconLatch::StopPeel() {
    if (window_ && peelTick_) KillTimer(window_, peelTimer);
    peelTick_ = 0;
    peelVelocity_ = 0;
}

void IconLatch::FinishRemoval() {
    removing_ = false;
    Hide();
    Send(LatchGesture::peelRemove);
}

bool IconLatch::Present(const RECT& bounds) const {
    static const auto* atlas = ReadStickerAtlas();
    if (!atlas) return false;
    SIZE size{bounds.right - bounds.left, bounds.bottom - bounds.top};
    if (size.cx <= 0 || size.cy <= 0) return false;
    const HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size.cx;
    info.bmiHeader.biHeight = -size.cy;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    bool painted = false;
    if (bitmap) {
        if (const HGDIOBJ previous = SelectObject(dc, bitmap)) {
            const auto peel = static_cast<float>(std::clamp(peel_, 0.0, 1.0));
            Rasterize(static_cast<std::uint32_t*>(pixels), size, atlas, below_, peel, corner_, gripClearance_);
            POINT origin{bounds.left, bounds.top};
            POINT source{};
            const float alpha = removing_ ? std::min(1.0f, (1.0f - peel) / removalFadeSpan) : 1.0f;
            BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(255 * alpha)), AC_SRC_ALPHA};
            painted = UpdateLayeredWindow(window_, nullptr, &origin, &size, dc, &source, 0, &blend, ULW_ALPHA) != FALSE;
            SelectObject(dc, previous);
        }
        DeleteObject(bitmap);
    }
    DeleteDC(dc);
    return painted;
}

} // namespace fullwintb::appearance

#include "ink_sampling.h"

#include <cstring>

namespace fullwintb::appearance {
namespace {

bool ReadGrid(HDC screen, const RECT& bounds, TaskbarInkGrid& pixels) {
    constexpr int gridSide = taskbarInkGridSide;
    const HDC memory = CreateCompatibleDC(screen);
    if (!memory) return false;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = gridSide;
    info.bmiHeader.biHeight = -gridSide;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    const HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    const HGDIOBJ previous = bitmap ? SelectObject(memory, bitmap) : nullptr;
    const bool selected = previous && previous != HGDI_ERROR;
    // One screen read avoids synchronizing with the compositor for each pixel.
    const bool read = selected && SetStretchBltMode(memory, COLORONCOLOR) &&
        StretchBlt(memory, 0, 0, gridSide, gridSide, screen, bounds.left, bounds.top,
                   bounds.right - bounds.left, bounds.bottom - bounds.top, SRCCOPY) && GdiFlush();
    if (read) std::memcpy(pixels.data(), bits, sizeof(pixels));
    if (selected) SelectObject(memory, previous);
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(memory);
    return read;
}

Ink ReadInk(HDC screen, const RECT& bounds, bool horizontal) {
    TaskbarInkGrid pixels{};
    return ReadGrid(screen, bounds, pixels) ? ClassifyTaskbarInk(pixels, horizontal) : Ink::system;
}

LONG Offset(HWND bar, const wchar_t* property) {
    return static_cast<LONG>(reinterpret_cast<LONG_PTR>(GetPropW(bar, property)));
}

} // namespace

Ink ClassifyTaskbarInk(const TaskbarInkGrid& pixels, bool horizontal) {
    constexpr int gridSide = taskbarInkGridSide;
    // Window borders can occupy the outermost rows. The middle of the
    // strip contains the taskbar glyphs, including the ones we recolor.
    constexpr std::array rows{1, 2, 3, gridSide - 4, gridSide - 3, gridSide - 2};
    InkSamples samples;
    for (const int row : rows) {
        for (int column = 1; column < gridSide - 1; ++column) {
            const int x = horizontal ? column : row;
            const int y = horizontal ? row : column;
            const auto& color = pixels[y * gridSide + x];
            samples.Add((54u * color.rgbRed + 183u * color.rgbGreen + 19u * color.rgbBlue) >> 8);
        }
    }
    return samples.Preferred();
}

void InkSamples::Add(unsigned brightness) {
    ++count_;
    if (brightness <= 105) ++dark_;
    else if (brightness >= 145) ++light_;
}

Ink InkSamples::Preferred() const {
    // A median can jump across the entire grayscale on a mixed background.
    // Require agreement across most of the strip before choosing a shade.
    if (!count_) return Ink::system;
    const unsigned minimum = count_ - count_ / 4;
    if (dark_ >= minimum) return Ink::white;
    if (light_ >= minimum) return Ink::black;
    return Ink::system;
}

bool AutomaticInk::Observe(Ink wanted, ULONGLONG now) {
    constexpr ULONGLONG stableFor = 100;
    if (wanted == Ink::system || wanted == current_) {
        Pause();
        return false;
    }
    if (wanted != pending_) {
        pending_ = wanted;
        pendingSince_ = now;
        return false;
    }
    if (now - pendingSince_ < stableFor) return false;
    current_ = wanted;
    Pause();
    return true;
}

TaskbarInkSample ReadTaskbarInk(HWND bar, const RECT& monitor, bool horizontal) {
    TaskbarInkSample ink;
    RECT bounds{}, dock{};
    if (!GetWindowRect(bar, &bounds) || !ReadDockBounds(bar, dock) ||
        !IntersectRect(&bounds, &bounds, &monitor)) return ink;

    const HDC screen = GetDC(nullptr);
    if (!screen) return ink;
    const auto along = horizontal ? MotionAxis::horizontal : MotionAxis::vertical;
    const auto across = OtherAxis(along);
    RECT notification{};
    if (const HWND tray = FindWindowExW(bar, nullptr, L"TrayNotifyWnd", nullptr);
        tray && GetWindowRect(tray, &notification)) {
        if (GetPropW(bar, DockThicknessProperty(across))) {
            AxisStart(notification, across) = AxisStart(dock, across);
            AxisEnd(notification, across) = AxisEnd(dock, across);
        }
        OffsetRect(&notification, Offset(bar, notificationOffsetX), Offset(bar, notificationOffsetY));
        if (IntersectRect(&notification, &notification, &bounds))
            ink.notification = ReadInk(screen, notification, horizontal);
    }
    const auto span = reinterpret_cast<UINT_PTR>(GetPropW(bar, appsInkSpanProperty));
    if (span) {
        RECT apps = dock;
        AxisStart(apps, along) += LOWORD(span);
        AxisEnd(apps, along) = AxisStart(dock, along) + HIWORD(span);
        OffsetRect(&apps, Offset(bar, appsOffsetX), Offset(bar, appsOffsetY));
        if (IntersectRect(&apps, &apps, &bounds))
            ink.apps = ReadInk(screen, apps, horizontal);
    }
    ReleaseDC(nullptr, screen);
    return ink;
}

} // namespace fullwintb::appearance

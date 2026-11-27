#include "ink_sampling.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace fullwintb::appearance {
namespace {

constexpr int gridSide = 16;
using PixelGrid = std::array<RGBQUAD, gridSide * gridSide>;

bool ReadGrid(HDC screen, const RECT& bounds, PixelGrid& pixels) {
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

} // namespace

bool AutomaticInk::Observe(unsigned brightness) {
    Ink wanted = current_;
    if (brightness <= 105) wanted = Ink::white;
    else if (brightness >= 145) wanted = Ink::black;
    else if (current_ == Ink::system) wanted = brightness < 125 ? Ink::white : Ink::black;

    if (wanted == current_) {
        Pause();
        return false;
    }
    if (wanted != pending_) {
        pending_ = wanted;
        return false;
    }
    current_ = wanted;
    Pause();
    return true;
}

std::optional<unsigned> ReadTaskbarBrightness(HWND bar, const RECT& monitor, bool horizontal) {
    RECT bounds{}, dock{}, notification{}, area{};
    const HWND tray = FindWindowExW(bar, nullptr, L"TrayNotifyWnd", nullptr);
    if (!tray || !GetWindowRect(bar, &bounds) || !ReadDockBounds(bar, dock) ||
        !GetWindowRect(tray, &notification)) return std::nullopt;
    const auto offset = static_cast<LONG>(reinterpret_cast<LONG_PTR>(GetPropW(bar, notificationOffsetProperty)));
    const bool vertical = GetPropW(bar, dockHeightProperty) != nullptr;
    if (vertical) {
        notification.top = dock.top;
        notification.bottom = dock.bottom;
    }
    OffsetRect(&notification, vertical ? 0 : offset, vertical ? offset : 0);
    if (!IntersectRect(&area, &bounds, &notification) || !IntersectRect(&bounds, &area, &monitor))
        return std::nullopt;

    // Most of these positions fall between the clock, glyphs and badges.
    constexpr std::array rowFractions{10, 25, 88};
    constexpr int columns = 7;
    std::array<unsigned, rowFractions.size() * columns> samples{};
    std::size_t count = 0;
    const HDC screen = GetDC(nullptr);
    if (!screen) return std::nullopt;
    PixelGrid pixels{};
    const bool hasGrid = ReadGrid(screen, bounds, pixels);
    const LONG width = bounds.right - bounds.left;
    const LONG height = bounds.bottom - bounds.top;
    for (int row : rowFractions) {
        for (int column = 1; column <= columns; ++column) {
            const int across = column * 100 / (columns + 1);
            const int x = horizontal ? across : row;
            const int y = horizontal ? row : across;
            RGBQUAD color{};
            if (hasGrid) {
                color = pixels[(y * gridSide / 100) * gridSide + x * gridSide / 100];
            } else {
                const COLORREF pixel = GetPixel(screen, bounds.left + width * x / 100,
                                                 bounds.top + height * y / 100);
                if (pixel == CLR_INVALID) continue;
                color = RGBQUAD{GetBValue(pixel), GetGValue(pixel), GetRValue(pixel), 0};
            }
            samples[count++] = (54u * color.rgbRed + 183u * color.rgbGreen + 19u * color.rgbBlue) >> 8;
        }
    }
    ReleaseDC(nullptr, screen);
    if (count < samples.size() / 2) return std::nullopt;
    const auto median = samples.begin() + count / 2;
    std::nth_element(samples.begin(), median, samples.begin() + count);
    return *median;
}

} // namespace fullwintb::appearance

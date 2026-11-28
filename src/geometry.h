#pragma once

#include <windows.h>
#include <algorithm>
#include <optional>

namespace fullwintb {

enum class Edge { left, top, right, bottom };
enum class WindowMode { windowed, maximized, fullscreen };

inline bool SameRect(const RECT& a, const RECT& b) {
    return EqualRect(&a, &b) != FALSE;
}

inline bool Overlaps(const RECT& a, const RECT& b) {
    return std::max(a.left, b.left) < std::min(a.right, b.right) &&
           std::max(a.top, b.top) < std::min(a.bottom, b.bottom);
}

inline bool Near(LONG a, LONG b, LONG tolerance = 2) {
    return a >= b - tolerance && a <= b + tolerance;
}

inline bool Fits(const RECT& frame, const RECT& area, SIZE border) {
    return Near(frame.left, area.left, border.cx + 2) &&
           Near(frame.right, area.right, border.cx + 2) &&
           Near(frame.top, area.top, border.cy + 2) &&
           Near(frame.bottom, area.bottom, border.cy + 2);
}

inline bool Covers(const RECT& frame, const RECT& screen) {
    return frame.left <= screen.left + 2 && frame.top <= screen.top + 2 &&
           frame.right >= screen.right - 2 && frame.bottom >= screen.bottom - 2;
}

inline WindowMode ClassifyWindow(const RECT& frame, const RECT& client, const RECT& screen,
                                 DWORD style, bool optsOutOfFullscreen) {
    const bool maximized = (style & WS_MAXIMIZE) != 0;
    const bool framed = (style & WS_CAPTION) == WS_CAPTION || (style & WS_THICKFRAME);
    // A fullscreen transition can retain WS_MAXIMIZE while removing the frame.
    if (!optsOutOfFullscreen && (!maximized || !framed) && Covers(frame, screen) && Covers(client, screen))
        return WindowMode::fullscreen;
    return maximized ? WindowMode::maximized : WindowMode::windowed;
}

inline std::optional<Edge> TaskbarEdge(const RECT& bar, const RECT& screen) {
    if (!Overlaps(bar, screen) || Covers(bar, screen)) return std::nullopt;
    if (Near(bar.left, screen.left) && Near(bar.right, screen.right)) {
        if (Near(bar.bottom, screen.bottom)) return Edge::bottom;
        if (Near(bar.top, screen.top)) return Edge::top;
    }
    if (Near(bar.top, screen.top) && Near(bar.bottom, screen.bottom)) {
        if (Near(bar.left, screen.left)) return Edge::left;
        if (Near(bar.right, screen.right)) return Edge::right;
    }
    return std::nullopt;
}

inline RECT ExpandedWork(const RECT& screen, RECT work, const RECT& bar, Edge edge) {
    switch (edge) {
    case Edge::left:
        if (work.left > screen.left && Near(work.left, bar.right))
            work.left = screen.left;
        break;
    case Edge::top:
        if (work.top > screen.top && Near(work.top, bar.bottom))
            work.top = screen.top;
        break;
    case Edge::right:
        if (work.right < screen.right && Near(work.right, bar.left))
            work.right = screen.right;
        break;
    case Edge::bottom:
        if (work.bottom < screen.bottom && Near(work.bottom, bar.top))
            work.bottom = screen.bottom;
        break;
    }
    return work;
}

inline RECT TransferBounds(const RECT& outer, const RECT& from, const RECT& to) {
    return {outer.left + to.left - from.left, outer.top + to.top - from.top,
            outer.right + to.right - from.right, outer.bottom + to.bottom - from.bottom};
}

struct PopupPlacement {
    LONG top;
    bool below;
};

inline constexpr RECT TransposeRect(const RECT& rect) {
    return {rect.top, rect.left, rect.bottom, rect.right};
}

inline PopupPlacement PlacePopup(const RECT& row, const RECT& surface, LONG height, LONG gap, bool preferBelow) {
    const LONG above = row.top - gap - height;
    const LONG below = row.bottom + gap;
    const LONG lastTop = surface.bottom - height;
    const LONG preferred = preferBelow ? below : above;
    const LONG other = preferBelow ? above : below;
    const auto fits = [&](LONG y) { return y >= surface.top && y <= lastTop; };
    if (fits(preferred)) return {preferred, preferBelow};
    if (fits(other)) return {other, !preferBelow};
    const LONG spaceAbove = row.top - surface.top;
    const LONG spaceBelow = surface.bottom - row.bottom;
    const bool opensBelow = spaceBelow == spaceAbove ? preferBelow : spaceBelow > spaceAbove;
    return {std::clamp(opensBelow ? below : above, surface.top, std::max(surface.top, lastTop)), opensBelow};
}

} // namespace fullwintb

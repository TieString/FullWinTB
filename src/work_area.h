#pragma once

#include "geometry.h"

namespace fullwintb {

// A journal belongs to one display in one sign-in session.
struct WorkArea {
    RECT monitor{};
    RECT original{};
    RECT expanded{};
    DWORD session = 0;

    bool Acquire(const MONITORINFOEXW& display, const RECT& desired);
    bool Restore(const MONITORINFOEXW& display, bool autoHide) const;
    bool Owned() const { return !SameRect(original, expanded); }

    static void Recover();
    static bool TaskbarAutoHides();
};

inline bool MonitorInfo(HMONITOR monitor, MONITORINFOEXW& info) {
    info.cbSize = sizeof(info);
    return GetMonitorInfoW(monitor, &info) != FALSE;
}

} // namespace fullwintb

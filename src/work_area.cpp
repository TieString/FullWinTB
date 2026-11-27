#include "work_area.h"

#include <shellapi.h>

namespace fullwintb {
namespace {

// Recover the work area even if an earlier release stopped while it owned it.
constexpr wchar_t journalKey[] = L"Software\\AuraBar\\WorkAreas";

DWORD SessionId() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return session;
}

void ClearJournal(const wchar_t* display) {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, journalKey, display);
}

bool SetWorkArea(RECT area) {
    return SystemParametersInfoW(SPI_SETWORKAREA, 0, &area, 0) != FALSE;
}

bool Contains(const RECT& outside, const RECT& inside) {
    return inside.left >= outside.left && inside.top >= outside.top &&
           inside.right <= outside.right && inside.bottom <= outside.bottom &&
           inside.left < inside.right && inside.top < inside.bottom;
}

BOOL CALLBACK RecoverMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM autoHide) {
    MONITORINFOEXW display{};
    if (!MonitorInfo(monitor, display)) return TRUE;
    WorkArea saved;
    DWORD size = sizeof(saved);
    if (RegGetValueW(HKEY_CURRENT_USER, journalKey, display.szDevice, RRF_RT_REG_BINARY,
                     nullptr, &saved, &size) == ERROR_SUCCESS && size == sizeof(saved)) {
        if (!saved.Owned() || !Contains(saved.expanded, saved.original) ||
            !Contains(saved.monitor, saved.expanded)) {
            ClearJournal(display.szDevice);
        } else {
            saved.Restore(display, autoHide != 0);
        }
    }
    return TRUE;
}

} // namespace

bool WorkArea::Acquire(const MONITORINFOEXW& display, const RECT& desired) {
    if (SameRect(display.rcWork, desired)) return false;
    const WorkArea record{display.rcMonitor, display.rcWork, desired, SessionId()};
    if (RegSetKeyValueW(HKEY_CURRENT_USER, journalKey, display.szDevice, REG_BINARY,
                       &record, sizeof(record)) != ERROR_SUCCESS) return false;
    if (!SetWorkArea(desired)) {
        ClearJournal(display.szDevice);
        return false;
    }
    *this = record;
    return true;
}

bool WorkArea::Restore(const MONITORINFOEXW& display, bool autoHide) const {
    if (!Owned()) return false;
    if (autoHide) {
        ClearJournal(display.szDevice);
        return false;
    }
    const bool sameDisplay = session == SessionId() && SameRect(display.rcMonitor, monitor);
    const bool stillOurs = sameDisplay && SameRect(display.rcWork, expanded);
    if (stillOurs && !SetWorkArea(original)) return false;
    ClearJournal(display.szDevice);
    return stillOurs || (sameDisplay && SameRect(display.rcWork, original));
}

void WorkArea::Recover() {
    EnumDisplayMonitors(nullptr, nullptr, RecoverMonitor, TaskbarAutoHides());
}

bool WorkArea::TaskbarAutoHides() {
    APPBARDATA request{sizeof(APPBARDATA)};
    request.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    return (SHAppBarMessage(ABM_GETSTATE, &request) & ABS_AUTOHIDE) != 0;
}

} // namespace fullwintb

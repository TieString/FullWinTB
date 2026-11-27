"""Interactive Windows regression using its own windows and an optional snip overlay."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time
import winreg


user = c.windll.user32
shell = c.windll.shell32
dwm = c.windll.dwmapi
gdi = c.windll.gdi32
kernel = c.windll.kernel32
user.SetProcessDpiAwarenessContext(c.c_void_p(-4))
user.GetForegroundWindow.restype = w.HWND
user.PostMessageW.argtypes = w.HWND, w.UINT, w.WPARAM, w.LPARAM
user.GetWindow.argtypes = w.HWND, w.UINT
user.GetWindow.restype = w.HWND
user.FindWindowW.restype = w.HWND
user.FindWindowExW.argtypes = w.HWND, w.HWND, w.LPCWSTR, w.LPCWSTR
user.FindWindowExW.restype = w.HWND
user.WindowFromPoint.argtypes = (w.POINT,)
user.WindowFromPoint.restype = w.HWND
user.GetAncestor.restype = w.HWND
user.GetDC.restype = w.HDC
user.GetPropW.restype = w.HANDLE
user.CreateWindowExW.restype = w.HWND
user.DefWindowProcW.argtypes = w.HWND, w.UINT, w.WPARAM, w.LPARAM
user.DefWindowProcW.restype = w.LPARAM
gdi.CreateSolidBrush.restype = w.HBRUSH
gdi.CreateRectRgn.restype = w.HRGN
shell.ShellExecuteW.restype = w.HINSTANCE
kernel.GetModuleHandleW.restype = w.HMODULE
kernel.OpenProcess.restype = w.HANDLE


def wait_for(condition, seconds=3):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if result := condition():
            return result
        time.sleep(0.025)
    return condition()


def physical_bounds(hwnd, visible_frame=False):
    rect = w.RECT()
    if visible_frame:
        dwm.DwmGetWindowAttribute(hwnd, 9, c.byref(rect), c.sizeof(rect))
    else:
        user.GetWindowRect(hwnd, c.byref(rect))
    return rect.left, rect.top, rect.right, rect.bottom


def dock_bounds(hwnd):
    left, top, right, bottom = physical_bounds(hwnd)
    height = c.c_ssize_t(user.GetPropW(hwnd, "FullWinTB.DockHeight") or 0).value
    if height > 0:
        top = bottom - height
    elif height < 0:
        bottom = top - height
    return left, top, right, bottom


def work_area():
    rect = w.RECT()
    user.SystemParametersInfoW(0x30, 0, c.byref(rect), 0)
    return rect.left, rect.top, rect.right, rect.bottom


def root_at(x, y):
    return user.GetAncestor(user.WindowFromPoint(w.POINT(x, y)), 2)


def window_process(hwnd):
    pid = w.DWORD()
    if hwnd:
        user.GetWindowThreadProcessId(hwnd, c.byref(pid))
    return pid.value


def visible(hwnd):
    if not hwnd or not user.IsWindowVisible(hwnd):
        return False
    cloaked = w.DWORD()
    dwm.DwmGetWindowAttribute(hwnd, 14, c.byref(cloaked), c.sizeof(cloaked))
    return cloaked.value == 0


def covers_display(hwnd):
    left, top, right, bottom = physical_bounds(hwnd)
    return left <= 0 and top <= 0 and right >= user.GetSystemMetrics(0) and bottom >= user.GetSystemMetrics(1)


def top_level_windows():
    result = []
    callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)

    @callback
    def collect(window, _):
        result.append(window)
        return True

    user.EnumWindows(collect, 0)
    return result


def clipping_windows():
    for window in top_level_windows():
        if not visible(window) or not covers_display(window):
            continue
        name = c.create_unicode_buffer(64)
        user.GetClassNameW(window, name, len(name))
        if name.value in ("SnipOverlayRootWindow", "ScreenClippingHost") or (
                name.value == "XamlWindow" and user.GetWindowLongW(window, -20) & 0x80):
            if process_name(window) in ("snippingtool.exe", "screenclippinghost.exe"):
                yield window


def snip_window():
    return next(clipping_windows(), None)


def desktop_locked():
    foreground = user.GetForegroundWindow()
    if process_name(foreground) == "lockapp.exe":
        return True
    hit = root_at(user.GetSystemMetrics(0) // 2, user.GetSystemMetrics(1) // 2)
    name = c.create_unicode_buffer(64)
    user.GetClassNameW(hit, name, len(name))
    return name.value == "LockScreenBackstopFrame"


def cancel_snip(capture):
    foreground = user.GetForegroundWindow()
    pid = window_process(capture)
    if not pid:
        return
    if foreground and window_process(foreground) == pid:
        scan = user.MapVirtualKeyW(0x1B, 0)
        user.keybd_event(0x1B, scan, 0, 0)
        user.keybd_event(0x1B, scan, 2, 0)
    else:
        user.PostMessageW(capture, 0x100, 0x1B, 0)
        user.PostMessageW(capture, 0x101, 0x1B, 0)
    if process_name(capture) in ("snippingtool.exe", "screenclippinghost.exe"):
        def surfaces():
            return [window for window in clipping_windows() if window_process(window) == pid]
        if not wait_for(lambda: not surfaces(), .75):
            for window in surfaces():
                user.PostMessageW(window, 0x10, 0, 0)


def above(window, other):
    for _ in range(512):
        other = user.GetWindow(other, 3)
        if not other or other == window:
            return other == window
    return False


def desktop_pixel(x, y):
    dc = user.GetDC(None)
    sample = gdi.GetPixel(w.HDC(dc), x, y)
    user.ReleaseDC(None, w.HDC(dc))
    return sample & 0xFFFFFF


def taskbar_is_light():
    path = r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as key:
            return winreg.QueryValueEx(key, "SystemUsesLightTheme")[0] != 0
    except FileNotFoundError:
        return True


def ink_preference():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\FullWinTB") as key:
            return winreg.QueryValueEx(key, "TaskbarInk")[0]
    except FileNotFoundError:
        return 0


def gap_near_tray(bar, start, tray):
    left, top, _, bottom = physical_bounds(bar)
    start_x = physical_bounds(start)[0] - left
    tray_x = physical_bounds(tray)[0] - left
    y = (bottom - top) // 2
    region = gdi.CreateRectRgn(0, 0, 0, 0)
    try:
        if not user.GetWindowRgn(bar, w.HRGN(region)):
            return None
        count = gdi.GetRegionData(w.HRGN(region), 0, None)
        if count <= 32:
            return None
        buffer = c.create_string_buffer(count)
        gdi.GetRegionData(w.HRGN(region), count, buffer)
        rects = [r for r in struct.iter_unpack("<4i", buffer.raw[32:]) if r[1] <= y < r[3]]
        for a, b in zip(rects, rects[1:]):
            if a[2] >= start_x and b[0] - a[2] > 10 and abs(b[0] - tray_x) <= 12:
                return left + (a[2] + b[0]) // 2, top + y, b[0] - a[2]
        return None
    finally:
        gdi.DeleteObject(w.HRGN(region))


def glyph_pixels(bar):
    _, top, right, bottom = physical_bounds(bar)
    dc = user.GetDC(None)
    white = black = 0
    try:
        for y in range(top + 10, bottom - 10, 2):
            for x in range(right - 205, right - 47, 2):
                color = gdi.GetPixel(w.HDC(dc), x, y) & 0xFFFFFF
                parts = [color >> bit & 255 for bit in (0, 8, 16)]
                white += min(parts) >= 222
                black += max(parts) <= 62
        return white, black
    finally:
        user.ReleaseDC(None, w.HDC(dc))


def taskbar_points():
    command = ["pwsh", "-NoProfile", "-NonInteractive", "-File",
               str(Path(__file__).with_name("taskbar_points.ps1"))]
    try:
        return json.loads(subprocess.check_output(command, encoding="utf-8", timeout=10,
                                                  creationflags=0x08000000))
    except (subprocess.SubprocessError, OSError, ValueError):
        return {}


def click_taskbar(tray, point, right=False):
    if not point or root_at(*point) != tray:
        return False
    cursor = w.POINT()
    user.GetCursorPos(c.byref(cursor))
    user.SetCursorPos(*point)
    if right:
        time.sleep(.08)
    down, up = (8, 16) if right else (2, 4)
    user.mouse_event(down, 0, 0, 0, 0)
    user.mouse_event(up, 0, 0, 0, 0)
    if right:
        time.sleep(.12)
    user.SetCursorPos(cursor.x, cursor.y)
    return True


def check_vertical_movement(tray, host, main, check):
    dock = dock_bounds(tray)
    original_frame = physical_bounds(main, True)
    original_work = work_area()
    bridge = user.FindWindowExW(tray, None, "Windows.UI.Composition.DesktopWindowContentBridge", None)
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\FullWinTB") as key:
            direction = winreg.QueryValueEx(key, "MotionAxis")[0]
    except FileNotFoundError:
        direction = 0
    cursor = w.POINT()
    user.GetCursorPos(c.byref(cursor))

    def app_offset():
        return c.c_ssize_t(user.GetPropW(tray, "FullWinTB.AppsOffset") or 0).value

    try:
        user.PostMessageW(host, 0x111, 210, 0)
        user.PostMessageW(host, 0x111, 208, 0)
        time.sleep(.35)
        point = taskbar_points().get("main")
        enabled = point and root_at(*point) == tray
        if enabled:
            user.SetCursorPos(*point)
            time.sleep(.06)
            wheel = -240 if dock[1] <= original_work[1] + 2 else 240
            user.mouse_event(0x0800, 0, 0, w.DWORD(wheel), 0)
        moved = enabled and wait_for(lambda: abs(app_offset()) > 24, .7)
        check("The vertical viewport opens with the native window region installed", moved, app_offset())
        if not moved:
            return
        viewport = physical_bounds(tray)

        time.sleep(.2)
        latch = next((h for h in top_level_windows() if visible(h) and
                      class_name(h).startswith("FullWinTB.IconLatch.")), None)
        if latch:
            left, top, right, bottom = physical_bounds(latch)
            dpi = user.GetDpiForWindow(w.HWND(latch))
            check("The slim sticker keeps its transparent corner clickable",
                  bottom - top == (24 * dpi + 48) // 96 and right - left == (38 * dpi + 48) // 96 and
                  root_at(left, top) != latch and root_at(*start_point(latch)) == latch)
        check("The vertical sticker accepts clicks", latch and click_taskbar(latch, start_point(latch)))
        displaced = taskbar_points().get("main")
        offset = app_offset()
        check("The real app button and its hit area move together",
              displaced and offset and abs(displaced[1] - point[1] - offset) <= 2 and
              root_at(*displaced) == tray and root_at(*point) == main, (point, displaced, offset))
        row = dock[1] + offset
        background = desktop_pixel(point[0], row - 4)
        samples = [desktop_pixel(point[0] + x, row + y)
                   for x in (-33, -24, 24, 33) for y in (3, 5, 7)
                   if root_at(point[0] + x, row + y) == tray]
        check("The shifted app strip shows the background through its gaps",
              len(samples) >= 6 and samples.count(background) >= len(samples) / 2)
        check("Raising the taskbar preserves the maximized window and work area",
              physical_bounds(main, True) == original_frame and work_area() == original_work)
        if latch and visible(latch):
            click_taskbar(latch, start_point(latch))
        check("The last released group returns to the original dock",
              wait_for(lambda: not app_offset() and dock_bounds(tray) == dock and
                       physical_bounds(tray) == viewport and physical_bounds(bridge) == viewport, 3.2))
    finally:
        user.PostMessageW(host, 0x111, 208, 0)
        wait_for(lambda: not app_offset() and dock_bounds(tray) == dock)
        user.PostMessageW(host, 0x111, 210 if direction == 1 else 209, 0)
        user.SetCursorPos(cursor.x, cursor.y)


def class_name(window):
    name = c.create_unicode_buffer(96)
    user.GetClassNameW(window, name, len(name))
    return name.value


def start_point(start):
    left, top, right, bottom = physical_bounds(start)
    return (left + right) // 2, (top + bottom) // 2


def trace_taskbar_hits(tray, start, seconds):
    deadline = time.monotonic() + seconds
    samples = []
    while time.monotonic() < deadline:
        samples.append(visible(tray) and root_at(*start_point(start)) == tray)
        time.sleep(.012)
    return samples


def process_name(hwnd):
    handle = kernel.OpenProcess(0x1000, False, window_process(hwnd))
    if not handle:
        return ""
    try:
        path = c.create_unicode_buffer(1024)
        size = w.DWORD(len(path))
        kernel.QueryFullProcessImageNameW(w.HANDLE(handle), 0, path, c.byref(size))
        return os.path.basename(path.value).lower()
    finally:
        kernel.CloseHandle(w.HANDLE(handle))


def launch_messenger_capture(keys, fixture):
    if user.GetForegroundWindow() != fixture:
        user.PostMessageW(fixture, 0x8004, fixture, 0)
    if not wait_for(lambda: user.GetForegroundWindow() == fixture, .75):
        return None
    existing = {window for window in top_level_windows() if visible(window) and covers_display(window)}
    for key in keys:
        user.keybd_event(key, user.MapVirtualKeyW(key, 0), 0, 0)
    for key in reversed(keys):
        user.keybd_event(key, user.MapVirtualKeyW(key, 0), 2, 0)
    def current_capture():
        for window in top_level_windows():
            if (window not in existing and visible(window) and covers_display(window) and
                    process_name(window) in ("qq.exe", "qqnt.exe", "weixin.exe", "wechat.exe", "qqscreenshot.exe")):
                return window
        return None
    return wait_for(current_capture, 1.5)


def fixture(title):
    wndproc_type = c.WINFUNCTYPE(w.LPARAM, w.HWND, w.UINT, w.WPARAM, w.LPARAM)

    class WindowClass(c.Structure):
        _fields_ = [
            ("style", w.UINT), ("proc", wndproc_type), ("class_extra", c.c_int),
            ("window_extra", c.c_int), ("instance", w.HINSTANCE), ("icon", w.HICON),
            ("cursor", w.HANDLE), ("brush", w.HBRUSH), ("menu", w.LPCWSTR),
            ("name", w.LPCWSTR),
        ]

    brush = gdi.CreateSolidBrush(0x111111)

    @wndproc_type
    def window_proc(hwnd, message, wp, lp):
        nonlocal brush
        if message == 2:
            user.PostQuitMessage(0)
            return 0
        if message == 0x14:
            rect = w.RECT()
            user.GetClientRect(hwnd, c.byref(rect))
            user.FillRect(w.HDC(wp), c.byref(rect), w.HBRUSH(brush))
            return 1
        if message == 0x8001:
            gdi.DeleteObject(w.HBRUSH(brush))
            brush = gdi.CreateSolidBrush(wp)
            user.RedrawWindow(hwnd, None, None, 0x0105)
            return 0
        if message == 0x8002:
            user.SetWindowLongW(hwnd, -16, 0x90000000 | (0x01000000 if wp & 2 else 0))
            user.SetWindowPos(hwnd, w.HWND(-2 if wp & 1 else -1), 0, 0, user.GetSystemMetrics(0),
                              user.GetSystemMetrics(1), 0x20)
            user.SetForegroundWindow(hwnd)
            return 0
        if message == 0x8003:
            user.SetWindowLongW(hwnd, -16, 0x90000000)
            if wp:
                user.SetWindowPos(hwnd, w.HWND(-2), 140, 120, user.GetSystemMetrics(0) - 280,
                                  user.GetSystemMetrics(1) - 120, 0x30)
            else:
                user.SetWindowLongW(hwnd, -16, 0x10CF0000)
                user.SetWindowPos(hwnd, None, 90, 150, 880, 640, 0x24)
            return 0
        if message == 0x8004:
            user.SetForegroundWindow(w.HWND(wp))
            return 0
        return user.DefWindowProcW(hwnd, message, wp, lp)

    shell.SetCurrentProcessExplicitAppUserModelID("FullWinTB.Regression." + title)
    instance = kernel.GetModuleHandleW(None)
    spec = WindowClass(3, window_proc, 0, 0, instance, None, None, None, None, "FullWinTB.Fixture")
    if not user.RegisterClassW(c.byref(spec)):
        raise c.WinError()
    user.CreateWindowExW.argtypes = (
        w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD, c.c_int, c.c_int, c.c_int, c.c_int,
        w.HWND, w.HMENU, w.HINSTANCE, w.LPVOID,
    )
    hwnd = user.CreateWindowExW(0x40000, spec.name, title, 0xCF0000, 90, 150, 880, 640,
                                None, None, instance, None)
    if not hwnd:
        raise c.WinError()
    user.ShowWindow(hwnd, 1 if title == "main" else 4)
    if title == "main":
        user.SetForegroundWindow(hwnd)
    message = w.MSG()
    while user.GetMessageW(c.byref(message), None, 0, 0) > 0:
        user.TranslateMessage(c.byref(message))
        user.DispatchMessageW(c.byref(message))
    gdi.DeleteObject(w.HBRUSH(brush))


def process_memory(process):
    class Counters(c.Structure):
        _fields_ = [("size", w.DWORD), ("faults", w.DWORD)] + [
            (name, c.c_size_t) for name in
            ("peak", "working", "peak_paged", "paged", "peak_np", "np", "page", "peak_page", "private")
        ]

    counters = Counters(c.sizeof(Counters))
    ph = w.HANDLE(int(process._handle))
    if c.windll.psapi.GetProcessMemoryInfo(ph, c.byref(counters), counters.size):
        print(f"Main process: private {counters.private / 2**20:.2f} MiB, "
              f"working set {counters.working / 2**20:.2f} MiB", flush=True)


def run(args):
    if desktop_locked():
        raise RuntimeError("Unlock the desktop before running the interactive tests")
    errors = []
    children = []

    def check(label, result, details=""):
        print(label, "PASS" if result else "FAIL", details, flush=True)
        if not result:
            errors.append(label)

    def start_window(title):
        process = subprocess.Popen([sys.executable, __file__, "--fixture", title],
                                   creationflags=0x08000000)
        children.append((process, None))
        hwnd = wait_for(lambda: user.FindWindowW("FullWinTB.Fixture", title))
        if not hwnd:
            raise RuntimeError(f"Fixture {title} failed to start")
        children[-1] = process, hwnd
        return hwnd

    running = user.FindWindowW("FullWinTB.Window", None)
    if running:
        if not args.replace_running:
            raise RuntimeError("Close the running FullWinTB or use --replace-running")
        if not user.PostMessageW(running, 0x10, 0, 0) or not wait_for(lambda: not user.IsWindow(running)):
            raise RuntimeError("The running FullWinTB did not exit")
        time.sleep(.3)

    baseline = work_area()
    light_theme = taskbar_is_light()
    wanted_ink = ink_preference()
    tray = user.FindWindowW("Shell_TrayWnd", None)
    tray_bottom = dock_bounds(tray)[3]
    process = subprocess.Popen([str(args.binary.resolve())], creationflags=0x08000000)
    host = wait_for(lambda: user.FindWindowW("FullWinTB.Window", None))
    children.append((process, host))
    if not host or not tray:
        raise RuntimeError("FullWinTB or Explorer taskbar is missing")
    capture = None
    try:
        main = start_window("main")
        peers = [start_window("peer-one"), start_window("peer-two")]
        original = physical_bounds(main)
        start = user.FindWindowExW(tray, None, "Start", None)
        user.PostMessageW(host, 0x111, 204, 0)
        user.PostMessageW(host, 0x111, 200, 0)
        check("Restored window keeps its bounds", wait_for(lambda: work_area()[3] == tray_bottom)
              and physical_bounds(main) == original)
        user.PostMessageW(main, 0x112, 0xF030, 0)
        check("Native maximize reaches the monitor edge",
              wait_for(lambda: user.IsZoomed(main) and physical_bounds(main, True)[3] == tray_bottom))
        control = wait_for(lambda: user.FindWindowExW(w.HWND(-3), None,
                                                      "FullWinTB.TaskbarAppearance.v0.1.0", None))
        check("The primary taskbar keeps its topmost band", control and
              wait_for(lambda: user.GetPropW(tray, "FullWinTB.LayerAttached")))
        if not wait_for(lambda: user.GetForegroundWindow() == main):
            raise RuntimeError("The test window did not become active")
        if args.vertical:
            check_vertical_movement(tray, host, main, check)

        rebar = user.FindWindowExW(tray, None, "ReBarWindow32", None)
        notification = user.FindWindowExW(tray, None, "TrayNotifyWnd", None)
        if start:
            gap = wait_for(lambda: gap_near_tray(tray, start, notification))
            check("Right gap passes clicks to the maximized window",
                  gap and wait_for(lambda: root_at(gap[0], gap[1]) == main), gap)

            user.PostMessageW(main, 0x8001, 0x808080, 0)
            user.PostMessageW(host, 0x111, 205, 0)
            def white_pixels():
                white, black = glyph_pixels(tray)
                return (white, black) if white > black + 25 else None
            white = wait_for(white_pixels)
            check("White text is rendered in the real notification area", white, white)
            user.PostMessageW(host, 0x111, 206, 0)
            def black_pixels():
                white, black = glyph_pixels(tray)
                return (white, black) if black > white + 25 else None
            black = wait_for(black_pixels)
            check("Black text is rendered in the real notification area", black, black)
            user.PostMessageW(main, 0x8001, 0x111111, 0)
            user.PostMessageW(host, 0x111, 207, 0)
            check("Automatic ink selects white on a dark background", control and
                  wait_for(lambda: user.GetPropW(control, "FullWinTB.AppliedInk") == 2))
            user.PostMessageW(main, 0x8001, 0xEEEEEE, 0)
            check("Automatic ink follows a background change in the same window", control and
                  wait_for(lambda: user.GetPropW(control, "FullWinTB.AppliedInk") == 3, 2))
            user.PostMessageW(main, 0x8001, 0x111111, 0)

            light = peers[0]
            user.PostMessageW(light, 0x8001, 0xEEEEEE, 0)
            user.PostMessageW(light, 0x112, 0xF030, 0)
            wait_for(lambda: user.IsZoomed(light))
            time.sleep(.5)
            points = taskbar_points()
            writes = user.GetPropW(tray, "FullWinTB.RegionWrites") or 0
            switches = 0
            frames = []
            colors = 0
            for target in [light, main] * 4:
                current = user.GetForegroundWindow()
                if current == target: continue
                if not click_taskbar(tray, points.get("main" if target == main else "peer-one")):
                    break
                frames.extend(trace_taskbar_hits(tray, start, .24))
                if not wait_for(lambda: user.GetForegroundWindow() == target, .65): break
                switches += 1
                expected = 2 if target == main else 3
                if control and wait_for(lambda: user.GetPropW(control, "FullWinTB.AppliedInk") == expected, .7):
                    colors += 1
            time.sleep(.4)
            writes_after = user.GetPropW(tray, "FullWinTB.RegionWrites") or 0
            check("White/dark max-window switches preserve the taskbar region",
                  switches >= 6 and writes == writes_after, (switches, writes, writes_after))
            check("Taskbar input stays visible throughout the switches", switches >= 6 and all(frames),
                  f"{sum(frames)}/{len(frames)} samples")
            check("Automatic ink follows both maximized windows", switches >= 6 and colors == switches,
                  (colors, switches))
            user.PostMessageW(light, 0x112, 0xF120, 0)

            def anchor_sample():
                left, top, right, bottom = physical_bounds(start)
                y = (top + bottom) // 2
                right_edge = physical_bounds(rebar)[2] - 2 if rebar else right - 2
                indices = (left + 2, right - 2, right_edge)
                hit = tuple(root_at(x, y) == tray for x in indices)
                bar_left, bar_top, _, _ = physical_bounds(tray)
                region = gdi.CreateRectRgn(0, 0, 0, 0)
                has_region = user.GetWindowRgn(tray, w.HRGN(region)) != 0
                drawn = tuple(not has_region or gdi.PtInRegion(w.HRGN(region), x - bar_left, y - bar_top)
                              for x in indices)
                gdi.DeleteObject(w.HRGN(region))
                return all(drawn), all(hit), (indices, y, drawn, hit)

            if args.messengers:
                if click_taskbar(tray, taskbar_points().get("weixin"), right=True):
                    def weixin_menu():
                        foreground = user.GetForegroundWindow()
                        return (foreground and process_name(foreground) in ("weixin.exe", "wechat.exe")
                                and "popup" in class_name(foreground).lower())
                    menu_open = wait_for(weixin_menu, 1)
                    menu_frames = trace_taskbar_hits(tray, start, .75)
                    check("Weixin tray context menu keeps the taskbar visible",
                          menu_open and all(menu_frames), f"{sum(menu_frames)}/{len(menu_frames)} samples")
                    menu_foreground = user.GetForegroundWindow()
                    if menu_foreground and (menu_foreground == tray or
                            process_name(menu_foreground) in ("explorer.exe", "weixin.exe", "wechat.exe")):
                        user.keybd_event(0x1B, 1, 0, 0)
                        user.keybd_event(0x1B, 1, 2, 0)
                    time.sleep(.2)
                else:
                    print("Weixin tray context menu SKIP (no visible Weixin icon)", flush=True)

            wait_for(lambda: all(anchor_sample()[:2]))
            for peer in peers:
                user.PostMessageW(peer, 0x10, 0, 0)
                samples = []
                for _ in range(36):
                    samples.append(anchor_sample())
                    time.sleep(.018)
                preserved = sum(bool(sample[0]) for sample in samples)
                details = [sample[2] for sample in samples if not (sample[0] and sample[1])]
                check("Taskbar icon paint region survives app close", preserved == len(samples),
                      f"{preserved}/{len(samples)}, transient samples {details[:4]}")
                check("Taskbar buttons receive input after the close",
                      wait_for(lambda: anchor_sample()[1]))

        check("Windows theme setting stays constant", taskbar_is_light() == light_theme)
        user.PostMessageW(main, 0x112, 0xF120, 0)
        check("Native restore returns to its original bounds",
              wait_for(lambda: not user.IsZoomed(main) and physical_bounds(main) == original))
        # Opening a real tray menu earlier can return focus to the user's app.
        # Test fullscreen only after bringing the dedicated fixture back.
        if user.GetForegroundWindow() != main:
            click_taskbar(tray, taskbar_points().get("main"))
        if not wait_for(lambda: user.GetForegroundWindow() == main):
            raise RuntimeError("The fullscreen test window did not become active")

        user.PostMessageW(main, 0x8002, 0, 0)
        check("A frameless fullscreen window receives input over the native taskbar",
              wait_for(lambda: not visible(tray) and root_at(*start_point(start)) == main))
        user.PostMessageW(main, 0x8003, 0, 0)
        check("Leaving fullscreen restores the taskbar",
              wait_for(lambda: visible(tray) and root_at(*start_point(start)) == tray))

        user.PostMessageW(main, 0x112, 0xF030, 0)
        wait_for(lambda: user.IsZoomed(main))
        user.PostMessageW(main, 0x8002, 3, 0)
        check("A true fullscreen surface can retain the native maximize flag",
              wait_for(lambda: user.IsZoomed(main) and not visible(tray) and
                       root_at(*start_point(start)) == main))
        windowed = start_window("peer-windowed")
        user.PostMessageW(main, 0x8004, windowed, 0)
        check("Switching away from fullscreen restores the taskbar",
              wait_for(lambda: user.GetForegroundWindow() == windowed and visible(tray) and
                       root_at(*start_point(start)) == tray))
        user.PostMessageW(windowed, 0x8004, main, 0)
        check("Returning to the same fullscreen window suppresses the taskbar again",
              wait_for(lambda: user.GetForegroundWindow() == main and not visible(tray)))
        user.PostMessageW(windowed, 0x10, 0, 0)
        fullscreen_bounds = physical_bounds(main)
        user.PostMessageW(host, 0x111, 200, 0)
        check("Disabling FullWinTB does not resize the fullscreen surface",
              wait_for(lambda: work_area() == baseline) and physical_bounds(main) == fullscreen_bounds)
        user.PostMessageW(host, 0x111, 200, 0)
        check("Enabling FullWinTB during fullscreen also yields to the application",
              wait_for(lambda: work_area()[3] == tray_bottom and not visible(tray)))
        user.PostMessageW(main, 0x8003, 0, 0)
        check("The taskbar recovers after a maximize-to-fullscreen transition",
              wait_for(lambda: visible(tray) and root_at(*start_point(start)) == tray))

        user.PostMessageW(host, 0x111, 204, 0)
        if start:
            user.PostMessageW(main, 0x8003, 1, 0)
            x, top, _, _ = physical_bounds(start)
            x += 8
            y = top + 8
            ready = wait_for(lambda: root_at(x, y - 108) == main and root_at(x, y) == tray)
            underlay = desktop_pixel(x, y - 108)
            check("Transparent mode still shows the window before disable",
                  ready and wait_for(lambda: desktop_pixel(x, y) == underlay),
                  f"behind #{underlay:06x}, sample #{desktop_pixel(x, y):06x}")
        user.PostMessageW(host, 0x111, 200, 0)
        check("Disable restores the original work area", wait_for(lambda: work_area() == baseline))
        if start:
            check("Disable restores the taskbar background",
                  wait_for(lambda: desktop_pixel(x, y) != underlay and root_at(x, y) == tray))
            opaque = desktop_pixel(x, y)
        user.PostMessageW(host, 0x111, 200, 0)
        if start:
            if control:
                wait_for(lambda: user.GetPropW(control, "FullWinTB.XamlStatus") or
                         user.GetPropW(control, "FullWinTB.XamlBackgrounds"), 9)
            clear = wait_for(lambda: desktop_pixel(x, y) == underlay and root_at(x, y) == tray)
            status = user.GetPropW(control, "FullWinTB.XamlStatus") or 0
            backgrounds = user.GetPropW(control, "FullWinTB.XamlBackgrounds") or 0
            check("FullWinTB alone changes opaque pixels to transparent",
                  opaque != underlay and clear and backgrounds,
                  f"#{opaque:06x} -> #{desktop_pixel(x, y):06x}, "
                  f"underlay #{underlay:06x}, target {root_at(x, y) == tray}, "
                  f"connection {status:#010x}, backgrounds {backgrounds}")

        user.PostMessageW(main, 0x8003, 0, 0)
        user.PostMessageW(main, 0x112, 0xF030, 0)
        wait_for(lambda: user.IsZoomed(main) and visible(tray))

        if args.messengers:
            seen = set()
            for keys in ((0x11, 0x12, 0x41), (0x12, 0x41)):
                capture = launch_messenger_capture(keys, main)
                if not capture: continue
                name = process_name(capture)
                seen.add(name)
                x, y, _, _ = physical_bounds(start)
                check(name + " capture does not leave the native taskbar on top",
                      wait_for(lambda: window_process(root_at(x + 15, y + 20)) == window_process(capture)
                               and (not visible(tray) or above(capture, tray))))
                cancel_snip(capture)
                wait_for(lambda: not visible(capture))
                check(name + " capture cancellation restores the taskbar",
                      wait_for(lambda: user.IsWindowVisible(tray) and above(tray, main)))
                capture = None
            print("Captured messenger clients", sorted(seen), flush=True)

        if args.snipping:
            if snip_window():
                raise RuntimeError("Close the existing screenshot overlay before this test")
            result = shell.ShellExecuteW(None, "open", "ms-screenclip:", None, None, 1)
            if not result or result <= 32:
                raise RuntimeError("Windows screenshot overlay could not be launched")
            capture = wait_for(snip_window)
            check("Windows capture overlay is present", capture)
            if capture:
                start_x = physical_bounds(start)[0] + 20
                user.PostMessageW(host, 0x111, 201, 0)
                time.sleep(.25)
                check("Live taskbar yields to the capture overlay",
                      not user.IsWindowVisible(tray) or above(capture, tray))
                check("Capture overlay gets input over the Start button",
                      window_process(root_at(start_x, tray_bottom - 23)) == window_process(capture))
                cancel_snip(capture)
                check("Capture overlay cancels", wait_for(lambda: not snip_window()))
                restored = wait_for(lambda: visible(tray) and above(tray, main) and
                                    root_at(start_x, tray_bottom - 23) == tray)
                if not restored:
                    foreground = user.GetForegroundWindow()
                    print("Focus after snipping", class_name(foreground), "test window",
                          foreground == main, "taskbar flags",
                          hex(user.GetWindowLongW(tray, -20) & 0xFFFFFFFF), flush=True)
                check("Taskbar returns over the application", restored)

        process_memory(process)
    finally:
        if visible(capture):
            cancel_snip(capture)
        if host and process.poll() is None:
            user.PostMessageW(host, 0x111, 204 + wanted_ink, 0)
        for child, window in children:
            if window and child.poll() is None:
                user.PostMessageW(window, 0x10, 0, 0)
        for child, _ in children:
            try:
                child.wait(3)
            except subprocess.TimeoutExpired:
                child.terminate()
                child.wait(3)

    return 1 if errors else 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture")
    parser.add_argument("--binary", type=Path, default=Path("build-out/FullWinTB.exe"))
    parser.add_argument("--replace-running", action="store_true")
    parser.add_argument("--snipping", action="store_true")
    parser.add_argument("--messengers", action="store_true")
    parser.add_argument("--vertical", action="store_true")
    args = parser.parse_args()
    if args.fixture:
        fixture(args.fixture)
    else:
        sys.exit(run(args))

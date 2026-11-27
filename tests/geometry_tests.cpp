#include "../src/desktop_session.h"
#include "../src/geometry.h"
#include "../src/ink_sampling.h"
#include "../src/icon_trail.h"
#include "../src/resource.h"
#include "../src/sticker_atlas.h"
#include "../src/taskbar_motion.h"
#include "../src/taskbar_layer.h"
#include "../src/taskbar_region.h"
#include <cstdio>
#include <cstring>

using namespace fullwintb;

namespace {

int bridgeVisibilityChanges = 0;
int nativeErases = 0;

bool StickerFramesUsable() {
    using namespace appearance;
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_STICKER_ATLAS), RT_RCDATA);
    if (!resource || SizeofResource(module, resource) != stickerAtlasBytes) return false;
    const auto* atlas = static_cast<const unsigned char*>(LockResource(LoadResource(module, resource)));
    if (!atlas || atlas[3]) return false;
    const auto* images = atlas + stickerPaletteBytes;
    if (!std::memcmp(images, images + (stickerFrames - 1) * stickerFrameBytes, stickerFrameBytes)) return false;
    for (int offset = 0; offset < stickerPaletteBytes; offset += 4) {
        const auto* pixel = atlas + offset;
        if (pixel[0] > pixel[3] || pixel[1] > pixel[3] || pixel[2] > pixel[3]) return false;
    }
    for (int frame = 0; frame < stickerFrames; ++frame) {
        const auto* image = images + frame * stickerFrameBytes;
        for (const int corner : {0, stickerWidth - 1, stickerFrameBytes - stickerWidth, stickerFrameBytes - 1})
            if (image[corner]) return false;
    }
    return true;
}

LRESULT CALLBACK WatchBridge(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_SHOWWINDOW || (message == WM_WINDOWPOSCHANGED &&
        (reinterpret_cast<const WINDOWPOS*>(lp)->flags & (SWP_HIDEWINDOW | SWP_SHOWWINDOW))))
        ++bridgeVisibilityChanges;
    return DefWindowProcW(window, message, wp, lp);
}

bool VisibleCanvasRoundTrip() {
    const HINSTANCE module = GetModuleHandleW(nullptr);
    WNDCLASSW spec{};
    spec.hInstance = module;
    spec.lpfnWndProc = WatchBridge;
    spec.lpszClassName = L"Windows.UI.Composition.DesktopWindowContentBridge";
    if (!RegisterClassW(&spec)) return false;
    WNDCLASSW parentSpec{};
    parentSpec.hInstance = module;
    parentSpec.lpszClassName = L"FullWinTB.CanvasTest";
    parentSpec.lpfnWndProc = [](HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
        if (message == WM_ERASEBKGND) {
            ++nativeErases;
            return TRUE;
        }
        return DefWindowProcW(window, message, wp, lp);
    };
    if (!RegisterClassW(&parentSpec)) return false;
    const HWND parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, parentSpec.lpszClassName, nullptr,
                                        WS_POPUP | WS_VISIBLE, 0, 0, 64, 16, nullptr, nullptr, module, nullptr);
    const HWND child = parent ? CreateWindowExW(0, spec.lpszClassName, nullptr, WS_CHILD | WS_VISIBLE,
                                               0, 0, 64, 16, parent, nullptr, module, nullptr) : nullptr;
    bool passed = false;
    if (child) {
        appearance::TaskbarLayer layer;
        layer.Update(parent, appearance::LayerMode::floating);
        bridgeVisibilityChanges = 0;
        const bool grew = layer.EnsureCanvas(30, true) && layer.EnsureCanvas(60, true) && layer.CanvasIntact();
        nativeErases = 0;
        SendMessageW(parent, WM_ERASEBKGND, 0, 0);
        HRGN movingClip = CreateRectRgn(0, 8, 54, 24);
        const bool regionWritten = movingClip && appearance::WriteTaskbarRegion(parent, movingClip, true);
        if (!regionWritten && movingClip) DeleteObject(movingClip);
        const HRGN actualClip = CreateRectRgn(0, 0, 0, 0);
        RECT clipBounds{};
        const bool clipped = actualClip && GetWindowRgn(parent, actualClip) == SIMPLEREGION &&
            GetRgnBox(actualClip, &clipBounds) == SIMPLEREGION && SameRect(clipBounds, {0, 8, 54, 24});
        if (actualClip) DeleteObject(actualClip);
        const bool eraseSuppressed = nativeErases == 0;
        const bool keptBridgeVisible = bridgeVisibilityChanges == 0;
        layer.Update(parent, appearance::LayerMode::suspended);
        const bool suspendedInPlace = IsWindowVisible(child) && layer.CanvasIntact();
        layer.HoldPresentation(true);
        SetWindowPos(parent, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        layer.Update(parent, appearance::LayerMode::floating);
        const bool hiddenUntilReady = !IsWindowVisible(parent) && layer.CanvasIntact();
        layer.HoldPresentation(false);
        const bool revealed = IsWindowVisible(child) && layer.CanvasIntact();
        layer.Update(parent, appearance::LayerMode::covered);
        layer.HoldPresentation(true);
        layer.HoldPresentation(false);
        const bool coverStillHidden = !IsWindowVisible(parent);
        layer.Update(nullptr, appearance::LayerMode::native);
        nativeErases = 0;
        SendMessageW(parent, WM_ERASEBKGND, 0, 0);
        RECT parentBounds{}, childBounds{};
        passed = grew && GetWindowRect(parent, &parentBounds) && GetWindowRect(child, &childBounds) &&
                 SameRect(parentBounds, {0, 0, 64, 16}) && SameRect(childBounds, parentBounds) &&
                 IsWindowVisible(child) && keptBridgeVisible && regionWritten && clipped && eraseSuppressed &&
                 nativeErases == 1 && suspendedInPlace && hiddenUntilReady && revealed && coverStillHidden;
        if (!passed)
            std::fprintf(stderr, "canvas: grew=%d region=%d clipped=%d erase suppressed=%d native erases=%d kept visible=%d suspended=%d hidden=%d revealed=%d covered=%d\n",
                         grew, regionWritten, clipped, eraseSuppressed, nativeErases, keptBridgeVisible,
                         suspendedInPlace, hiddenUntilReady, revealed, coverStillHidden);
    }
    if (parent) DestroyWindow(parent);
    UnregisterClassW(spec.lpszClassName, module);
    UnregisterClassW(parentSpec.lpszClassName, module);
    return passed;
}

DWORD WINAPI IsolatedCanvasThread(void* desktop) {
    if (!SetThreadDesktop(static_cast<HDESK>(desktop))) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "isolated test could not attach to its desktop: %lu\n", error);
        return error == ERROR_ACCESS_DENIED ? 77 : 1;
    }
    return VisibleCanvasRoundTrip() ? 0 : 1;
}

int IsolatedCanvasRoundTrip() {
    wchar_t name[48]{};
    swprintf_s(name, L"FullWinTB.Geometry.%lu", GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(name, nullptr, nullptr, 0,
                                         DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
    if (!desktop) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "isolated test could not create a desktop: %lu\n", error);
        return error == ERROR_ACCESS_DENIED ? 77 : 1;
    }
    const HANDLE thread = CreateThread(nullptr, 0, IsolatedCanvasThread, desktop, 0, nullptr);
    DWORD result = 1;
    if (thread) {
        if (WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0) GetExitCodeThread(thread, &result);
        CloseHandle(thread);
    }
    CloseDesktop(desktop);
    return static_cast<int>(result);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--canvas") == 0) return IsolatedCanvasRoundTrip();
    int failures = 0;
    const auto check = [&](bool success, const char* caseName) {
        if (success) return;
        std::fprintf(stderr, "FAIL: %s\n", caseName);
        ++failures;
    };

    const RECT screen{-2560, -1600, 0, 0};
    const RECT work{-2520, -1560, -20, -84};
    const RECT bar{-2560, -84, 0, 0};
    const auto edge = TaskbarEdge(bar, screen);
    check(edge == Edge::bottom, "bottom bar on a monitor at negative coordinates");

    const RECT expanded = ExpandedWork(screen, work, bar, Edge::bottom);
    check(SameRect(expanded, RECT{-2520, -1560, -20, 0}), "other appbar reservations survive");
    check(SameRect(ExpandedWork(screen, RECT{-2520, -1560, -20, -130}, bar, Edge::bottom),
                   RECT{-2520, -1560, -20, -130}), "unrelated bottom reservation");
    check(!TaskbarEdge(RECT{-2560, 0, 0, 84}, screen), "bar entirely outside the display");
    check(!TaskbarEdge(screen, screen), "a full-screen viewport is never the dock reservation");

    DesktopSession session;
    check(session.Active() && session.Lock() && !session.Active() && !session.Lock(), "lock suspends the desktop once");
    check(session.Unlock(1000) && session.Settling() && !session.Resume(1399), "unlock waits for the shell geometry");
    session.ObserveChange(1250);
    check(!session.Resume(1600) && session.Resume(1670) && !session.Resume(1671),
          "on unlock a burst of geometry notifications yields one reconciliation");
    session.Lock();
    session.Unlock(2000);
    for (ULONGLONG now = 2200; now <= 3800; now += 200) session.ObserveChange(now);
    check(session.Resume(3800), "a persistent notification stream cannot extend settling forever");

    check(StickerFramesUsable(), "the embedded peel artwork changes across frames and has transparent corners");

    const RECT leftBar{-2560, -1600, -2520, 0};
    const RECT rightBar{-20, -1600, 0, 0};
    const RECT topBar{-2560, -1600, 0, -1560};
    check(TaskbarEdge(leftBar, screen) == Edge::left, "left edge");
    check(TaskbarEdge(rightBar, screen) == Edge::right, "right edge");
    check(TaskbarEdge(topBar, screen) == Edge::top, "top edge");
    check(SameRect(ExpandedWork(screen, work, leftBar, Edge::left),
                   RECT{-2560, -1560, -20, -84}), "expand left");
    check(SameRect(ExpandedWork(screen, work, rightBar, Edge::right),
                   RECT{-2520, -1560, 0, -84}), "expand right");
    check(SameRect(ExpandedWork(screen, work, topBar, Edge::top),
                   RECT{-2520, -1600, -20, -84}), "expand top");

    const RECT outer{-2532, -1572, -8, -72};
    check(Fits(outer, work, SIZE{12, 12}), "native maximize borders at 150 percent");
    check(!Fits(RECT{-2520, -1560, -1280, -84}, work, SIZE{12, 12}), "snap keeps its original bounds");
    const RECT full = TransferBounds(outer, work, expanded);
    check(SameRect(full, RECT{-2532, -1572, -8, 12}), "retain invisible frame margins");
    check(SameRect(TransferBounds(full, expanded, work), outer), "no drift over a round trip");

    const DWORD normalMaximize = WS_OVERLAPPEDWINDOW | WS_MAXIMIZE;
    check(ClassifyWindow(screen, screen, screen, normalMaximize, false) == WindowMode::maximized,
          "ordinary maximize remains managed even when its whole client fills the monitor");
    check(ClassifyWindow(screen, screen, screen, WS_POPUP | WS_MAXIMIZE, false) == WindowMode::fullscreen,
          "a frameless fullscreen window can retain the maximize style");
    check(ClassifyWindow(screen, screen, screen, WS_POPUP, false) == WindowMode::fullscreen,
          "a plain fullscreen popup takes over the monitor");
    check(ClassifyWindow(screen, work, screen, WS_OVERLAPPEDWINDOW, false) == WindowMode::windowed,
          "a window whose client leaves the monitor edges free remains windowed");
    check(ClassifyWindow(screen, screen, RECT{0, -1600, 2560, 0}, WS_POPUP, false) == WindowMode::windowed,
          "fullscreen on another monitor leaves this monitor alone");
    check(ClassifyWindow(screen, screen, screen, WS_POPUP, true) == WindowMode::windowed,
          "respect an explicit opt-out of shell fullscreen detection");

    appearance::AutomaticInk ink;
    using appearance::Ink;
    check(!ink.Observe(239) && ink.Settling(), "a single bright sample waits");
    check(ink.Observe(232) && ink.Current() == Ink::black, "light backgrounds select black ink");
    check(!ink.Observe(28) && !ink.Observe(132) && !ink.Settling(), "a single dark frame is ignored");
    check(!ink.Observe(32) && ink.Observe(33) && ink.Current() == Ink::white,
          "a settled dark background selects white ink");
    check(!ink.Observe(125) && ink.Current() == Ink::white, "the middle range preserves white ink");
    check(!ink.Observe(233) && ink.Settling(), "light recovery waits for a second frame");
    ink.Pause();
    check(!ink.Observe(232) && ink.Observe(232) && ink.Current() == Ink::black,
          "a pause discards the unconfirmed sample");

    using appearance::IconOffset;
    appearance::IconMotion motion;
    const auto advance = [&](int frames) {
        for (int frame = 0; frame < frames; ++frame) motion.Step(1.0 / 120, 1.0);
    };
    check(!motion.Moving(), "the resting icon group uses no animation timer");
    constexpr double impulse = 64 * 35.04;
    motion.Kick(-impulse, 0);
    advance(12);
    check(motion.Position() == IconOffset{-58, 0} && !motion.Target(),
          "the movement impulse adds no resting point");
    advance(24);
    check(motion.Position().x > -58 && motion.Position().x < 0, "the group returns as soon as its outward momentum runs out");
    advance(180);
    check(!motion.Position() && !motion.Moving(), "the magnet settles at the exact origin");

    motion.BeginDrag({});
    motion.Aim({-112, -60});
    advance(120);
    check(motion.Position() == IconOffset{-112, -60} && motion.Dragging(), "the group can be positioned on both axes");
    motion.TogglePin({-112, -60});
    advance(240);
    check(motion.Position() == IconOffset{-112, -60} && motion.Pinned() && !motion.Moving(),
          "a manual pin holds without a timer");
    motion.Release();
    check(!motion.ShowsLatch() && motion.Position() == IconOffset{-112, -60} && motion.Moving(),
          "tearing away the sticker hides it as the magnetic return begins");
    advance(480);
    check(!motion.Position() && !motion.Moving(), "unpinning returns to the same magnet");

    motion.BeginDrag({});
    motion.Aim({120, 0});
    advance(3);
    const LONG released = motion.Position().x;
    motion.EndDrag();
    advance(2);
    check(motion.Position().x > released, "release keeps the velocity the body had in the hand");
    const appearance::MotionProfile simpleLanding({}, appearance::ReturnStyle::simple);
    for (int frame = 0; frame < 240; ++frame) {
        motion.Step(1.0 / 120, 1.0, simpleLanding);
        motion.Limit({}, {500, 0}, simpleLanding.returnStyle);
    }
    check(!motion.Position() && !motion.Moving(), "the simple style preserves the previous stop at a screen edge");

    using appearance::ReturnStyle;
    const appearance::MotionSettings wobbly{100, 498, 500, 85};
    for (const auto style : {ReturnStyle::elastic, ReturnStyle::simple}) {
        const appearance::MotionProfile selected(wobbly, style);
        appearance::IconMotion across, top, bottom;
        across.SetPosition({100, 0});
        top.SetPosition({0, 100});
        bottom.SetPosition({0, -100});
        bool crossedHome = false;
        bool sameCurve = true;
        LONG previous = 100;
        for (int frame = 0; frame < 780; ++frame) {
            for (auto* group : {&across, &top, &bottom}) group->Step(1.0 / 120, 1.0, selected);
            const LONG x = across.Limit({-500, 0}, {500, 0}, style).x;
            const LONG fromTop = top.Limit({}, {0, 500}, style).y;
            const LONG fromBottom = bottom.Limit({0, -500}, {}, style).y;
            crossedHome |= x < -2;
            sameCurve &= fromTop == std::abs(x) && fromBottom == -fromTop;
            if (style == ReturnStyle::simple) sameCurve &= x <= previous && x >= 0;
            previous = x;
        }
        check(sameCurve, "the selected return curve has the same amplitude on both axes and both vertical edges");
        check(crossedHome == (style == ReturnStyle::elastic), "only elastic return has a visible rebound");
        check(!across.Moving() && !top.Moving() && !bottom.Moving(), "all return styles stop exactly at home");
    }

    appearance::IconMotion parked;
    const appearance::MotionProfile profile;
    ULONGLONG time = 0;
    const auto tick = [&](int frames, bool inside) {
        for (int frame = 0; frame < frames; ++frame) {
            time += 8;
            parked.WatchReturn(time, profile.hold, inside);
            parked.Wake(time);
            parked.Step(.008, 1.0, profile);
        }
    };
    parked.ScrollTo({-profile.wheelStep, 0}, profile);
    tick(300, true);
    check(parked.Position() == IconOffset{-32, 0} && !parked.Moving() && parked.WaitingToReturn() &&
          !parked.ReturnDelay(time), "the cursor can stay in the sweep longer than the entire hold");
    parked.ScrollTo({parked.WheelOrigin().x - profile.wheelStep, 0}, profile);
    tick(100, true);
    check(parked.Position() == IconOffset{-64, 0} && !parked.ReturnDelay(time),
          "another wheel detent accumulates and waits at the new position");
    tick(75, false);
    check(parked.Position() == IconOffset{-64, 0} && parked.ReturnDelay(time) > 550,
          "leaving the rectangle starts the full hold and does not return immediately");
    tick(200, true);
    check(parked.Position() == IconOffset{-64, 0} && !parked.ReturnDelay(time) && !parked.Moving(),
          "re-entering the rectangle cancels the countdown");
    tick(145, false);
    check(parked.Position() == IconOffset{-64, 0} && parked.ReturnDelay(time) > 0,
          "the next departure starts a full new hold");
    tick(20, false);
    check(parked.Position().x > -64 && parked.Moving() && !parked.ShowsLatch(),
          "the completed countdown starts a magnetic return and dismisses the sticker");
    tick(300, false);
    check(!parked.Position() && !parked.Moving(), "a temporary parking spot returns exactly to the dock");

    parked.BeginDrag({-20, -70});
    parked.Aim({-80, -70});
    tick(70, false);
    parked.EndDrag(true);
    tick(280, true);
    check(parked.Position() == IconOffset{-80, -70} && parked.WaitingToReturn() && !parked.Moving(),
          "a sideways drag of a lifted group preserves the other coordinate and waits");
    check(!parked.WatchReturn(time, 0, true) && !parked.Wake(time),
          "a zero hold still waits for the pointer to leave");
    check(parked.WatchReturn(time, 0, false) && parked.Wake(time) && !parked.WaitingToReturn(),
          "with a zero hold the return starts on departure");

    appearance::IconTilt tilt;
    for (int frame = 0; frame < 14; ++frame) tilt.Step(12, .008, 1);
    check(tilt.Tenths() > 40 && tilt.Tenths() <= 80, "moving buttons get visible skew capped at eight degrees");
    for (int frame = 0; frame < 46; ++frame) tilt.Step(0, .008, 1);
    check(!tilt.Active() && tilt.Tenths() == 0, "a coasting icon springs back to an upright position");

    constexpr appearance::MotionSettings tuning{242, 8142, 775, 130, 27, 1475};
    constexpr auto legacyPacket = (1ull << 63) | 242 | (8142ull << 16) | (775ull << 32) | (130ull << 48);
    check(appearance::UnpackMotionSettings(appearance::PackMotionSettings(tuning)) == tuning &&
          appearance::UnpackMotionSettings(legacyPacket) == appearance::MotionSettings{242, 8142, 775, 130} &&
          appearance::UnpackMotionSettings(0) == appearance::MotionSettings{},
          "the four controls persist with the reference precision and older installs get Snappy");
    check(appearance::UnpackMotionSettings(appearance::PackMotionSettings({1, 999999, 1, 99999, 7, 4000})) ==
          appearance::MotionSettings{15, 16000, 100, 300, 12, 3000}, "invalid tuning values are clamped to the dial ranges");

    const appearance::ReturnMagnet snappy({});
    double position = 0, velocity = impulse;
    // The same one-point Feel Body2D with no walls, sampled at 120 Hz.
    for (int frame = 0; frame < 24; ++frame) snappy.Advance(position, velocity, 1.0 / 120, 1.0);
    check(std::abs(position - 55.11123113) < .001 && std::abs(velocity + 43.64854297) < .001,
          "Snappy matches the reference during the coast and far pull");
    for (int frame = 24; frame < 144; ++frame) snappy.Advance(position, velocity, 1.0 / 120, 1.0);
    check(std::abs(position - 21.40633404) < .001 && std::abs(velocity + 74.65718987) < .001,
          "Snappy matches the reference as the near magnet catches");
    double scaled = 0, scaledV = impulse * 1.75;
    for (int frame = 0; frame < 576; ++frame) snappy.Advance(scaled, scaledV, 1.0 / 480, 1.75);
    check(std::abs(scaled / 1.75 - position) < .01, "the field scales with DPI and follows the same path at different refresh rates");

    const appearance::ReturnMagnet highDrag({15, 16000, 1400, 300});
    double stable = 0, stableV = impulse;
    for (int frame = 0; frame < 90; ++frame) highDrag.Advance(stable, stableV, .016, 1.0);
    check(std::isfinite(stable) && std::abs(stable) < 3 && std::abs(stableV) < 4,
          "the lightest and most damped settings remain stable");

    using namespace std::chrono_literals;
    check(appearance::IconDelay(44, 96) == appearance::IconDelay(77, 168) &&
          appearance::IconDelay(88, 96) > appearance::IconDelay(44, 96),
          "a wave starts later at distant icons and uses the same spacing at different DPIs");
    const auto waveStart = appearance::MotionClock::time_point{} + 1s;
    appearance::IconTrail path;
    path.Start(waveStart, 0);
    path.Record(waveStart + 8ms, 20);
    path.Record(waveStart + 16ms, 48);
    check(path.At(waveStart + 16ms) == 48 && path.At(waveStart + 12ms) == 34 && path.At(waveStart - 12ms) == 0,
          "a single motion supplies distinct offsets at different per-icon delays");
    path.Record(waveStart + 24ms, 0);
    path.Record(waveStart + 40ms, 0);
    check(path.Pending(waveStart + 40ms, 20ms) && !path.Pending(waveStart + 44ms, 20ms) &&
          path.At(waveStart + 44ms - 20ms) == 0,
          "the wave's tail completes at the exact dock once every icon sees the final sample");
    for (const double damping : {0.42, 1.0, 1.24}) {
        const appearance::Spring spring(1.0, 340.0, damping);
        double x = 0, v = 0, otherX = 0, otherV = 0;
        spring.Advance(x, v, 100, .048);
        for (int sample = 0; sample < 12; ++sample) spring.Advance(otherX, otherV, 100, .004);
        check(std::abs(x - otherX) < 1e-7 && std::abs(v - otherV) < 1e-7,
              "the exact spring also handles critical and overdamped settings");
    }
    double light = 0, heavy = 0, lightV = 0, heavyV = 0;
    appearance::Spring(1.0, 340, .7).Advance(light, lightV, 100, .048);
    appearance::Spring(2.4, 340, .7).Advance(heavy, heavyV, 100, .048);
    check(light > heavy && heavy > 0, "mass changes the response to the same input");
    check(PlacePopup({0, 110, 2560, 194}, {0, 84, 2560, 1600}, 250, 12, true).top == 206 &&
          PlacePopup({0, 1480, 2560, 1564}, {0, 84, 2560, 1600}, 250, 12, true).top == 1218,
          "a thumbnail follows a top taskbar and flips when there is no room below");
    check(PlacePopup({0, 1120, 2560, 1204}, {0, 0, 2560, 1516}, 250, 12, false).top == 858 &&
          PlacePopup({0, 0, 2560, 84}, {0, 0, 2560, 1516}, 250, 12, false).top == 96,
          "a thumbnail follows a bottom taskbar and flips when there is no room above");
    const auto trayAtBottom = PlacePopup({0, 1516, 2560, 1600}, {0, 0, 2560, 1600}, 340, 14, true);
    check(!trayAtBottom.below && trayAtBottom.top == 1162, "a top-mounted tray opens upward after being moved down");
    const auto trayAtTop = PlacePopup({0, 0, 2560, 84}, {0, 0, 2560, 1600}, 340, 14, false);
    check(trayAtTop.below && trayAtTop.top == 98, "a bottom-mounted tray opens downward after being moved up");
    const auto tall = PlacePopup({0, 250, 800, 300}, {0, 0, 800, 600}, 440, 12, false);
    check(tall.below && tall.top == 160, "a popup taller than either side stays within the screen");

    appearance::IconWheelGesture gesture;
    check(!gesture.Continues({1920, 1558}, 1000, 8), "a fresh wheel event is not owned by FullWinTB");
    gesture.Start({1920, 1558}, 1000);
    check(gesture.Continues({1923, 1559}, 1600, 8), "a stationary pointer keeps its tray gesture");
    check(!gesture.Continues({1940, 1558}, 1600, 8), "moving the pointer leaves the tray gesture");
    gesture.Extend(1600);
    check(gesture.Continues({1920, 1558}, 2300, 8) && !gesture.Remaining(2350),
          "consecutive detents extend the gesture until the wheel rests");

    using appearance::IconGroup;
    using appearance::TaskbarRegion;
    appearance::TaskbarLayout layout;
    layout.bounds = layout.dock = RECT{0, 1516, 2560, 1600};
    layout.buttons = {{940, 1516, 1700, 1600}};
    layout.notification = RECT{1783, 1516, 2560, 1600};
    layout.dpi = 168;
    layout.startButton = {940, 1516, 1020, 1600};
    check(TaskbarRegion::Fits(layout, {{-112, 0}, {}}), "the tray can stay put as the app strip moves");
    const auto horizontal = TaskbarRegion::Travel(layout);
    check(horizontal.Project({{}, {-40, 0}}) == appearance::IconOffsets{{}, {-40, 0}},
          "the tray moves alone before contact");
    check(horizontal.Project({{}, {-112, 0}}) == appearance::IconOffsets{{-45, 0}, {-112, 0}} &&
          horizontal.Project({{}, {-224, 0}}) == appearance::IconOffsets{{-157, 0}, {-224, 0}},
          "the tray pushes the app strip left with a constant gap");
    check(horizontal.Project({{-112, 0}, {-112, 0}}) == appearance::IconOffsets{{-112, 0}, {-112, 0}} &&
          horizontal.Project({{-112, 0}, {-40, 0}}) == appearance::IconOffsets{{-112, 0}, {-40, 0}},
          "the app strip keeps its own offset after contact ends");
    check(horizontal.Project({{}, {-5000, 0}}) == appearance::IconOffsets{{-940, 0}, {-1007, 0}},
          "both strips stop when the app strip reaches the screen edge");
    check(SameRect(TaskbarRegion::ReturnArea(layout, {{-112, 0}, {}}, IconGroup::apps),
                   RECT{828, 1516, 1700, 1600}),
          "horizontal dwell covers the original group, the moved group and everything between");

    layout.bounds.top = 1313;
    check(TaskbarRegion::Fits(layout, {{0, -112}, {}}) && TaskbarRegion::Fits(layout, {{}, {0, -112}}),
          "each strip can occupy a different row");
    check(TaskbarRegion::Travel(layout).Project({{0, -300}, {0, 20}}) == appearance::IconOffsets{{0, -203}, {}},
          "the vertical viewport bounds both strips");
    check(TaskbarRegion::Fits(layout, {{-112, -112}, {-900, 0}}),
          "a horizontal tray can travel under an independently raised app strip");
    check(SameRect(TaskbarRegion::Strip(layout, {{0, -112}, {}}, IconGroup::notification),
                   TaskbarRegion::Strip(layout, {}, IconGroup::notification)) &&
          SameRect(TaskbarRegion::Strip(layout, {{0, -112}, {}}, IconGroup::apps), RECT{940, 1404, 1700, 1488}),
          "the app hit strip follows the vertical displacement");
    check(SameRect(TaskbarRegion::ReturnArea(layout, {{0, -112}, {}}, IconGroup::apps),
                   RECT{940, 1404, 1700, 1600}) &&
          SameRect(TaskbarRegion::ReturnArea(layout, {{-112, -112}, {}}, IconGroup::apps),
                   RECT{828, 1404, 1700, 1600}),
          "vertical and diagonal dwell extend to the original taskbar row");
    return failures ? 1 : 0;
}

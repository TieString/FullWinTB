#include "appearance.h"
#include "geometry.h"
#include "taskbar_latch.h"
#include "taskbar_layer.h"
#include "taskbar_motion.h"
#include "taskbar_region.h"

#include <ocidl.h>
#include <windowsx.h>
#include <xamlOM.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include "taskbar_icon_ink.h"
#include "taskbar_overflow.h"
#include "taskbar_pin_input.h"
#include "taskbar_preview.h"
#include "taskbar_start.h"
#include "taskbar_wave.h"
#include "xaml_translation.h"

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace xaml = winrt::Windows::UI::Xaml;
namespace appearance = fullwintb::appearance;

constexpr GUID siteId{0x23e89502, 0x8614, 0x4b54, {0x8d, 0xc5, 0x06, 0xf5, 0xc5, 0xff, 0x62, 0xfd}};
constexpr UINT setupFinished = WM_APP + 1;
constexpr UINT hostExited = WM_APP + 2;
constexpr UINT layoutReady = WM_APP + 3;
constexpr UINT motionFrame = WM_APP + 4;
constexpr UINT iconWheel = WM_APP + 6;
constexpr UINT motionInputChanged = WM_APP + 8;
static_assert(motionInputChanged != appearance::taskbarOrderChanged);
constexpr UINT_PTR layoutTimer = 1;
constexpr UINT_PTR motionTimer = 2;
constexpr UINT_PTR pinRecoveryTimer = 3;
constexpr UINT pinRecoveryTimeout = 1300;
constexpr UINT renderingWatchdog = 60;
constexpr UINT motionRetryWindow = 120;
constexpr appearance::IconGroup iconGroups[]{appearance::IconGroup::apps, appearance::IconGroup::notification};
using appearance::MotionClock;
using appearance::MotionAxis;
using appearance::IconOffset;

struct Background {
    InstanceHandle handle;
    winrt::weak_ref<xaml::Shapes::Shape> element;
    winrt::Windows::Foundation::IInspectable originalLocalFill{nullptr};
    xaml::Media::Brush originalFill{nullptr};
    bool owned = false;
    int64_t watch = 0;

    void Apply(bool enabled) {
        const auto current = element.get();
        if (!current) return;
        const auto property = xaml::Shapes::Shape::FillProperty();
        if (enabled && current.Fill()) {
            originalLocalFill = current.ReadLocalValue(property);
            originalFill = current.Fill();
            current.Fill(nullptr);
            owned = true;
        } else if (!enabled && owned) {
            if (!current.Fill()) {
                try {
                    if (originalLocalFill == xaml::DependencyProperty::UnsetValue())
                        current.ClearValue(property);
                    else
                        current.SetValue(property, originalLocalFill);
                } catch (const winrt::hresult_error&) {
                    current.Fill(originalFill);
                }
            }
            originalLocalFill = nullptr;
            originalFill = nullptr;
            owned = false;
        }
    }
};

struct ThemeRoot {
    InstanceHandle handle;
    winrt::weak_ref<xaml::FrameworkElement> element;
    winrt::Windows::Foundation::IInspectable originalLocal{nullptr};
    xaml::ElementTheme original = xaml::ElementTheme::Default;
    std::optional<xaml::ElementTheme> applied;

    void Apply(appearance::Ink ink) {
        const auto current = element.get();
        if (!current) return;
        const auto property = xaml::FrameworkElement::RequestedThemeProperty();
        if (ink != appearance::Ink::system) {
            const auto theme = ink == appearance::Ink::white
                ? xaml::ElementTheme::Dark : xaml::ElementTheme::Light;
            if (current.RequestedTheme() == theme) return;
            if (!applied) {
                originalLocal = current.ReadLocalValue(property);
                original = current.RequestedTheme();
            }
            current.RequestedTheme(theme);
            applied = theme;
        } else if (applied) {
            if (current.RequestedTheme() == *applied) {
                try {
                    if (originalLocal == xaml::DependencyProperty::UnsetValue())
                        current.ClearValue(property);
                    else
                        current.SetValue(property, originalLocal);
                } catch (const winrt::hresult_error&) {
                    current.RequestedTheme(original);
                }
            }
            applied.reset();
            originalLocal = nullptr;
        }
    }
};

struct IconFrame {
    winrt::weak_ref<xaml::FrameworkElement> frame;
    winrt::event_token wheel{};
    appearance::XamlTranslation translation;
    appearance::IconWave wave;
    std::optional<POINT> returnAnchor;
    bool stickerDismissed = false;
    std::array<winrt::Windows::Foundation::IInspectable, 5> gripHandlers{};
    winrt::Windows::Foundation::IInspectable originalHeight{nullptr};
    winrt::Windows::Foundation::IInspectable originalAlignment{nullptr};
    double rowHeight = 0;
    double previousHeight = 0;
    xaml::VerticalAlignment previousAlignment = xaml::VerticalAlignment::Stretch;
    xaml::VerticalAlignment rowAlignment = xaml::VerticalAlignment::Stretch;

    bool Move(IconOffset pixels, UINT dpi) noexcept try {
        return translation.Move(frame.get(), pixels.x * 96.0 / dpi, pixels.y * 96.0 / dpi);
    } catch (const winrt::hresult_error&) {
        return false;
    }

    bool SetRow(double height, bool top = false) noexcept try {
        const auto target = frame.get();
        if (!target) return false;
        const auto heightProperty = xaml::FrameworkElement::HeightProperty();
        const auto alignmentProperty = xaml::FrameworkElement::VerticalAlignmentProperty();
        if (height) {
            originalHeight = target.ReadLocalValue(heightProperty);
            originalAlignment = target.ReadLocalValue(alignmentProperty);
            previousHeight = target.Height();
            previousAlignment = target.VerticalAlignment();
            rowHeight = height;
            rowAlignment = top ? xaml::VerticalAlignment::Top : xaml::VerticalAlignment::Bottom;
            target.Height(height);
            target.VerticalAlignment(rowAlignment);
        } else if (rowHeight) {
            const auto restore = [&](const auto& property, const auto& original, auto fallback) {
                try {
                    if (original == xaml::DependencyProperty::UnsetValue()) target.ClearValue(property);
                    else target.SetValue(property, original);
                } catch (const winrt::hresult_error&) {
                    fallback();
                }
            };
            if (target.Height() == rowHeight)
                restore(heightProperty, originalHeight, [&] { target.Height(previousHeight); });
            if (target.VerticalAlignment() == rowAlignment)
                restore(alignmentProperty, originalAlignment, [&] { target.VerticalAlignment(previousAlignment); });
            rowHeight = 0;
            originalHeight = nullptr;
            originalAlignment = nullptr;
        }
        return true;
    } catch (const winrt::hresult_error&) {
        return false;
    }
};

enum class GripPhase { idle, pressed, capturing, dragging };

struct Grip {
    xaml::Input::Pointer pointer{nullptr};
    winrt::weak_ref<xaml::DependencyObject> source;
    appearance::IconGroup group = appearance::IconGroup::apps;
    GripPhase phase = GripPhase::idle;
    POINT origin{};
    LONG last = 0;
    winrt::weak_ref<xaml::FrameworkElement> button;
    winrt::Windows::Foundation::IInspectable originalClickMode{nullptr};
    MotionAxis axis = MotionAxis::horizontal;
};

struct SuspendedPins {
    appearance::IconOffsets offsets;
    appearance::TaskbarLayout layout;
};

// These fields are used on the taskbar's own XAML thread.
struct AppearanceState {
    HMODULE module = nullptr;
    HWND control = nullptr;
    HWND host = nullptr;
    DWORD hostProcess = 0;
    HANDLE hostHandle = nullptr;
    PTP_WAIT wait = nullptr;
    bool enabled = false;
    bool highContrast = false;
    bool connecting = false;
    bool connected = false;
    bool layoutPending = false;
    bool layoutMeasuring = false;
    bool layoutDirty = false;
    unsigned layoutGeneration = 0;
    appearance::Ink ink = appearance::Ink::system;
    appearance::LayerMode layerMode = appearance::LayerMode::native;
    MotionAxis wheelAxis = MotionAxis::horizontal;
    appearance::MotionProfile motionProfile;
    std::uint64_t motionPacket = 0;
    IconFrame apps;
    IconFrame notification;
    appearance::IconMotion appsMotion;
    appearance::IconMotion notificationMotion;
    appearance::IconWheelGesture wheelGesture;
    appearance::IconGroup wheelGroup = appearance::IconGroup::notification;
    HHOOK inputHook = nullptr;
    std::optional<POINT> inputCursor;
    UINT wheelSequence = 0;
    appearance::PinInput pinInput;
    appearance::IconLatch appsLatch;
    appearance::IconLatch notificationLatch;
    std::optional<appearance::IconGroup> peeling;
    Grip grip;
    MotionClock::time_point lastMotionFrame;
    winrt::event_token rendering{};
    bool renderingRequested = false;
    bool motionFrameQueued = false;
    UINT motionInterval = 0;
    ULONGLONG motionRetryUntil = 0;
    appearance::IconOffsets offsets;
    std::optional<SuspendedPins> suspendedPins;
    ULONGLONG pinRecoveryAt = 0;
    appearance::TaskbarLayout layout;
    winrt::event_token layoutChanged{};
    appearance::TaskbarIconInk iconInk;
    fullwintb::appearance::TaskbarRegion region;
    fullwintb::appearance::TaskbarLayer layer;
    std::vector<Background> backgrounds;
    std::vector<ThemeRoot> themeRoots;
};

AppearanceState state;

struct LayoutResult {
    fullwintb::appearance::TaskbarLayout layout;
    unsigned generation = 0;
    appearance::IconOffsets offsets;
};

SRWLOCK layoutLock = SRWLOCK_INIT;
LayoutResult nextLayout;

appearance::IconMotion& Motion(appearance::IconGroup group) {
    return group == appearance::IconGroup::apps ? state.appsMotion : state.notificationMotion;
}

IconFrame& Frame(appearance::IconGroup group) {
    return group == appearance::IconGroup::apps ? state.apps : state.notification;
}

appearance::IconLatch& Latch(appearance::IconGroup group) {
    return group == appearance::IconGroup::apps ? state.appsLatch : state.notificationLatch;
}

void AnchorWave(appearance::IconGroup group, MotionAxis axis, POINT cursor) {
    auto& frame = Frame(group);
    const auto displacement = state.offsets.For(group);
    frame.wave.Anchor(group, frame.frame.get(), state.layout, displacement, axis, cursor, MotionClock::now());
    cursor.x -= displacement.x;
    cursor.y -= displacement.y;
    frame.returnAnchor = cursor;
}

void WakeReturn(appearance::IconGroup group, ULONGLONG now, MotionClock::time_point frameTime) {
    if (!Motion(group).Wake(now)) return;
    auto& frame = Frame(group);
    if (auto anchor = std::exchange(frame.returnAnchor, std::nullopt)) {
        const auto displacement = state.offsets.For(group);
        anchor->x += displacement.x;
        anchor->y += displacement.y;
        frame.wave.Anchor(group, frame.frame.get(), state.layout, displacement, frame.wave.Axis(), *anchor, frameTime);
    }
}

appearance::IconSlices WaveSlices(appearance::IconOffsets offsets, bool planned) {
    return {state.apps.wave.Slices(offsets.apps, planned), state.notification.wave.Slices(offsets.notification, planned)};
}

void EndGrip() noexcept try {
    const auto grip = std::exchange(state.grip, {});
    if (grip.phase != GripPhase::dragging && grip.phase != GripPhase::capturing) return;
    Motion(grip.group).EndDrag(true);
    if (grip.originalClickMode) {
        appearance::RestoreStartClick(grip.button.get(), grip.originalClickMode);
        return;
    }
    if (const auto frame = Frame(grip.group).frame.get(); frame && grip.pointer)
        frame.ReleasePointerCapture(grip.pointer);
} catch (const winrt::hresult_error&) {
    // Explorer can end pointer capture by removing the frame.
}

bool Animating() {
    return state.appsMotion.Moving() || state.notificationMotion.Moving();
}

bool NeedsMotionFrame() {
    return Animating() || (state.peeling != appearance::IconGroup::apps && state.apps.wave.Pending()) ||
           (state.peeling != appearance::IconGroup::notification && state.notification.wave.Pending()) ||
           state.region.MotionPending() || state.motionRetryUntil;
}

void RestoreIcons();
void RecoverPinnedLayout();

bool CanvasLost() {
    return state.layer.CanvasOpen() && (!state.layer.CanvasIntact() ||
        FindWindowW(L"Shell_SecondaryTrayWnd", nullptr));
}

void ScheduleLayout() {
    if (state.enabled && state.layerMode == appearance::LayerMode::floating &&
        !state.highContrast && !state.layoutPending && !state.peeling && !NeedsMotionFrame())
        state.layoutPending = SetTimer(state.control, layoutTimer, 105, nullptr) != 0;
}

void MeasureLayout() {
    if (!state.enabled || state.layerMode != appearance::LayerMode::floating ||
        state.highContrast || state.peeling || NeedsMotionFrame())
        return;
    if (CanvasLost()) {
        RestoreIcons();
        return;
    }
    if (state.suspendedPins) {
        RecoverPinnedLayout();
        return;
    }
    if (state.layoutMeasuring) {
        state.layoutDirty = true;
        return;
    }
    struct Request {
        HWND bar;
        HWND control;
        unsigned generation;
        appearance::IconOffsets offsets;
    };
    auto* request = new (std::nothrow) Request{FindWindowW(L"Shell_TrayWnd", nullptr), state.control,
                                               state.layoutGeneration, state.offsets};
    if (!request) return;
    state.layoutMeasuring = TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE, void* data) noexcept {
        const std::unique_ptr<Request> task(static_cast<Request*>(data));
        auto layout = fullwintb::appearance::ReadTaskbarLayout(task->bar, task->offsets.apps);
        AcquireSRWLockExclusive(&layoutLock);
        nextLayout = LayoutResult{std::move(layout), task->generation, task->offsets};
        ReleaseSRWLockExclusive(&layoutLock);
        PostMessageW(task->control, layoutReady, 0, 0);
    }, request, nullptr) != FALSE;
    if (!state.layoutMeasuring) delete request;
}

void PublishOffsets(HWND bar, appearance::IconOffsets previous) {
    if (!bar) return;
    const auto publish = [bar](const wchar_t* name, LONG pixels) {
        if (pixels) SetPropW(bar, name, reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(pixels)));
        else RemovePropW(bar, name);
    };
    if (previous.apps != state.offsets.apps) {
        const auto next = state.offsets.apps;
        publish(appearance::appsOffsetProperty, next.y ? next.y : next.x);
        publish(appearance::appsOffsetX, next.x);
        publish(appearance::appsOffsetY, next.y);
    }
    if (previous.notification != state.offsets.notification) {
        const auto next = state.offsets.notification;
        publish(appearance::notificationOffsetProperty, next.y ? next.y : next.x);
        publish(appearance::notificationOffsetX, next.x);
        publish(appearance::notificationOffsetY, next.y);
    }
}

void FollowPopups() {
    const HWND bar = state.layerMode == appearance::LayerMode::floating ? state.layout.bar : nullptr;
    const auto appsOffset = state.apps.wave.AnchorOffset(state.offsets.apps);
    appearance::FollowTaskbarThumbnails(appsOffset.y ? bar : nullptr, appsOffset.y);
    const auto trayOffset = state.notification.wave.GripOffset(state.offsets.notification);
    appearance::FollowTrayOverflow(bar, state.layout.dock, trayOffset.x, trayOffset.y,
                                  trayOffset ? state.notification.frame.get() : nullptr);
}

enum class IconWrite { applied, retry, failed };

IconWrite MoveIcons(appearance::IconOffsets next, bool animated, bool positionChanged = true) {
    const auto previous = state.offsets;
    const auto& layout = state.layout;
    const auto apply = [&](appearance::IconOffsets offsets, bool planned) {
        const bool apps = next.apps == previous.apps || state.apps.Move(offsets.apps, layout.dpi);
        const bool tray = next.notification == previous.notification ||
                          state.notification.Move(offsets.notification, layout.dpi);
        const bool buttons = state.apps.wave.Apply(offsets.apps, layout.dpi, planned);
        const bool icons = state.notification.wave.Apply(offsets.notification, layout.dpi, planned);
        return apps && tray && buttons && icons;
    };
    const bool translated = apply(next, true);
    if (positionChanged) ++state.layoutGeneration;
    if (!translated || (positionChanged && !state.region.Place(layout, next, animated, WaveSlices(next, true)))) {
        // A rejected region write leaves the previous clip installed. Roll back
        // the offsets and give Explorer another frame while we still own it.
        const bool rolledBack = apply(previous, false);
        if (translated && rolledBack && state.region.OwnsCurrent() && !CanvasLost()) return IconWrite::retry;
        return IconWrite::failed;
    }
    state.offsets = next;
    state.apps.wave.Commit();
    state.notification.wave.Commit();
    if (positionChanged) {
        PublishOffsets(layout.bar, previous);
        FollowPopups();
    }
    return IconWrite::applied;
}

void CloseVerticalCanvas() {
    if (!state.layer.CanvasOpen() && !state.apps.rowHeight && !state.notification.rowHeight) return;
    state.apps.SetRow(0);
    state.notification.SetRow(0);
    state.layer.CloseCanvas();
    RECT bounds{};
    if (GetWindowRect(state.layout.bar, &bounds) && EqualRect(&bounds, &state.layout.dock)) {
        state.layout.bounds = bounds;
        if (!state.region.Rebase(state.layout, {})) {
            state.region.Reset();
            state.region.Update(state.layout, {}, true);
        }
    } else {
        state.region.Reset();
        state.layout = {};
    }
    ++state.layoutGeneration;
}

void SetMotionInterval(UINT interval) {
    if (state.motionInterval == interval) return;
    if (!interval) KillTimer(state.control, motionTimer);
    state.motionInterval = interval && SetTimer(state.control, motionTimer, interval, nullptr) ? interval : 0;
}

void StopRendering() {
    if (state.rendering) xaml::Media::CompositionTarget::Rendering(state.rendering);
    state.rendering = {};
    state.renderingRequested = false;
    state.lastMotionFrame = {};
}

bool CanArmFromStartDrag() {
    return state.layerMode == appearance::LayerMode::floating && state.grip.phase == GripPhase::dragging &&
        state.grip.group == appearance::IconGroup::apps && !state.appsMotion.Pinned();
}

UINT PinArmDuration() { return std::max<UINT>(750, state.motionProfile.hold); }

bool AwaitingReturn() {
    return state.appsMotion.WaitingToReturn() || state.notificationMotion.WaitingToReturn();
}

void StopInputHook() {
    if (const auto hook = std::exchange(state.inputHook, nullptr)) UnhookWindowsHookEx(hook);
    state.inputCursor.reset();
}

void StopIdleInputHook() {
    if (!state.wheelGesture.Active() && !state.pinInput.Active() && !CanArmFromStartDrag() && !AwaitingReturn())
        StopInputHook();
}

void EndIconWheel() {
    state.wheelGesture = {};
    ++state.wheelSequence;
    StopIdleInputHook();
}

void PauseMotion() {
    appearance::FollowTaskbarThumbnails(nullptr, 0);
    appearance::FollowTrayOverflow(nullptr, {}, 0, 0);
    EndGrip();
    state.pinInput.Reset();
    EndIconWheel();
    StopInputHook();
    for (const auto group : iconGroups) Motion(group).WatchReturn(0, 0, true);
    state.peeling.reset();
    StopRendering();
    SetMotionInterval(0);
    state.motionRetryUntil = 0;
}

void RestoreIcons() {
    if (state.suspendedPins) state.layer.HoldPresentation(true);
    PauseMotion();
    state.appsMotion = {};
    state.notificationMotion = {};
    state.apps.returnAnchor.reset();
    state.notification.returnAnchor.reset();
    state.apps.stickerDismissed = false;
    state.notification.stickerDismissed = false;
    state.appsLatch.Hide();
    state.notificationLatch.Hide();
    const auto previous = state.offsets;
    const auto slices = WaveSlices(previous, false);
    const bool moved = previous != appearance::IconOffsets{} || !slices.apps.slices.empty() || !slices.notification.slices.empty();
    state.apps.wave.Clear();
    state.notification.wave.Clear();
    const bool finishingRegion = state.region.MotionPending();
    if (!(moved ? MoveIcons({}, false) == IconWrite::applied : state.region.FinishMotion())) {
        state.apps.Move({}, state.layout.dpi);
        state.notification.Move({}, state.layout.dpi);
        state.offsets = {};
        PublishOffsets(state.layout.bar, previous);
        if (!state.layer.CanvasOpen()) state.region.Reset();
        ++state.layoutGeneration;
    }
    const bool hadCanvas = state.layer.CanvasOpen();
    CloseVerticalCanvas();
    if (moved || hadCanvas || finishingRegion) ScheduleLayout();
}

void InvalidatePinnedLayout() {
    RestoreIcons();
    state.region.Reset();
    state.layout = {};
}

bool PrepareVerticalTarget(LONG wanted, LONG& target) {
    const bool open = state.layer.CanvasOpen();
    if (open && CanvasLost()) {
        RestoreIcons();
        return false;
    }
    auto& layout = state.layout;
    MONITORINFO monitor{sizeof(monitor)};
    const HMONITOR display = MonitorFromWindow(layout.bar, MONITOR_DEFAULTTONULL);
    if (!display || !GetMonitorInfoW(display, &monitor)) return false;
    const bool top = layout.dock.top == monitor.rcMonitor.top;
    const bool bottom = layout.dock.bottom == monitor.rcMonitor.bottom;
    if (!top && !bottom) return false;
    const LONG height = layout.dock.bottom - layout.dock.top;
    const LONG room = monitor.rcMonitor.bottom - monitor.rcMonitor.top - height;
    if (room <= 0) return false;
    target = top ? std::clamp(wanted, 0L, room) : std::clamp(wanted, -room, 0L);
    const LONG extent = layout.bounds.bottom - layout.bounds.top - height;
    if (open && std::abs(target) <= extent) return true;
    if (!target || !state.region.OwnsCurrent()) return false;
    // Grow the backing surface as needed instead of reserving an entire
    // monitor for one wheel step. The monitor edges are the travel limits.
    const LONG headroom = MulDiv(24, layout.dpi, 96);
    const LONG initial = MulDiv(116, layout.dpi, 96);
    const LONG travel = std::min(room, std::max({initial, extent * 2, std::abs(target) + headroom}));
    const double row = height * 96.0 / layout.dpi;
    if ((!open && (!state.apps.SetRow(row, top) || !state.notification.SetRow(row, top))) ||
        !state.layer.EnsureCanvas(travel, top)) {
        if (!open || CanvasLost() || !state.layer.CanvasOpen()) RestoreIcons();
        return false;
    }
    if (!GetWindowRect(layout.bar, &layout.bounds) ||
        !state.region.Rebase(layout, state.offsets, WaveSlices(state.offsets, false))) {
        RestoreIcons();
        return false;
    }
    ++state.layoutGeneration;
    return true;
}

RECT StartDisplayBounds() {
    auto bounds = state.layout.startButton;
    const auto offset = state.apps.wave.GripOffset(state.offsets.apps);
    OffsetRect(&bounds, offset.x, offset.y);
    return bounds;
}

bool AppsPushedByTray() { return state.offsets.apps.x < state.appsMotion.Position().x; }

void UpdateLatches() {
    const bool visible = state.layerMode == appearance::LayerMode::floating &&
        !state.layout.buttons.empty() && IsWindowVisible(state.layout.bar);
    for (const auto group : iconGroups) {
        auto& handle = Latch(group);
        const auto& motion = Motion(group);
        auto& frame = Frame(group);
        if (!state.offsets.For(group)) frame.stickerDismissed = false;
        const bool pushed = group == appearance::IconGroup::apps && AppsPushedByTray() &&
            state.notificationMotion.ShowsLatch();
        if (visible && !frame.stickerDismissed && (motion.ShowsLatch() || pushed) &&
            (state.offsets.For(group) || motion.Dragging())) {
            RECT anchor = appearance::TaskbarRegion::Strip(state.layout, state.offsets, group);
            bool corner = false;
            if (group == appearance::IconGroup::apps) {
                anchor = StartDisplayBounds();
                corner = true;
            } else {
                if (const auto chevron = appearance::TrayChevronBounds(state.layout.bar, state.layout.dpi,
                                                                       state.notification.frame.get())) {
                    anchor = *chevron;
                    corner = true;
                }
            }
            handle.Show(state.module, state.layout.bar, state.control, group, anchor,
                        state.layout.dpi, state.layout.monitor, corner, motion.Pinned());
        } else {
            handle.Hide();
        }
    }
}

bool PinsInPlace(HWND bar, const SuspendedPins& pins) noexcept try {
    const auto& layout = state.layout;
    const auto& saved = pins.layout;
    RECT bounds{}, dock{};
    MONITORINFO display{sizeof(display)};
    if (!bar || bar != layout.bar || GetDpiForWindow(bar) != saved.dpi ||
        !GetWindowRect(bar, &bounds) || !EqualRect(&layout.bounds, &bounds) ||
        !appearance::ReadDockBounds(bar, dock) || !EqualRect(&saved.dock, &dock) ||
        !GetMonitorInfoW(MonitorFromRect(&dock, MONITOR_DEFAULTTONULL), &display) ||
        !EqualRect(&saved.monitor, &display.rcMonitor) || CanvasLost() || !state.region.OwnsCurrent() ||
        appearance::TaskbarRegion::Travel(layout).Project(pins.offsets) != state.offsets) return false;
    for (const auto group : iconGroups) {
        if (pins.offsets.For(group) && !Motion(group).Pinned()) return false;
        const auto& icons = Frame(group);
        if (state.offsets.For(group) && !icons.translation.Owns(icons.frame.get())) return false;
    }
    return true;
} catch (const winrt::hresult_error&) {
    return false;
}

void StopPinRecoveryTimer() {
    KillTimer(state.control, pinRecoveryTimer);
    state.pinRecoveryAt = 0;
}

void FinishPinRecovery() {
    StopPinRecoveryTimer();
    state.suspendedPins.reset();
    state.layer.HoldPresentation(false);
}

void RecoverPinnedLayout() {
    const auto& saved = *state.suspendedPins;
    auto current = appearance::ReadTaskbarGeometry(FindWindowW(L"Shell_TrayWnd", nullptr));
    if (!current.bar || !state.apps.frame.get() || !state.notification.frame.get()) {
        ScheduleLayout();
        return;
    }
    if (saved.layout.dpi != current.dpi || !EqualRect(&saved.layout.dock, &current.dock) ||
        !EqualRect(&saved.layout.monitor, &current.monitor)) {
        FinishPinRecovery();
        ScheduleLayout();
        return;
    }
    // The accessibility provider may withhold button bounds while the bar
    // is hidden. Reuse the last visible spans until a fresh visible read.
    current.buttons = saved.layout.buttons;
    current.startButton = saved.layout.startButton;
    state.layout = std::move(current);
    state.region.Update(state.layout, {}, true);
    if (!state.region.Installed()) {
        ScheduleLayout();
        return;
    }
    const LONG farthest = std::abs(saved.offsets.apps.y) >= std::abs(saved.offsets.notification.y)
        ? saved.offsets.apps.y : saved.offsets.notification.y;
    if (farthest) {
        LONG allowed = 0;
        if (!PrepareVerticalTarget(farthest, allowed)) {
            InvalidatePinnedLayout();
            ScheduleLayout();
            return;
        }
    }
    const auto offsets = appearance::TaskbarRegion::Travel(state.layout).Project(saved.offsets);
    if (MoveIcons(offsets, false) != IconWrite::applied) {
        InvalidatePinnedLayout();
        ScheduleLayout();
        return;
    }
    for (const auto group : iconGroups)
        if (saved.offsets.For(group) && offsets.For(group)) Motion(group).TogglePin(offsets.For(group));
    FinishPinRecovery();
    FollowPopups();
    UpdateLatches();
    ScheduleLayout();
}

bool EnsureIconInput();

bool WatchReturnPointer(POINT cursor, ULONGLONG now) {
    if (!AwaitingReturn() || state.layerMode != appearance::LayerMode::floating) return false;
    const auto appsArea = appearance::TaskbarRegion::ReturnArea(state.layout, state.offsets, appearance::IconGroup::apps);
    const bool inApps = PtInRect(&appsArea, cursor) ||
                        Latch(appearance::IconGroup::apps).ContainsPointer(cursor);
    const auto trayArea = appearance::TaskbarRegion::ReturnArea(state.layout, state.offsets,
                                                             appearance::IconGroup::notification);
    // While the tray holds the app strip to its left, returning the tray also
    // moves the apps. Keep the tray parked in either group's cleared area.
    const bool inTray = PtInRect(&trayArea, cursor) ||
                        Latch(appearance::IconGroup::notification).ContainsPointer(cursor) ||
                        (inApps && AppsPushedByTray());
    const bool appsChanged = state.appsMotion.WatchReturn(now, state.motionProfile.hold, inApps);
    const bool trayChanged = state.notificationMotion.WatchReturn(now, state.motionProfile.hold, inTray);
    return appsChanged || trayChanged;
}

void RefreshReturnPointer(ULONGLONG now) {
    if (!AwaitingReturn()) return;
    POINT cursor{};
    if (state.inputCursor) cursor = *state.inputCursor;
    else if (!GetPhysicalCursorPos(&cursor)) return;
    WatchReturnPointer(cursor, now);
}

void QueueMotion(ULONGLONG now) {
    if (state.layerMode != appearance::LayerMode::floating) return;
    const bool pollReturn = AwaitingReturn() && !EnsureIconInput();
    RefreshReturnPointer(now);
    UINT delay = state.wheelGesture.Remaining(now);
    if (!delay && state.wheelGesture.Active()) EndIconWheel();
    state.pinInput.Expire(now);
    const auto pinWait = state.pinInput.Remaining(now);
    if (pinWait && (!delay || pinWait < delay)) delay = pinWait;
    for (const auto group : iconGroups) {
        WakeReturn(group, now, MotionClock::now());
        const UINT wait = Motion(group).ReturnDelay(now);
        if (wait && (!delay || wait < delay)) delay = wait;
    }
    StopIdleInputHook();
    if (pollReturn && (!delay || delay > 40)) delay = 40;
    if (NeedsMotionFrame()) {
        if (!state.renderingRequested) {
            state.renderingRequested = true;
            state.lastMotionFrame = MotionClock::now();
            try {
                state.rendering = xaml::Media::CompositionTarget::Rendering([](const auto&, const auto&) {
                    if (!state.motionFrameQueued)
                        state.motionFrameQueued = PostMessageW(state.control, motionFrame, 0, 0) != FALSE;
                });
            } catch (const winrt::hresult_error&) {
                // Older or restarting XAML hosts can still animate using a timer.
            }
            SetMotionInterval(state.rendering ? renderingWatchdog : USER_TIMER_MINIMUM);
        }
        return;
    }
    StopRendering();
    SetMotionInterval(delay);
}

void StepMotion() {
    if (state.layerMode != appearance::LayerMode::floating) return;
    if (CanvasLost()) {
        RestoreIcons();
        return;
    }
    const auto now = GetTickCount64();
    const auto frame = MotionClock::now();
    RefreshReturnPointer(now);
    const double elapsed = state.renderingRequested
        ? std::chrono::duration<double>(frame - state.lastMotionFrame).count() : 0.0;
    state.lastMotionFrame = frame;
    const auto travel = appearance::TaskbarRegion::Travel(state.layout);
    const double scale = state.layout.dpi / 96.0;
    appearance::IconOffsets freePositions;
    for (const auto group : iconGroups) {
        if (state.peeling == group) {
            freePositions.For(group) = state.offsets.For(group);
            continue;
        }
        WakeReturn(group, now, frame);
        auto& motion = Motion(group);
        motion.ConstrainTarget(travel.minimum.For(group), travel.maximum.For(group));
        motion.Step(elapsed, scale, state.motionProfile);
        freePositions.For(group) = motion.Limit(travel.minimum.For(group), travel.maximum.For(group),
                                               state.motionProfile.returnStyle);
    }
    // The tray leads at contact. A displaced app strip retains its own spring
    // position so it can return there as the tray releases it.
    auto next = travel.Project(freePositions);
    const bool sharedRow = travel.ShareRow(next);
    auto trayMinimum = travel.minimum.notification;
    if (sharedRow) trayMinimum.x = std::max(trayMinimum.x, travel.minimum.apps.x - travel.clearance);
    if (state.peeling != appearance::IconGroup::notification)
        state.notification.wave.Plan(next.notification, trayMinimum, travel.maximum.notification, frame);
    auto appsMaximum = travel.maximum.apps;
    if (sharedRow) {
        appsMaximum.x = std::clamp(state.notification.wave.LeadingX(next.notification) + travel.clearance,
                                   travel.minimum.apps.x, appsMaximum.x);
        next.apps.x = std::min(next.apps.x, appsMaximum.x);
    }
    if (state.peeling != appearance::IconGroup::apps)
        state.apps.wave.Plan(next.apps, travel.minimum.apps, appsMaximum, frame);
    const bool changed = next != state.offsets || state.apps.wave.PositionChanged() ||
                         state.notification.wave.PositionChanged();
    const bool tilt = state.apps.wave.TiltChanged() || state.notification.wave.TiltChanged();
    auto written = IconWrite::applied;
    if (changed || tilt) written = MoveIcons(next, true, changed);
    if (!changed && written == IconWrite::applied && !state.region.FinishMotion())
        written = state.region.OwnsCurrent() ? IconWrite::retry : IconWrite::failed;
    if (written == IconWrite::retry) {
        if (!state.motionRetryUntil) state.motionRetryUntil = now + motionRetryWindow;
        if (now < state.motionRetryUntil) {
            QueueMotion(now);
            return;
        }
    }
    if (written != IconWrite::applied) {
        RestoreIcons();
        return;
    }
    state.motionRetryUntil = 0;
    if (changed) UpdateLatches();
    if (!NeedsMotionFrame()) {
        for (const auto group : iconGroups) {
            if (!Motion(group).Dragging()) Frame(group).wave.Clear();
        }
        ScheduleLayout();
    }
    QueueMotion(now);
}

bool ProjectMotionTarget(appearance::IconGroup group, MotionAxis axis, IconOffset& target) {
    if (axis == MotionAxis::vertical && target.y && !PrepareVerticalTarget(target.y, target.y)) return false;
    auto positions = state.offsets;
    positions.For(group) = target;
    target = appearance::TaskbarRegion::Travel(state.layout).Project(positions).For(group);
    return true;
}

bool AimMotion(appearance::IconGroup group, MotionAxis axis, LONG delta) {
    if (!delta || state.peeling == group || state.layerMode != appearance::LayerMode::floating ||
        !state.region.Installed()) return false;
    auto& motion = Motion(group);
    const auto drawn = state.offsets.For(group);
    const bool pushed = group == appearance::IconGroup::apps && AppsPushedByTray();
    auto start = motion.Target();
    if (pushed) start.x = drawn.x;
    auto target = start;
    target.Along(axis) += delta;
    if (!ProjectMotionTarget(group, axis, target) || (target == start && (pushed || !target))) return false;
    if (pushed) motion.SetPosition(drawn);
    Frame(group).stickerDismissed = false;
    motion.Aim(target);
    return true;
}

bool ScrollMotion(appearance::IconGroup group, int delta, std::optional<POINT> anchor) {
    if (!delta || state.peeling == group || state.layerMode != appearance::LayerMode::floating ||
        !state.region.Installed()) return false;
    auto& motion = Motion(group);
    const auto& profile = state.motionProfile;
    const MotionAxis axis = state.wheelAxis;
    const LONG step = -MulDiv(delta, MulDiv(profile.wheelStep, state.layout.dpi, 96), WHEEL_DELTA);
    if (!step) return false;
    if (motion.Pinned() || motion.Dragging()) {
        if (!AimMotion(group, axis, step)) return false;
        if (anchor) AnchorWave(group, axis, *anchor);
        return true;
    }

    const auto drawn = state.offsets.For(group);
    const bool pushed = group == appearance::IconGroup::apps && AppsPushedByTray();
    auto target = motion.WheelOrigin();
    if (pushed) target.x = drawn.x;
    target.Along(axis) += step;
    if (!ProjectMotionTarget(group, axis, target) || (target == drawn && !target)) return false;
    if (pushed) motion.SetPosition(drawn);
    if (anchor) AnchorWave(group, axis, *anchor);
    Frame(group).stickerDismissed = false;
    motion.ScrollTo(target, profile);
    return true;
}

bool ContinuesIconWheel(POINT cursor, ULONGLONG now) {
    const LONG tolerance = MulDiv(5, state.layout.dpi, 96);
    return state.wheelGesture.Continues(cursor, now, tolerance);
}

bool CanPinAt(POINT cursor) {
    const auto& pin = state.pinInput;
    if (!pin.Active() || state.peeling ||
        state.layerMode != appearance::LayerMode::floating || !state.offsets.For(pin.Group()) ||
        Motion(pin.Group()).Pinned()) return false;
    if (pin.Source() == appearance::PinInput::Origin::wheel) {
        const LONG tolerance = MulDiv(2, state.layout.dpi, 96);
        const auto anchor = pin.Anchor();
        return std::abs(cursor.x - anchor.x) <= tolerance && std::abs(cursor.y - anchor.y) <= tolerance;
    }
    const auto start = StartDisplayBounds();
    return PtInRect(&start, cursor) && GetAncestor(WindowFromPhysicalPoint(cursor), GA_ROOT) == state.layout.bar;
}

LRESULT CALLBACK RouteIconInput(int code, WPARAM kind, LPARAM data) {
    if (code == HC_ACTION && state.inputHook) {
        const auto& input = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        if (input.dwExtraInfo == appearance::PinInput::replayTag) return CallNextHookEx(nullptr, code, kind, data);
        const auto now = GetTickCount64();
        state.inputCursor = input.pt;
        if (kind == WM_MOUSEMOVE && WatchReturnPointer(input.pt, now))
            PostMessageW(state.control, motionInputChanged, 0, 0);
        const bool wheel = kind == WM_MOUSEWHEEL;
        const bool pressed = kind == WM_LBUTTONDOWN || kind == WM_RBUTTONDOWN ||
                             kind == WM_MBUTTONDOWN || kind == WM_XBUTTONDOWN;
        if (kind == WM_LBUTTONUP && CanArmFromStartDrag() && !state.pinInput.Active()) {
            // The next tap can arrive before the XAML release handler runs.
            state.pinInput.Arm(appearance::IconGroup::apps, appearance::PinInput::Origin::startDrag,
                               input.pt, now, PinArmDuration());
            PostMessageW(state.control, motionInputChanged, 0, 0);
        }
        if (state.pinInput.Active() && (pressed || kind == WM_LBUTTONUP || kind == WM_MOUSEMOVE)) {
            const bool tap = kind == WM_LBUTTONDOWN || kind == WM_LBUTTONUP;
            const auto result = state.pinInput.Route(static_cast<UINT>(kind), input.pt,
                                                      tap && CanPinAt(input.pt), state.layout.dpi, now);
            if (pressed && state.wheelGesture.Active()) EndIconWheel();
            if (result.pin)
                PostMessageW(state.control, appearance::latchMessage,
                             MAKEWPARAM(static_cast<UINT>(*result.pin),
                                        static_cast<UINT>(appearance::LatchGesture::pin)), 0);
            else if (result.changed) PostMessageW(state.control, motionInputChanged, 0, 0);
            if (result.consumed) return 1;
        } else if (pressed && state.wheelGesture.Active()) {
            EndIconWheel();
        } else if (wheel && state.wheelGesture.Active()) {
            if (!ContinuesIconWheel(input.pt, now)) {
                EndIconWheel();
            } else if (PostMessageW(state.control, iconWheel, state.wheelSequence,
                                   GET_WHEEL_DELTA_WPARAM(input.mouseData))) {
                state.wheelGesture.Extend(now);
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, kind, data);
}

bool EnsureIconInput() {
    if (!state.inputHook) {
        state.inputHook = SetWindowsHookExW(WH_MOUSE_LL, RouteIconInput, state.module, 0);
        POINT cursor{};
        if (state.inputHook && GetPhysicalCursorPos(&cursor)) state.inputCursor = cursor;
    }
    return state.inputHook != nullptr;
}

void ArmPin(appearance::IconGroup group, appearance::PinInput::Origin origin, POINT cursor, ULONGLONG now) {
    if (state.layerMode == appearance::LayerMode::floating &&
        !Motion(group).Pinned() && !Motion(group).Dragging() && !state.peeling && EnsureIconInput())
        state.pinInput.Arm(group, origin, cursor, now, PinArmDuration());
}

void TrackIconWheel(appearance::IconGroup group, POINT cursor, ULONGLONG now) {
    // XAML islands can receive wheel input without exposing a wheel message on
    // their Win32 queue. Keep consecutive detents with the group that started them.
    if (!EnsureIconInput()) return;
    if (group == state.wheelGroup && ContinuesIconWheel(cursor, now)) {
        state.wheelGesture.Extend(now);
    } else {
        ++state.wheelSequence;
        state.wheelGroup = group;
        state.wheelGesture.Start(cursor, now);
    }
    ArmPin(group, appearance::PinInput::Origin::wheel, cursor, now);
}

void HandleLatch(appearance::IconGroup group, appearance::LatchGesture gesture, LONG delta,
                 std::optional<POINT> anchor = std::nullopt, std::optional<MotionAxis> direction = std::nullopt) {
    if (state.layerMode != appearance::LayerMode::floating) return;
    auto& motion = Motion(group);
    auto& frame = Frame(group);
    const auto now = GetTickCount64();
    const auto anchorWave = [&](MotionAxis axis) {
        POINT cursor{};
        if (anchor) AnchorWave(group, axis, *anchor);
        else if (GetPhysicalCursorPos(&cursor)) AnchorWave(group, axis, cursor);
    };
    const MotionAxis axis = direction.value_or(state.wheelAxis);
    switch (gesture) {
    case appearance::LatchGesture::toggle:
        if (motion.Pinned()) {
            Latch(group).PeelOff();
            return;
        }
        if (state.peeling == group) state.peeling.reset();
        frame.stickerDismissed = false;
        motion.TogglePin(state.offsets.For(group));
        UpdateLatches();
        break;
    case appearance::LatchGesture::pin:
        if (!motion.Pinned() && state.offsets.For(group)) {
            frame.stickerDismissed = false;
            motion.TogglePin(state.offsets.For(group));
            UpdateLatches();
        }
        break;
    case appearance::LatchGesture::dragStart:
        if (state.peeling == group) return;
        frame.stickerDismissed = false;
        anchorWave(axis);
        motion.BeginDrag(state.offsets.For(group));
        break;
    case appearance::LatchGesture::drag:
        if (AimMotion(group, axis, delta)) anchorWave(axis);
        break;
    case appearance::LatchGesture::dragEnd:
        motion.EndDrag(true);
        break;
    case appearance::LatchGesture::peelStart:
        if (state.peeling || !state.offsets.For(group)) return;
        state.pinInput.Reset();
        EndIconWheel();
        state.peeling = group;
        motion.BeginDrag(state.offsets.For(group));
        frame.wave.HoldFrame();
        break;
    case appearance::LatchGesture::peelEnd:
        if (state.peeling != group) return;
        state.peeling.reset();
        motion.EndDrag(true);
        ScheduleLayout();
        break;
    case appearance::LatchGesture::peelRemove:
        if (state.peeling == group) state.peeling.reset();
        else if (!motion.Pinned()) return;
        frame.stickerDismissed = true;
        anchorWave(frame.wave.Axis());
        motion.Release();
        UpdateLatches();
        break;
    }
    QueueMotion(now);
}

void ScrollIcons(appearance::IconGroup group, const xaml::Input::PointerRoutedEventArgs& args) noexcept try {
    if (state.layerMode != appearance::LayerMode::floating || args.Handled()) return;
    const auto wheel = args.GetCurrentPoint(nullptr).Properties();
    if (wheel.IsHorizontalMouseWheel()) return;
    const bool notification = group == appearance::IconGroup::notification;
    const auto now = GetTickCount64();
    POINT cursor{};
    const bool track = GetPhysicalCursorPos(&cursor);
    if (notification && (!track || !ContinuesIconWheel(cursor, now))) {
        const auto frame = state.notification.frame.get();
        for (auto node = args.OriginalSource().try_as<xaml::DependencyObject>(); node && node != frame;
             node = xaml::Media::VisualTreeHelper::GetParent(node)) {
            // Leave the system's volume and quick-settings wheel handling alone.
            if (std::wstring_view(winrt::get_class_name(node)).starts_with(L"SystemTray.OmniButton")) return;
        }
    }
    if (ScrollMotion(group, wheel.MouseWheelDelta(), track ? std::optional(cursor) : std::nullopt)) {
        if (track) TrackIconWheel(group, cursor, now);
        args.Handled(true);
        QueueMotion(now);
    }
} catch (const winrt::hresult_error&) {
    // Explorer can replace the frame during input dispatch.
}

enum class GripEvent { pressed, moved, released, canceled, captureLost };

void HandleGrip(appearance::IconGroup group, GripEvent event, const xaml::Input::PointerRoutedEventArgs& args) noexcept try {
    const auto frame = Frame(group).frame.get();
    const auto pointer = args.Pointer();
    if (!frame || !pointer || pointer.PointerDeviceType() != winrt::Windows::Devices::Input::PointerDeviceType::Mouse)
        return;
    auto& grip = state.grip;
    if (event == GripEvent::pressed) {
        if (state.layerMode != appearance::LayerMode::floating || !state.region.Installed() ||
            !args.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed() || grip.phase != GripPhase::idle)
            return;
        const auto source = args.OriginalSource().try_as<xaml::DependencyObject>();
        for (auto node = source; node && node != frame;
             node = xaml::Media::VisualTreeHelper::GetParent(node)) {
            const auto element = node.try_as<xaml::FrameworkElement>();
            if (!element || !appearance::IsTaskbarGrip(group, element)) continue;
            POINT cursor{};
            if (GetPhysicalCursorPos(&cursor)) {
                grip = {pointer, winrt::make_weak(source), group, GripPhase::pressed, cursor, 0,
                        winrt::make_weak(element)};
            }
            return;
        }
        return;
    }
    if (!grip.pointer || grip.group != group || grip.pointer.PointerId() != pointer.PointerId() ||
        grip.phase == GripPhase::capturing) return;
    if (event == GripEvent::captureLost && grip.phase == GripPhase::dragging && !grip.originalClickMode &&
        args.OriginalSource() != frame)
        return;
    if (event != GripEvent::moved || !args.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed()) {
        const bool dragging = grip.phase == GripPhase::dragging;
        EndGrip();
        if (dragging) {
            args.Handled(true);
            const auto now = GetTickCount64();
            POINT cursor{};
            if (group == appearance::IconGroup::apps && event != GripEvent::canceled &&
                !args.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed() && GetPhysicalCursorPos(&cursor))
                ArmPin(group, appearance::PinInput::Origin::startDrag, cursor, now);
            QueueMotion(now);
        }
        return;
    }
    POINT cursor{};
    if (!GetPhysicalCursorPos(&cursor)) return;
    if (grip.phase == GripPhase::pressed) {
        const auto dx = std::abs(cursor.x - grip.origin.x);
        const auto dy = std::abs(cursor.y - grip.origin.y);
        const bool vertical = dy > dx;
        const int threshold = std::max(3, GetSystemMetricsForDpi(vertical ? SM_CYDRAG : SM_CXDRAG, state.layout.dpi));
        if (std::max(dx, dy) < threshold) return;
        grip.axis = vertical ? MotionAxis::vertical : MotionAxis::horizontal;
        grip.last = vertical ? grip.origin.y : grip.origin.x;
        grip.phase = GripPhase::capturing;
        // Keep Start's native release animation. Other grips transfer capture
        // to their frame to cancel the button's pending click.
        if (group == appearance::IconGroup::apps)
            grip.originalClickMode = appearance::KeepStartRelease(grip.button.get(), pointer);
        bool captured = grip.originalClickMode || frame.CapturePointer(pointer);
        if (!captured) {
            for (auto node = grip.source.get(); node && node != frame;
                 node = xaml::Media::VisualTreeHelper::GetParent(node)) {
                if (const auto element = node.try_as<xaml::UIElement>()) element.ReleasePointerCapture(pointer);
            }
            captured = frame.CapturePointer(pointer);
        }
        if (!captured) {
            grip = {};
            return;
        }
        grip.phase = GripPhase::dragging;
        state.pinInput.Reset();
        EndIconWheel();
        if (CanArmFromStartDrag()) EnsureIconInput();
        HandleLatch(group, appearance::LatchGesture::dragStart, 0, grip.origin, grip.axis);
    }
    const LONG along = grip.axis == MotionAxis::vertical ? cursor.y : cursor.x;
    const LONG delta = along - grip.last;
    if (delta) {
        grip.last = along;
        HandleLatch(group, appearance::LatchGesture::drag, delta, grip.origin, grip.axis);
    }
    args.Handled(true);
} catch (const winrt::hresult_error&) {
    EndGrip();
    QueueMotion(GetTickCount64());
}

void WatchGrip(IconFrame& icons, appearance::IconGroup group, bool active) {
    const auto frame = icons.frame.get();
    if (!frame) {
        icons.gripHandlers = {};
        return;
    }
    const auto installed = [](const auto& handler) { return static_cast<bool>(handler); };
    if (active ? std::all_of(icons.gripHandlers.begin(), icons.gripHandlers.end(), installed)
               : std::none_of(icons.gripHandlers.begin(), icons.gripHandlers.end(), installed)) return;
    const std::array events{xaml::UIElement::PointerPressedEvent(), xaml::UIElement::PointerMovedEvent(),
                            xaml::UIElement::PointerReleasedEvent(), xaml::UIElement::PointerCanceledEvent(),
                            xaml::UIElement::PointerCaptureLostEvent()};
    for (std::size_t i = 0; i < events.size(); ++i) {
        auto& handler = icons.gripHandlers[i];
        if (active && !handler) {
            const auto event = static_cast<GripEvent>(i);
            auto callback = winrt::box_value(xaml::Input::PointerEventHandler(
                [group, event](const auto&, const auto& args) { HandleGrip(group, event, args); }));
            frame.AddHandler(events[i], callback, true);
            handler = std::move(callback);
        } else if (!active && handler) {
            frame.RemoveHandler(events[i], handler);
            handler = nullptr;
        }
    }
}

void WatchLayout(bool attached = true) {
    const auto frame = state.apps.frame.get();
    const bool active = attached && state.enabled && !state.highContrast;
    if (active && frame && !state.layoutChanged) {
        state.layoutChanged = frame.LayoutUpdated([](const auto&, const auto&) { ScheduleLayout(); });
    } else if (!active && state.layoutChanged) {
        if (frame) frame.LayoutUpdated(state.layoutChanged);
        state.layoutChanged = {};
    }
    const auto watchWheel = [active](IconFrame& icons, appearance::IconGroup group) {
        const auto current = icons.frame.get();
        if (active && current && !icons.wheel) {
            icons.wheel = current.PointerWheelChanged([group](const auto&, const auto& args) { ScrollIcons(group, args); });
        } else if (!active && icons.wheel) {
            if (current) current.PointerWheelChanged(icons.wheel);
            icons.wheel = {};
        }
    };
    watchWheel(state.apps, appearance::IconGroup::apps);
    watchWheel(state.notification, appearance::IconGroup::notification);
    WatchGrip(state.apps, appearance::IconGroup::apps, active && state.layerMode == appearance::LayerMode::floating);
    WatchGrip(state.notification, appearance::IconGroup::notification, active && state.layerMode == appearance::LayerMode::floating);
}

void WatchFill(Background& entry, bool watch) {
    const auto shape = entry.element.get();
    if (!shape) return;
    const auto property = xaml::Shapes::Shape::FillProperty();
    if (watch && !entry.watch) {
        const auto handle = entry.handle;
        entry.watch = shape.RegisterPropertyChangedCallback(property,
            [handle](const xaml::DependencyObject&, const xaml::DependencyProperty&) {
                if (!state.enabled || state.highContrast) return;
                const auto existing = std::find_if(state.backgrounds.begin(), state.backgrounds.end(),
                    [handle](const Background& item) { return item.handle == handle; });
                if (existing != state.backgrounds.end()) existing->Apply(true);
            });
    } else if (!watch && entry.watch) {
        shape.UnregisterPropertyChangedCallback(property, entry.watch);
        entry.watch = 0;
    }
}

void PauseLayout() {
    KillTimer(state.control, layoutTimer);
    state.layoutPending = false;
    state.layoutDirty = false;
    ++state.layoutGeneration;
}

void SetAppearance(bool enabled, appearance::Ink ink = appearance::Ink::system,
                   appearance::LayerMode layer = appearance::LayerMode::native,
                   appearance::Ink iconInk = appearance::Ink::system) {
    const bool wasActive = state.enabled && !state.highContrast;
    const auto previousLayer = state.layerMode;
    if (enabled && layer == appearance::LayerMode::suspended && previousLayer != layer && !state.suspendedPins) {
        appearance::IconOffsets pins;
        for (const auto group : iconGroups)
            if (Motion(group).Pinned()) pins.For(group) = state.offsets.For(group);
        if (pins != appearance::IconOffsets{})
            state.suspendedPins = SuspendedPins{pins, state.layout};
    }
    state.enabled = enabled && layer != appearance::LayerMode::suspended;
    state.layerMode = layer;
    if (layer != appearance::LayerMode::floating) StopPinRecoveryTimer();
    HIGHCONTRASTW accessibility{sizeof(HIGHCONTRASTW)};
    state.highContrast = enabled &&
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(accessibility), &accessibility, 0) &&
        (accessibility.dwFlags & HCF_HIGHCONTRASTON);
    const bool active = state.enabled && !state.highContrast;
    const HWND primary = layer == appearance::LayerMode::native ? nullptr : FindWindowW(L"Shell_TrayWnd", nullptr);
    if (enabled && layer == appearance::LayerMode::suspended && state.suspendedPins && !state.highContrast) {
        if (previousLayer == layer) return;
        PauseLayout();
        if (previousLayer == appearance::LayerMode::covered) state.layer.HoldPresentation(true);
        if (state.peeling || state.grip.phase != GripPhase::idle || NeedsMotionFrame() ||
            !PinsInPlace(primary, *state.suspendedPins)) {
            InvalidatePinnedLayout();
        } else {
            // The secure desktop already covers the ordinary desktop. Keep the
            // committed pose and transparency in place until it comes back.
            PauseMotion();
        }
        state.layer.Update(primary, layer, state.control);
        try { WatchLayout(false); } catch (const winrt::hresult_error&) { state.layoutChanged = {}; }
        return;
    }

    state.ink = ink;
    const bool tintIcons = active && layer == appearance::LayerMode::floating;
    state.iconInk.SetInk(tintIcons ? iconInk : appearance::Ink::system,
                         tintIcons ? ink : appearance::Ink::system);
    const bool resuming = active && layer == appearance::LayerMode::floating && state.suspendedPins &&
                          previousLayer != layer;
    const bool pinsReady = resuming && PinsInPlace(primary, *state.suspendedPins);
    if (resuming && !pinsReady) InvalidatePinnedLayout();
    const HWND bar = state.enabled ? primary : nullptr;
    if (!active || layer == appearance::LayerMode::native) RestoreIcons();
    if (layer == appearance::LayerMode::covered && previousLayer != layer) {
        PauseMotion();
        for (const auto group : iconGroups)
            if (Motion(group).Dragging()) Motion(group).EndDrag();
        UpdateLatches();
    }
    state.layer.Update(bar, layer, state.control);
    SetPropW(state.control, L"FullWinTB.AppliedInk",
             reinterpret_cast<HANDLE>(1 + static_cast<ULONG_PTR>(active ? ink : appearance::Ink::system)));
    for (auto& root : state.themeRoots) {
        try {
            root.Apply(active ? ink : appearance::Ink::system);
        } catch (const winrt::hresult_error&) {
            // Explorer may have just replaced this theme root.
        }
    }
    for (auto& background : state.backgrounds) {
        try {
            WatchFill(background, active);
            background.Apply(active);
        } catch (const winrt::hresult_error&) {
            // Explorer may have just discarded the XAML island.
        }
    }
    try {
        WatchLayout();
    } catch (const winrt::hresult_error&) {
        state.layoutChanged = {};
    }
    if (pinsReady || !enabled || state.highContrast || layer == appearance::LayerMode::native) {
        FinishPinRecovery();
    } else if (resuming) {
        if (SetTimer(state.control, pinRecoveryTimer, pinRecoveryTimeout, nullptr))
            state.pinRecoveryAt = GetTickCount64() + pinRecoveryTimeout;
        else FinishPinRecovery();
    }
    if (active && layer == appearance::LayerMode::floating && previousLayer != layer) {
        if (CanvasLost() || (state.layer.CanvasOpen() && !state.region.OwnsCurrent())) RestoreIcons();
        FollowPopups();
        UpdateLatches();
        QueueMotion(GetTickCount64());
    }
    if (active) {
        if (!wasActive || previousLayer != layer) ScheduleLayout();
    } else {
        PauseLayout();
        state.layout = {};
        state.region.Reset();
        state.appsLatch.Destroy();
        state.notificationLatch.Destroy();
    }
}

void StopWatchingHost() {
    if (state.wait) {
        SetThreadpoolWait(state.wait, nullptr, nullptr);
        WaitForThreadpoolWaitCallbacks(state.wait, TRUE);
        CloseThreadpoolWait(state.wait);
        state.wait = nullptr;
    }
    if (state.hostHandle) {
        CloseHandle(state.hostHandle);
        state.hostHandle = nullptr;
    }
    state.host = nullptr;
    state.hostProcess = 0;
}

void CALLBACK OnHostExit(PTP_CALLBACK_INSTANCE, void* control, PTP_WAIT, TP_WAIT_RESULT) {
    PostMessageW(static_cast<HWND>(control), hostExited, 0, 0);
}

bool WatchHost(HWND host) {
    wchar_t className[64]{};
    if (!GetClassNameW(host, className, ARRAYSIZE(className)) ||
        wcscmp(className, appearance::hostClass)) return false;
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(host, &pid)) return false;
    if (state.host == host && state.hostProcess == pid) return true;
    StopWatchingHost();
    state.host = host;
    state.hostProcess = pid;
    state.hostHandle = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (state.hostHandle) {
        state.wait = CreateThreadpoolWait(OnHostExit, state.control, nullptr);
        if (state.wait) SetThreadpoolWait(state.wait, state.hostHandle, nullptr);
    }
    return true;
}

void ReadWheelAxis() {
    const auto wanted = reinterpret_cast<UINT_PTR>(GetPropW(state.host, appearance::requestedMotionAxis)) == 2
        ? MotionAxis::vertical : MotionAxis::horizontal;
    if (wanted != state.wheelAxis) {
        state.wheelAxis = wanted;
        EndIconWheel();
    }
}

void ReadMotionSettings() {
    const auto packet = appearance::ReadMotionPacket(state.host);
    const auto style = reinterpret_cast<UINT_PTR>(GetPropW(state.host, appearance::requestedReturnStyle)) == 2
        ? appearance::ReturnStyle::simple : appearance::ReturnStyle::elastic;
    if (packet && packet != state.motionPacket) {
        state.motionPacket = packet;
        state.motionProfile = appearance::MotionProfile(appearance::UnpackMotionSettings(packet), style);
    } else {
        if (style == state.motionProfile.returnStyle) return;
        state.motionProfile.returnStyle = style;
    }
    QueueMotion(GetTickCount64());
}

DWORD WINAPI ConnectToXaml(void* context) {
    const HWND control = static_cast<HWND>(context);
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(state.module, path, ARRAYSIZE(path));
    const HMODULE xamlModule = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto initialize = xamlModule ? reinterpret_cast<decltype(&InitializeXamlDiagnosticsEx)>(
        GetProcAddress(xamlModule, "InitializeXamlDiagnosticsEx")) : nullptr;
    HRESULT result = E_FAIL;
    if (initialize && length && length < ARRAYSIZE(path)) {
        struct Connection {
            decltype(&InitializeXamlDiagnosticsEx) initialize;
            const wchar_t* path;
            unsigned slot;
            HRESULT status = E_FAIL;
        };
        for (unsigned slot = 1; slot <= 32; ++slot) {
            Connection connection{initialize, path, slot};
            // XAML Diagnostics binds the connection to its calling thread.
            const HANDLE attempt = CreateThread(nullptr, 0, [](void* data) -> DWORD {
                auto& request = *static_cast<Connection*>(data);
                wchar_t endpoint[32]{};
                swprintf_s(endpoint, L"VisualDiagConnection%u", request.slot);
                request.status = request.initialize(endpoint, GetCurrentProcessId(),
                                                    nullptr, request.path, siteId, nullptr);
                return 0;
            }, &connection, 0, nullptr);
            if (!attempt) {
                result = HRESULT_FROM_WIN32(GetLastError());
                break;
            }
            WaitForSingleObject(attempt, INFINITE);
            CloseHandle(attempt);
            result = connection.status;
            if (SUCCEEDED(result)) break;
            if (slot == 1)
                SetPropW(control, L"FullWinTB.XamlFirstAttempt",
                    reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(static_cast<DWORD>(result))));
        }
    }
    if (xamlModule) FreeLibrary(xamlModule);
    if (FAILED(result)) PostMessageW(control, setupFinished, static_cast<DWORD>(result), 0);
    return 0;
}

void StartConnection() {
    // Explorer can deliver usable visual callbacks even if a diagnostics
    // endpoint reports an error during setup. Use the live subscription.
    if (state.connecting || state.connected || !state.themeRoots.empty() || !state.backgrounds.empty()) return;
    state.connecting = true;
    if (const HANDLE thread = CreateThread(nullptr, 0, ConnectToXaml, state.control, 0, nullptr))
        CloseHandle(thread);
    else
        state.connecting = false;
}

LRESULT CALLBACK AppearanceProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    static const UINT update = RegisterWindowMessageW(appearance::updateMessage);
    static const UINT layout = RegisterWindowMessageW(appearance::layoutMessage);
    static const UINT resetIcons = RegisterWindowMessageW(appearance::resetIconsMessage);
    static const UINT motionAxis = RegisterWindowMessageW(appearance::motionAxisMessage);
    static const UINT motionSettings = RegisterWindowMessageW(appearance::motionSettingsMessage);
    if (message == update) {
        const auto host = reinterpret_cast<HWND>(wp);
        // A posted color change can lag behind a synchronous disable.
        if (reinterpret_cast<LPARAM>(GetPropW(host, appearance::requestedMode)) != lp + 1) return 0;
        const LPARAM ink = lp & appearance::inkMask;
        const auto layer = static_cast<appearance::LayerMode>((lp >> appearance::layerShift) & appearance::inkMask);
        const auto iconInk = static_cast<appearance::Ink>((lp >> appearance::iconInkShift) & appearance::inkMask);
        if (ink >= 1 && ink <= 3 && layer <= appearance::LayerMode::suspended &&
            iconInk <= appearance::Ink::black && WatchHost(host)) {
            ReadWheelAxis();
            ReadMotionSettings();
            SetAppearance(true, static_cast<appearance::Ink>(ink - 1), layer, iconInk);
            if (state.enabled) StartConnection();
        } else if (!lp && host == state.host) {
            SetAppearance(false);
            StopWatchingHost();
        }
        return 0;
    }
    if (message == layout) {
        if (reinterpret_cast<HWND>(wp) == state.host) ScheduleLayout();
        return 0;
    }
    if (message == resetIcons) {
        if (reinterpret_cast<HWND>(wp) == state.host) {
            RestoreIcons();
            FinishPinRecovery();
        }
        return 0;
    }
    if (message == motionAxis) {
        if (reinterpret_cast<HWND>(wp) == state.host) ReadWheelAxis();
        return 0;
    }
    if (message == motionSettings) {
        if (reinterpret_cast<HWND>(wp) == state.host) ReadMotionSettings();
        return 0;
    }
    if (message == appearance::latchMessage) {
        if (LOWORD(wp) <= static_cast<UINT>(appearance::IconGroup::notification) &&
            HIWORD(wp) <= static_cast<UINT>(appearance::LatchGesture::peelRemove))
            HandleLatch(static_cast<appearance::IconGroup>(LOWORD(wp)),
                        static_cast<appearance::LatchGesture>(HIWORD(wp)), static_cast<LONG>(lp));
        return 0;
    }
    if (message == appearance::taskbarOrderChanged) {
        const bool fromBar = wp && reinterpret_cast<HWND>(wp) == state.layout.bar;
        const bool fromHost = lp && reinterpret_cast<HWND>(lp) == state.host;
        if (state.layerMode == appearance::LayerMode::floating && (fromBar || fromHost)) {
            UpdateLatches();
            state.appsLatch.KeepAbove(state.layout.bar);
            state.notificationLatch.KeepAbove(state.layout.bar);
        }
        return 0;
    }
    if (message == iconWheel) {
        if (wp == state.wheelSequence && state.inputHook) {
            const auto now = GetTickCount64();
            const auto anchor = state.wheelGesture.Anchor();
            if (ScrollMotion(state.wheelGroup, static_cast<int>(lp), anchor)) {
                POINT cursor = anchor;
                GetPhysicalCursorPos(&cursor);
                ArmPin(state.wheelGroup, appearance::PinInput::Origin::wheel, cursor, now);
            }
            QueueMotion(now);
        }
        return 0;
    }
    if (message == motionInputChanged) {
        QueueMotion(GetTickCount64());
        return 0;
    }
    if (message == motionFrame) {
        state.motionFrameQueued = false;
        if (state.rendering) {
            SetMotionInterval(renderingWatchdog);
            StepMotion();
        }
        return 0;
    }
    if (message == WM_TIMER && wp == motionTimer) {
        if (state.motionInterval == renderingWatchdog && state.rendering) {
            if (MotionClock::now() - state.lastMotionFrame < std::chrono::milliseconds(48)) return 0;
            SetMotionInterval(USER_TIMER_MINIMUM);
        }
        if (state.motionInterval) StepMotion();
        return 0;
    }
    if (message == setupFinished) {
        state.connecting = false;
        state.connected = state.connected || SUCCEEDED(static_cast<HRESULT>(wp)) ||
            !state.themeRoots.empty() || !state.backgrounds.empty();
        SetPropW(window, L"FullWinTB.XamlStatus", reinterpret_cast<HANDLE>(state.connected ? 1 : wp));
        return 0;
    }
    if (message == hostExited) {
        if (state.hostHandle && WaitForSingleObject(state.hostHandle, 0) == WAIT_OBJECT_0) {
            SetAppearance(false);
            StopWatchingHost();
        }
        return 0;
    }
    if (message == WM_TIMER && wp == layoutTimer) {
        KillTimer(window, layoutTimer);
        state.layoutPending = false;
        MeasureLayout();
        return 0;
    }
    if (message == WM_TIMER && wp == pinRecoveryTimer) {
        if (state.pinRecoveryAt && GetTickCount64() >= state.pinRecoveryAt) {
            InvalidatePinnedLayout();
            FinishPinRecovery();
            ScheduleLayout();
        }
        return 0;
    }
    if (message == layoutReady) {
        state.layoutMeasuring = false;
        AcquireSRWLockExclusive(&layoutLock);
        auto result = std::move(nextLayout);
        ReleaseSRWLockExclusive(&layoutLock);
        const bool dirty = std::exchange(state.layoutDirty, false);
        bool settling = false;
        if (state.enabled && state.layerMode == appearance::LayerMode::floating && !state.highContrast &&
            !state.peeling && !NeedsMotionFrame() && result.generation == state.layoutGeneration && result.offsets == state.offsets) {
            if (CanvasLost() || (state.layer.CanvasOpen() && !state.region.OwnsCurrent())) {
                RestoreIcons();
                return 0;
            }
            if (!result.layout.bar) return 0;
            state.apps.wave.Clear();
            state.notification.wave.Clear();
            if (!appearance::TaskbarRegion::Fits(result.layout, state.offsets)) {
                RestoreIcons();
                if (fullwintb::SameRect(result.layout.bounds, result.layout.dock) && result.layout.bar)
                    state.layout = std::move(result.layout);
            } else {
                state.layout = std::move(result.layout);
                UpdateLatches();
                if (state.offsets.notification) FollowPopups();
            }
            settling = state.region.Update(state.layout, state.offsets);
            if (AwaitingReturn()) QueueMotion(GetTickCount64());
        }
        if (settling || dirty)
            ScheduleLayout();
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

xaml::FrameworkElement TaskbarFrame(const xaml::UIElement& window) {
    auto parent = xaml::Media::VisualTreeHelper::GetParent(window);
    while (parent) {
        const auto type = winrt::get_class_name(parent);
        if (type == L"Taskbar.TaskbarFrame" || type == L"SystemTray.SystemTrayFrame")
            return parent.try_as<xaml::FrameworkElement>();
        parent = xaml::Media::VisualTreeHelper::GetParent(parent);
    }
    return nullptr;
}

bool IsTaskbarAppIcon(const xaml::Controls::Image& image) {
    const auto panel = xaml::Media::VisualTreeHelper::GetParent(image).try_as<xaml::FrameworkElement>();
    if (!panel || panel.Name() != L"IconPanel") return false;
    const auto button = xaml::Media::VisualTreeHelper::GetParent(panel);
    if (!button || winrt::get_class_name(button) != L"Taskbar.TaskListButton") return false;
    const auto frame = TaskbarFrame(image);
    return frame && winrt::get_class_name(frame) == L"Taskbar.TaskbarFrame";
}

bool IsNotificationAreaIcon(const xaml::Controls::Image& image) {
    const auto grid = xaml::Media::VisualTreeHelper::GetParent(image);
    if (!grid) return false;
    auto ancestor = xaml::Media::VisualTreeHelper::GetParent(grid);
    if (!ancestor || winrt::get_class_name(ancestor) != L"SystemTray.ImageIconContent") return false;
    // The overflow flyout also contains tray icons, but has its own background.
    while (ancestor) {
        if (winrt::get_class_name(ancestor) == L"SystemTray.NotificationAreaIcons") return true;
        ancestor = xaml::Media::VisualTreeHelper::GetParent(ancestor);
    }
    return false;
}

struct VisualObserver : winrt::implements<VisualObserver, IVisualTreeServiceCallback2, winrt::non_agile> {
    explicit VisualObserver(winrt::com_ptr<IXamlDiagnostics> diagnostics)
        : diagnostics_(std::move(diagnostics)) {}

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation, VisualElement element,
                                                 VisualMutationType mutation) noexcept final try {
        const std::wstring_view type = element.Type ? element.Type : L"";
        if (mutation == Add && type == L"Taskbar.FlyoutFrame") {
            winrt::com_ptr<::IInspectable> inspectable;
            if (SUCCEEDED(diagnostics_->GetIInspectableFromHandle(element.Handle, inspectable.put()))) {
                if (const auto frame = inspectable.try_as<xaml::FrameworkElement>())
                    appearance::SetThumbnailFlyout(element.Handle, frame);
            }
        } else if (mutation == Remove) {
            appearance::RemoveThumbnailFlyout(element.Handle);
        }
        if (GetCurrentThreadId() != GetWindowThreadProcessId(state.control, nullptr)) return S_OK;
        if (mutation == Add && type == L"Windows.UI.Xaml.Controls.Image") {
            winrt::com_ptr<::IInspectable> inspectable;
            if (SUCCEEDED(diagnostics_->GetIInspectableFromHandle(element.Handle, inspectable.put()))) {
                if (const auto image = inspectable.try_as<xaml::Controls::Image>()) {
                    if (element.Name && std::wstring_view(element.Name) == L"Icon" && IsTaskbarAppIcon(image))
                        state.iconInk.Track(element.Handle, appearance::IconGroup::apps, image);
                    else if (IsNotificationAreaIcon(image))
                        state.iconInk.Track(element.Handle, appearance::IconGroup::notification, image);
                }
            }
        } else if (mutation == Remove) {
            state.iconInk.Forget(element.Handle);
        }
        if (mutation == Add && element.Name && std::wstring_view(element.Name) == L"OverflowRootGrid") {
            winrt::com_ptr<::IInspectable> inspectable;
            if (SUCCEEDED(diagnostics_->GetIInspectableFromHandle(element.Handle, inspectable.put()))) {
                if (const auto grid = inspectable.try_as<xaml::FrameworkElement>())
                    appearance::SetTrayOverflowGrid(element.Handle, grid);
            }
        } else if (mutation == Remove) {
            appearance::RemoveTrayOverflowGrid(element.Handle);
        }
        if (!state.apps.frame.get() || type.starts_with(L"Taskbar.") ||
            type.starts_with(L"SystemTray.")) ScheduleLayout();
        if (mutation == Add && (type == L"Taskbar.TaskbarFrame" || type == L"SystemTray.SystemTrayFrame")) {
            winrt::com_ptr<::IInspectable> inspectable;
            if (SUCCEEDED(diagnostics_->GetIInspectableFromHandle(element.Handle, inspectable.put()))) {
                if (const auto frame = inspectable.try_as<xaml::FrameworkElement>()) {
                    const auto existing = std::find_if(state.themeRoots.begin(), state.themeRoots.end(),
                        [&](const ThemeRoot& root) { return root.handle == element.Handle; });
                    if (existing == state.themeRoots.end()) {
                        state.themeRoots.push_back(ThemeRoot{element.Handle, winrt::make_weak(frame)});
                        if (state.enabled && !state.highContrast) state.themeRoots.back().Apply(state.ink);
                    }
                    const bool apps = type == L"Taskbar.TaskbarFrame";
                    auto& icons = apps ? state.apps : state.notification;
                    if (!icons.frame.get()) {
                        icons.frame = winrt::make_weak(frame);
                        if (apps) state.layoutChanged = {};
                        icons.wheel = {};
                        WatchLayout();
                    }
                }
            }
        }
        auto& entries = state.backgrounds;
        if (mutation == Remove) {
            const auto existing = std::find_if(entries.begin(), entries.end(),
                [&](const Background& item) { return item.handle == element.Handle; });
            if (existing != entries.end()) {
                Background removed = std::move(*existing);
                entries.erase(existing);
                WatchFill(removed, false);
                removed.Apply(false);
            }
            auto& roots = state.themeRoots;
            const auto root = std::find_if(roots.begin(), roots.end(),
                [&](const ThemeRoot& item) { return item.handle == element.Handle; });
            if (root != roots.end()) {
                ThemeRoot removed = std::move(*root);
                roots.erase(root);
                const auto frame = removed.element.get();
                if (frame && (frame == state.apps.frame.get() || frame == state.notification.frame.get())) {
                    RestoreIcons();
                    WatchLayout(false);
                    if (frame == state.apps.frame.get()) state.apps.frame = {};
                    if (frame == state.notification.frame.get()) state.notification.frame = {};
                    state.region.Reset();
                    ++state.layoutGeneration;
                    WatchLayout();
                }
                removed.Apply(appearance::Ink::system);
            }
        } else if (mutation == Add && element.Name && element.Type &&
                   (std::wstring_view(element.Name) == L"BackgroundFill" ||
                    std::wstring_view(element.Name) == L"BackgroundStroke") &&
                   std::wstring_view(element.Type) == L"Windows.UI.Xaml.Shapes.Rectangle") {
            winrt::com_ptr<::IInspectable> inspectable;
            if (SUCCEEDED(diagnostics_->GetIInspectableFromHandle(element.Handle, inspectable.put()))) {
                const auto shape = inspectable.try_as<xaml::Shapes::Shape>();
                const auto frame = shape ? TaskbarFrame(shape) : nullptr;
                if (frame &&
                    std::none_of(entries.begin(), entries.end(), [&](const Background& item) {
                        return item.handle == element.Handle;
                    })) {
                    entries.push_back(Background{element.Handle, winrt::make_weak(shape)});
                    WatchFill(entries.back(), state.enabled && !state.highContrast);
                    entries.back().Apply(state.enabled && !state.highContrast);
                    SetPropW(state.control, L"FullWinTB.XamlBackgrounds",
                             reinterpret_cast<HANDLE>(entries.size()));
                    if (!state.apps.frame.get() && winrt::get_class_name(frame) == L"Taskbar.TaskbarFrame") {
                        state.apps.frame = winrt::make_weak(frame);
                        state.layoutChanged = {};
                        state.apps.wheel = {};
                        WatchLayout();
                    }
                }
            }
        }
        return S_OK;
    } catch (...) {
        // A departing/rebuilt XAML subtree must not interrupt the observer.
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState,
                                                    LPCWSTR) noexcept final { return S_OK; }

private:
    winrt::com_ptr<IXamlDiagnostics> diagnostics_;
};

struct Site : winrt::implements<Site, IObjectWithSite> {
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept final try {
        diagnostics_.copy_from(site);
        if (!site) return S_OK;
        const auto diagnostics = diagnostics_.as<IXamlDiagnostics>();
        const auto service = diagnostics.as<IVisualTreeService3>();
        observer_ = winrt::make_self<VisualObserver>(diagnostics);
        // Observing the existing tree dispatches callbacks onto the UI thread.
        // Start the subscription off that thread so it can service them.
        struct Subscription {
            winrt::com_ptr<IVisualTreeService3> service;
            winrt::com_ptr<VisualObserver> observer;
            HWND control;
        };
        auto* request = new Subscription{service, observer_, state.control};
        if (!TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE, void* context) noexcept {
            auto* subscription = static_cast<Subscription*>(context);
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const HRESULT status = subscription->service->AdviseVisualTreeChange(subscription->observer.get());
            PostMessageW(subscription->control, setupFinished, static_cast<DWORD>(status), 0);
            delete subscription;
            if (SUCCEEDED(apartment)) CoUninitialize();
        }, request, nullptr)) {
            delete request;
            return HRESULT_FROM_WIN32(GetLastError());
        }
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID id, void** object) noexcept final {
        if (!diagnostics_) return E_FAIL;
        return diagnostics_->QueryInterface(id, object);
    }

private:
    winrt::com_ptr<IUnknown> diagnostics_;
    winrt::com_ptr<VisualObserver> observer_;
};

struct SiteFactory : winrt::implements<SiteFactory, IClassFactory> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID id, void** object) noexcept final try {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        return winrt::make_self<Site>()->QueryInterface(id, object);
    } catch (...) {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept final { return S_OK; }
};

void OpenControl() {
    if (state.control) return;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&OpenControl), &module)) return;
    state.module = module;
    WNDCLASSW spec{};
    spec.hInstance = module;
    spec.lpszClassName = appearance::controlClass;
    spec.lpfnWndProc = AppearanceProc;
    if (!RegisterClassW(&spec)) return;
    state.control = CreateWindowExW(0, appearance::controlClass, nullptr, 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, module, nullptr);
}

} // namespace

extern "C" __declspec(dllexport) LRESULT CALLBACK OpenAppearance(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto& request = *reinterpret_cast<const CWPSTRUCT*>(lp);
        static const UINT update = RegisterWindowMessageW(appearance::updateMessage);
        const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        DWORD explorer = 0;
        GetWindowThreadProcessId(GetShellWindow(), &explorer);
        if (request.message == update && request.hwnd == tray && explorer == GetCurrentProcessId()) {
            OpenControl();
            if (state.control)
                PostMessageW(state.control, update, request.wParam, request.lParam);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
STDAPI DllGetClassObject(REFCLSID clsid, REFIID id, void** object) try {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (clsid != siteId || !state.control) return CLASS_E_CLASSNOTAVAILABLE;
    return winrt::make_self<SiteFactory>()->QueryInterface(id, object);
} catch (...) {
    return winrt::to_hresult();
}

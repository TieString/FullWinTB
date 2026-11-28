#pragma once

#include "motion_settings.h"
#include "version.h"

#include <windows.h>

namespace fullwintb::appearance {

inline constexpr wchar_t controlClass[] = L"FullWinTB.TaskbarAppearance.v" FULLWINTB_APPEARANCE_VERSION_WIDE;
inline constexpr wchar_t updateMessage[] = L"FullWinTB.TaskbarAppearance.Update.v" FULLWINTB_APPEARANCE_VERSION_WIDE;
inline constexpr wchar_t layoutMessage[] = L"FullWinTB.TaskbarAppearance.Layout.v" FULLWINTB_APPEARANCE_VERSION_WIDE;
inline constexpr wchar_t resetIconsMessage[] = L"FullWinTB.TaskbarAppearance.ResetIcons.v" FULLWINTB_APPEARANCE_VERSION_WIDE;
inline constexpr wchar_t motionAxisMessage[] = L"FullWinTB.TaskbarAppearance.MotionAxis.v" FULLWINTB_APPEARANCE_VERSION_WIDE;
inline constexpr wchar_t motionSettingsMessage[] = L"FullWinTB.TaskbarAppearance.MotionSettings.v" FULLWINTB_APPEARANCE_VERSION_WIDE;
inline constexpr wchar_t hostClass[] = L"FullWinTB.Window";
inline constexpr wchar_t requestedMode[] = L"FullWinTB.RequestedAppearance";
inline constexpr wchar_t requestedMotionAxis[] = L"FullWinTB.RequestedMotionAxis";
inline constexpr wchar_t requestedReturnStyle[] = L"FullWinTB.RequestedReturnStyle";
inline constexpr wchar_t motionPacketLow[] = L"FullWinTB.MagnetSettings.Low";
inline constexpr wchar_t motionPacketHigh[] = L"FullWinTB.MagnetSettings.High";
inline constexpr wchar_t motionPacketSequence[] = L"FullWinTB.MagnetSettings.Sequence";
inline constexpr wchar_t appsOffsetProperty[] = L"FullWinTB.AppsOffset";
inline constexpr wchar_t notificationOffsetProperty[] = L"FullWinTB.NotificationOffset";
inline constexpr wchar_t appsOffsetX[] = L"FullWinTB.AppsOffsetX";
inline constexpr wchar_t appsOffsetY[] = L"FullWinTB.AppsOffsetY";
inline constexpr wchar_t notificationOffsetX[] = L"FullWinTB.NotificationOffsetX";
inline constexpr wchar_t notificationOffsetY[] = L"FullWinTB.NotificationOffsetY";
inline constexpr wchar_t appsInkSpanProperty[] = L"FullWinTB.AppsInkSpan";
inline constexpr wchar_t dockWidthProperty[] = L"FullWinTB.DockWidth";
inline constexpr wchar_t dockHeightProperty[] = L"FullWinTB.DockHeight";
inline constexpr LPARAM inkMask = 0x0f;
inline constexpr UINT layerShift = 4;
inline constexpr UINT iconInkShift = 8;

enum class Ink : UINT { system, white, black, automatic };
enum class LayerMode : UINT { native, floating, covered, suspended };
enum class MotionAxis : UINT { horizontal, vertical };

inline constexpr MotionAxis OtherAxis(MotionAxis axis) {
    return axis == MotionAxis::horizontal ? MotionAxis::vertical : MotionAxis::horizontal;
}

inline constexpr LONG AxisStart(const RECT& rect, MotionAxis axis) {
    return axis == MotionAxis::horizontal ? rect.left : rect.top;
}

inline constexpr LONG& AxisStart(RECT& rect, MotionAxis axis) {
    return axis == MotionAxis::horizontal ? rect.left : rect.top;
}

inline constexpr LONG AxisEnd(const RECT& rect, MotionAxis axis) {
    return axis == MotionAxis::horizontal ? rect.right : rect.bottom;
}

inline constexpr LONG& AxisEnd(RECT& rect, MotionAxis axis) {
    return axis == MotionAxis::horizontal ? rect.right : rect.bottom;
}

inline constexpr LONG AxisSize(const RECT& rect, MotionAxis axis) {
    return AxisEnd(rect, axis) - AxisStart(rect, axis);
}

inline constexpr LONG AxisPoint(POINT point, MotionAxis axis) {
    return axis == MotionAxis::horizontal ? point.x : point.y;
}

inline constexpr MotionAxis TaskbarAxis(const RECT& dock) {
    return dock.right - dock.left >= dock.bottom - dock.top ? MotionAxis::horizontal : MotionAxis::vertical;
}

inline constexpr const wchar_t* DockThicknessProperty(MotionAxis axis) {
    return axis == MotionAxis::horizontal ? dockWidthProperty : dockHeightProperty;
}

inline bool ReadDockBounds(HWND bar, RECT& bounds) {
    if (!GetWindowRect(bar, &bounds)) return false;
    for (const auto axis : {MotionAxis::horizontal, MotionAxis::vertical}) {
        const auto thickness = static_cast<LONG>(reinterpret_cast<LONG_PTR>(GetPropW(bar, DockThicknessProperty(axis))));
        const LONG size = AxisSize(bounds, axis);
        if (thickness > 0 && thickness < size) AxisStart(bounds, axis) = AxisEnd(bounds, axis) - thickness;
        if (thickness < 0 && thickness > -size) AxisEnd(bounds, axis) = AxisStart(bounds, axis) - thickness;
    }
    return true;
}

inline std::uint64_t ReadMotionPacket(HWND host) {
    const auto sequence = reinterpret_cast<UINT_PTR>(GetPropW(host, motionPacketSequence));
    if (!sequence || (sequence & 1)) return 0;
    const auto low = static_cast<std::uint32_t>(reinterpret_cast<UINT_PTR>(GetPropW(host, motionPacketLow)));
    const auto high = static_cast<std::uint32_t>(reinterpret_cast<UINT_PTR>(GetPropW(host, motionPacketHigh)));
    if (sequence != reinterpret_cast<UINT_PTR>(GetPropW(host, motionPacketSequence))) return 0;
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

void Update(HWND host, bool enabled, Ink ink, LayerMode layer = LayerMode::native,
            Ink iconInk = Ink::system);
void RefreshLayout(HWND host);
void RefreshLatchOrder(HWND host);
void ResetIcons(HWND host);
void SetWheelAxis(HWND host, MotionAxis axis);
void SetReturnStyle(HWND host, ReturnStyle style);
void SetMotionSettings(HWND host, const MotionSettings& settings);

} // namespace fullwintb::appearance

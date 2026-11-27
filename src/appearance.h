#pragma once

#include "motion_settings.h"
#include "version.h"

#include <windows.h>

namespace fullwintb::appearance {

inline constexpr wchar_t controlClass[] = L"FullWinTB.TaskbarAppearance.v" FULLWINTB_VERSION_WIDE;
inline constexpr wchar_t updateMessage[] = L"FullWinTB.TaskbarAppearance.Update.v" FULLWINTB_VERSION_WIDE;
inline constexpr wchar_t layoutMessage[] = L"FullWinTB.TaskbarAppearance.Layout.v" FULLWINTB_VERSION_WIDE;
inline constexpr wchar_t resetIconsMessage[] = L"FullWinTB.TaskbarAppearance.ResetIcons.v" FULLWINTB_VERSION_WIDE;
inline constexpr wchar_t motionAxisMessage[] = L"FullWinTB.TaskbarAppearance.MotionAxis.v" FULLWINTB_VERSION_WIDE;
inline constexpr wchar_t motionSettingsMessage[] = L"FullWinTB.TaskbarAppearance.MotionSettings.v" FULLWINTB_VERSION_WIDE;
inline constexpr wchar_t hostClass[] = L"FullWinTB.Window";
inline constexpr wchar_t requestedMode[] = L"FullWinTB.RequestedAppearance";
inline constexpr wchar_t requestedMotionAxis[] = L"FullWinTB.RequestedMotionAxis";
inline constexpr wchar_t requestedReturnStyle[] = L"FullWinTB.RequestedReturnStyle";
inline constexpr wchar_t motionPacketLow[] = L"FullWinTB.MagnetSettings.Low";
inline constexpr wchar_t motionPacketHigh[] = L"FullWinTB.MagnetSettings.High";
inline constexpr wchar_t motionPacketSequence[] = L"FullWinTB.MagnetSettings.Sequence";
inline constexpr wchar_t appsOffsetProperty[] = L"FullWinTB.AppsOffset";
inline constexpr wchar_t notificationOffsetProperty[] = L"FullWinTB.NotificationOffset";
inline constexpr wchar_t dockHeightProperty[] = L"FullWinTB.DockHeight";
inline constexpr LPARAM inkMask = 0x0f;
inline constexpr UINT layerShift = 4;

enum class Ink : UINT { system, white, black, automatic };
enum class LayerMode : UINT { native, floating, covered, suspended };
enum class MotionAxis : UINT { horizontal, vertical };

inline bool ReadDockBounds(HWND bar, RECT& bounds) {
    if (!GetWindowRect(bar, &bounds)) return false;
    const auto height = static_cast<LONG>(reinterpret_cast<LONG_PTR>(GetPropW(bar, dockHeightProperty)));
    if (height > 0 && height < bounds.bottom - bounds.top) bounds.top = bounds.bottom - height;
    if (height < 0 && -height < bounds.bottom - bounds.top) bounds.bottom = bounds.top - height;
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

void Update(HWND host, bool enabled, Ink ink, LayerMode layer = LayerMode::native);
void RefreshLayout(HWND host);
void RefreshLatchOrder(HWND host);
void ResetIcons(HWND host);
void SetWheelAxis(HWND host, MotionAxis axis);
void SetReturnStyle(HWND host, ReturnStyle style);
void SetMotionSettings(HWND host, const MotionSettings& settings);

} // namespace fullwintb::appearance

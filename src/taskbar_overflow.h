#pragma once

#include <windows.h>
#include <optional>
#include <xamlOM.h>
#include <winrt/Windows.UI.Xaml.h>

namespace fullwintb::appearance {

std::optional<RECT> TrayChevronBounds(HWND bar, UINT dpi, const winrt::Windows::UI::Xaml::FrameworkElement& tray);
void FollowTrayOverflow(HWND bar, const RECT& dock, LONG x, LONG y,
                        const winrt::Windows::UI::Xaml::FrameworkElement& tray = nullptr);
void SetTrayOverflowGrid(InstanceHandle handle, const winrt::Windows::UI::Xaml::FrameworkElement& grid);
void RemoveTrayOverflowGrid(InstanceHandle handle);

} // namespace fullwintb::appearance

#pragma once

#include <xamlOM.h>
#include <winrt/Windows.UI.Xaml.h>

namespace fullwintb::appearance {

void SetThumbnailFlyout(InstanceHandle handle, const winrt::Windows::UI::Xaml::FrameworkElement& frame);
void RemoveThumbnailFlyout(InstanceHandle handle);
void FollowTaskbarThumbnails(HWND taskbar, LONG offset);

} // namespace fullwintb::appearance

#pragma once

#include <winrt/Windows.UI.Xaml.h>

namespace fullwintb::appearance {

winrt::Windows::Foundation::IInspectable KeepStartRelease(
    const winrt::Windows::UI::Xaml::FrameworkElement& button,
    const winrt::Windows::UI::Xaml::Input::Pointer& pointer) noexcept;
void RestoreStartClick(const winrt::Windows::UI::Xaml::FrameworkElement& button,
                       const winrt::Windows::Foundation::IInspectable& original) noexcept;

} // namespace fullwintb::appearance

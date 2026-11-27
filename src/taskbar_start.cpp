#include <windows.h>
#include <unknwn.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>

#include "taskbar_start.h"

namespace fullwintb::appearance {
namespace {

namespace xaml = winrt::Windows::UI::Xaml;
using winrt::Windows::Foundation::IInspectable;
using xaml::Controls::Primitives::ButtonBase;
using xaml::Controls::ClickMode;

void Restore(const ButtonBase& button, const IInspectable& original) {
    if (button.ClickMode() != ClickMode::Press) return;
    const auto property = ButtonBase::ClickModeProperty();
    try {
        if (original == xaml::DependencyProperty::UnsetValue()) button.ClearValue(property);
        else button.SetValue(property, original);
    } catch (const winrt::hresult_error&) {
        button.ClickMode(ClickMode::Release);
    }
}

} // namespace

IInspectable KeepStartRelease(const xaml::FrameworkElement& element, const xaml::Input::Pointer& pointer) noexcept try {
    const auto button = element.try_as<ButtonBase>();
    if (!button || button.ClickMode() != ClickMode::Release) return nullptr;
    bool captured = false;
    if (const auto pointers = button.PointerCaptures()) {
        for (const auto& current : pointers) {
            if (current.PointerId() != pointer.PointerId()) continue;
            captured = true;
            break;
        }
    }
    if (!captured && !button.CapturePointer(pointer)) return nullptr;

    // A drag starts after PointerPressed, so ClickMode::Press suppresses the
    // pending release click while the native button still sees PointerReleased.
    const auto original = button.ReadLocalValue(ButtonBase::ClickModeProperty());
    button.ClickMode(ClickMode::Press);
    return original;
} catch (const winrt::hresult_error&) {
    return nullptr;
}

void RestoreStartClick(const xaml::FrameworkElement& element, const IInspectable& original) noexcept try {
    const auto button = element.try_as<ButtonBase>();
    if (!button) return;
    const auto weak = winrt::make_weak(button);
    // Capture loss can be raised inside the native release handler. Keep the
    // click mode suppressed until that input event has finished dispatching.
    button.Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weak, original] {
        try {
            if (const auto current = weak.get()) Restore(current, original);
        } catch (const winrt::hresult_error&) {
            // Explorer may have replaced the button.
        }
    });
} catch (const winrt::hresult_error&) {
    // There is no dispatcher to restore once the island has closed.
}

} // namespace fullwintb::appearance

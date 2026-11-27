#pragma once

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <utility>

namespace fullwintb::appearance {

class XamlTranslation {
public:
    bool Move(const winrt::Windows::UI::Xaml::UIElement& target, double x, double y,
              double angle = 0, double center = 0) noexcept try {
        namespace xaml = winrt::Windows::UI::Xaml;
        const auto property = xaml::UIElement::RenderTransformProperty();
        if (!x && !y && !angle) {
            if (Owns(target)) {
                try {
                    if (originalLocal_ == xaml::DependencyProperty::UnsetValue()) target.ClearValue(property);
                    else target.SetValue(property, originalLocal_);
                } catch (const winrt::hresult_error&) {
                    target.RenderTransform(original_);
                }
            }
            applied_ = nullptr;
            shift_ = nullptr;
            tilt_ = nullptr;
            original_ = nullptr;
            originalLocal_ = nullptr;
            x_ = y_ = angle_ = 0;
            return true;
        }
        if (!target || (applied_ && !Owns(target))) return false;
        if (!applied_) {
            auto previous = target.RenderTransform();
            auto local = target.ReadLocalValue(property);
            xaml::Media::TransformGroup transforms;
            xaml::Media::TranslateTransform translation;
            if (previous) transforms.Children().Append(previous);
            transforms.Children().Append(translation);
            target.RenderTransform(transforms);
            originalLocal_ = std::move(local);
            original_ = std::move(previous);
            applied_ = std::move(transforms);
            shift_ = std::move(translation);
        }
        if (x != x_) {
            shift_.X(x);
            x_ = x;
        }
        if (y != y_) {
            shift_.Y(y);
            y_ = y;
        }
        if (angle != angle_) {
            if (!tilt_) {
                xaml::Media::SkewTransform skew;
                skew.CenterX(center);
                const auto parts = applied_.Children();
                parts.InsertAt(parts.Size() - 1, skew);
                tilt_ = std::move(skew);
            }
            tilt_.AngleY(angle);
            angle_ = angle;
        }
        return true;
    } catch (const winrt::hresult_error&) {
        return false;
    }

    bool Owns(const winrt::Windows::UI::Xaml::UIElement& target) const {
        return target && applied_ && target.RenderTransform() == applied_;
    }

    double Y() const { return y_; }

private:
    winrt::Windows::Foundation::IInspectable originalLocal_{nullptr};
    winrt::Windows::UI::Xaml::Media::Transform original_{nullptr};
    winrt::Windows::UI::Xaml::Media::TransformGroup applied_{nullptr};
    winrt::Windows::UI::Xaml::Media::TranslateTransform shift_{nullptr};
    winrt::Windows::UI::Xaml::Media::SkewTransform tilt_{nullptr};
    double x_ = 0;
    double y_ = 0;
    double angle_ = 0;
};

} // namespace fullwintb::appearance

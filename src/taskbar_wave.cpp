#include <windows.h>
#include <unknwn.h>
#undef GetCurrentTime
#include "taskbar_wave.h"

#include <string_view>
#include <winrt/Windows.UI.Xaml.Automation.h>

namespace fullwintb::appearance {
namespace {

namespace xaml = winrt::Windows::UI::Xaml;
using Element = xaml::FrameworkElement;
using xaml::Automation::AutomationProperties;

bool NativeButton(IconGroup group, const Element& element) {
    const auto name = winrt::get_class_name(element);
    const auto id = AutomationProperties::GetAutomationId(element);
    if (group == IconGroup::apps)
        return std::wstring_view(id).starts_with(L"Appid:") || name == L"Taskbar.TaskListButton" ||
               name == L"Taskbar.TaskListLabeledButton" || name == L"Taskbar.ExperienceToggleButton" ||
               name == L"Taskbar.SearchBoxButton" || name == L"Taskbar.TaskViewButton";
    return id == L"NotifyItemIcon" || id == L"SystemTrayIcon" ||
           (std::wstring_view(name).starts_with(L"SystemTray.") && std::wstring_view(name).find(L"Button") != std::wstring_view::npos);
}

struct Candidate {
    Element element;
    LONG left;
    LONG right;
    bool grip;
};

void Collect(const Element& parent, IconGroup group, const TaskbarLayout& layout, LONG offsetX,
             std::vector<Candidate>& result, int depth = 0) {
    if (!parent || depth == 32 || result.size() >= 256) return;
    const auto count = xaml::Media::VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < count; ++i) {
        const auto child = xaml::Media::VisualTreeHelper::GetChild(parent, i).try_as<Element>();
        if (!child || child.Visibility() != xaml::Visibility::Visible || !child.IsLoaded()) continue;
        const bool grip = IsTaskbarGrip(group, child);
        if (!grip && !NativeButton(group, child)) {
            Collect(child, group, layout, offsetX, result, depth + 1);
            continue;
        }
        const auto width = child.ActualWidth();
        const auto height = child.ActualHeight();
        if (width < 8 || height < 12) continue;
        const auto shape = child.TransformToVisual(nullptr).TransformBounds({0, 0, static_cast<float>(width), static_cast<float>(height)});
        const double scale = layout.dpi / 96.0;
        const LONG origin = layout.bounds.left - offsetX;
        const LONG left = origin + std::lround(shape.X * scale);
        const LONG right = origin + std::lround((shape.X + shape.Width) * scale);
        const LONG bandLeft = group == IconGroup::apps ? layout.buttons.front().left : layout.notification.left;
        const LONG bandRight = group == IconGroup::apps ? layout.buttons.back().right : layout.dock.right;
        if (right > bandLeft && left < bandRight && right > left)
            result.push_back({child, std::max(left, bandLeft), std::min(right, bandRight), grip});
    }
}

} // namespace

bool IsTaskbarGrip(IconGroup group, const Element& element) {
    if (group == IconGroup::apps)
        return element.Name() == L"StartButton" || AutomationProperties::GetAutomationId(element) == L"StartButton";
    return element.Name() == L"NotifyIconStack" || winrt::get_class_name(element) == L"SystemTray.ChevronIconView";
}

void IconWave::Anchor(IconGroup group, const Element& frame, const TaskbarLayout& layout,
                      IconOffset offset, MotionAxis axis, POINT cursor, MotionClock::time_point now) noexcept try {
    if (axis_ != axis) {
        Clear();
        axis_ = axis;
    }
    if (layout.buttons.empty() || !frame || anchorX_ == cursor.x) return;
    const LONG along = offset.Along(axis_);
    const bool fresh = icons_.empty();
    if (fresh) {
        std::vector<Candidate> items;
        Collect(frame, group, layout, offset.x, items);
        if (items.empty()) return;
        std::sort(items.begin(), items.end(), [](const Candidate& a, const Candidate& b) { return a.left < b.left; });
        const LONG padding = std::max(2, MulDiv(3, layout.dpi, 96));
        const LONG bandLeft = group == IconGroup::apps ? layout.buttons.front().left : layout.notification.left - 2;
        const LONG bandRight = group == IconGroup::apps ? layout.buttons.back().right : layout.dock.right;
        icons_.reserve(items.size());
        planned_.reserve(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            const LONG left = i ? std::max(item.left - padding, (items[i - 1].right + item.left) / 2) : item.left - padding;
            const LONG right = i + 1 < items.size() ? std::min(item.right + padding, (item.right + items[i + 1].left) / 2)
                                                   : item.right + padding;
            const LONG first = std::max(bandLeft, left);
            const LONG last = std::min(bandRight, right);
            if (first >= last || (!planned_.empty() && first < planned_.back().right)) continue;
            planned_.push_back({first, last, along});
            icons_.push_back({winrt::make_weak(item.element), {}, (item.left + item.right) / 2,
                              item.element.ActualWidth() / 2.0, item.grip});
        }
        if (icons_.empty()) {
            Clear();
            return;
        }
        applied_ = planned_;
        trail_.Start(now, along);
        lastFrame_ = now;
        dpiScale_ = layout.dpi / 96.0;
        BOOL animations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
        animateTilt_ = animations != FALSE;
    }
    const bool horizontal = axis_ == MotionAxis::horizontal;
    const auto distance = [&](std::size_t i) {
        return std::abs(cursor.x - icons_[i].center - (horizontal ? applied_[i].offset : offset.x));
    };
    nearest_ = 0;
    for (std::size_t i = 1; i < icons_.size(); ++i) {
        if (distance(i) < distance(nearest_)) nearest_ = i;
    }
    const auto firstDelay = IconDelay(distance(nearest_), layout.dpi);
    for (std::size_t i = 0; i < icons_.size(); ++i) {
        auto& icon = icons_[i];
        icon.wantedDelay = IconDelay(distance(i), layout.dpi) - firstDelay;
        if (fresh || !Pending()) icon.delay = icon.wantedDelay;
    }
    anchorX_ = cursor.x;
} catch (...) {
    Clear();
}

void IconWave::Plan(IconOffset baseline, IconOffset lower, IconOffset upper, MotionClock::time_point now) {
    if (icons_.empty()) return;
    const bool horizontal = axis_ == MotionAxis::horizontal;
    const LONG minimum = lower.Along(axis_);
    const LONG maximum = upper.Along(axis_);
    trail_.Record(now, baseline.Along(axis_));
    // Change the delay gradually if a new gesture starts before the previous
    // wave is home. This keeps the time sampled by each button moving forward.
    const auto change = (now - lastFrame_) * 3 / 4;
    const double elapsed = std::chrono::duration<double>(now - lastFrame_).count();
    lastFrame_ = now;
    longestDelay_ = {};
    tilting_ = false;
    LONG previous = minimum;
    LONG center = icons_.front().center;
    for (std::size_t i = 0; i < icons_.size(); ++i) {
        auto& icon = icons_[i];
        icon.delay += std::clamp(icon.wantedDelay - icon.delay, -change, change);
        longestDelay_ = std::max(longestDelay_, icon.delay);
        const LONG spacing = std::max(0L, (icon.center - center) / 5);
        const LONG left = horizontal ? std::max(minimum, previous - spacing) : minimum;
        const LONG offset = std::clamp(trail_.At(now - icon.delay), left, maximum);
        if (animateTilt_) {
            icon.tilt.Step(offset - planned_[i].offset, elapsed, dpiScale_);
            icon.angle = icon.tilt.Tenths();
            tilting_ |= icon.tilt.Active();
        }
        planned_[i].offset = offset;
        previous = offset;
        center = icon.center;
    }
}

bool IconWave::Apply(IconOffset baseline, UINT dpi, bool planned) {
    const auto& slices = planned ? planned_ : applied_;
    const bool vertical = axis_ == MotionAxis::vertical;
    for (std::size_t i = 0; i < icons_.size(); ++i) {
        auto& icon = icons_[i];
        const LONG pixels = slices[i].offset - baseline.Along(axis_);
        const int angle = planned ? icon.angle : icon.previousAngle;
        if (pixels == icon.appliedShift && angle == icon.writtenAngle) continue;
        const double shift = pixels * 96.0 / dpi;
        if (!icon.translation.Move(icon.element.get(), vertical ? 0 : shift, vertical ? shift : 0,
                                   angle / 10.0, icon.centerDip)) return false;
        icon.appliedShift = pixels;
        icon.writtenAngle = angle;
    }
    return true;
}

void IconWave::Commit() {
    std::copy(planned_.begin(), planned_.end(), applied_.begin());
    for (auto& icon : icons_) icon.previousAngle = icon.angle;
}

void IconWave::HoldFrame() {
    std::copy(applied_.begin(), applied_.end(), planned_.begin());
    for (auto& icon : icons_) icon.angle = icon.previousAngle;
}

void IconWave::Clear() noexcept {
    for (auto& icon : icons_) {
        try { icon.translation.Move(icon.element.get(), 0, 0); } catch (...) {}
    }
    icons_.clear();
    planned_.clear();
    applied_.clear();
    anchorX_.reset();
    longestDelay_ = {};
    tilting_ = false;
}

bool IconWave::PositionChanged() const {
    return !std::equal(planned_.begin(), planned_.end(), applied_.begin(),
                       [](const IconSlice& a, const IconSlice& b) { return a.offset == b.offset; });
}

bool IconWave::TiltChanged() const {
    return std::any_of(icons_.begin(), icons_.end(), [](const auto& icon) { return icon.angle != icon.previousAngle; });
}

LONG IconWave::LeadingX(IconOffset baseline) const {
    if (axis_ == MotionAxis::horizontal)
        for (const auto& part : planned_) baseline.x = std::min(baseline.x, part.offset);
    return baseline.x;
}

IconOffset IconWave::AnchorOffset(IconOffset baseline) const {
    if (!icons_.empty()) baseline.Along(axis_) = applied_[nearest_].offset;
    return baseline;
}

IconOffset IconWave::GripOffset(IconOffset baseline) const {
    for (std::size_t i = 0; i < icons_.size(); ++i) {
        if (!icons_[i].grip) continue;
        baseline.Along(axis_) = applied_[i].offset;
        break;
    }
    return baseline;
}

IconSliceRow IconWave::Slices(IconOffset baseline, bool planned) const {
    const auto& slices = planned ? planned_ : applied_;
    for (const auto& slice : slices) {
        if (slice.offset != baseline.Along(axis_)) return {axis_, slices};
    }
    return {};
}

} // namespace fullwintb::appearance

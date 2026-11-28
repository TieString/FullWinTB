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
    LONG first;
    LONG last;
    bool grip;
};

void Collect(const Element& parent, IconGroup group, const TaskbarLayout& layout, LONG offsetAlong,
             std::vector<Candidate>& result, int depth = 0) {
    if (!parent || depth == 32 || result.size() >= 256) return;
    const auto axis = layout.Axis();
    const auto count = xaml::Media::VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < count; ++i) {
        const auto child = xaml::Media::VisualTreeHelper::GetChild(parent, i).try_as<Element>();
        if (!child || child.Visibility() != xaml::Visibility::Visible || !child.IsLoaded()) continue;
        const bool grip = IsTaskbarGrip(group, child);
        if (!grip && !NativeButton(group, child)) {
            Collect(child, group, layout, offsetAlong, result, depth + 1);
            continue;
        }
        const auto width = child.ActualWidth();
        const auto height = child.ActualHeight();
        if (width < 8 || height < 12) continue;
        const auto shape = child.TransformToVisual(nullptr).TransformBounds({0, 0, static_cast<float>(width), static_cast<float>(height)});
        const double scale = layout.dpi / 96.0;
        const LONG origin = AxisStart(layout.bounds, axis) - offsetAlong;
        const bool horizontal = axis == MotionAxis::horizontal;
        const auto start = horizontal ? shape.X : shape.Y;
        const auto size = horizontal ? shape.Width : shape.Height;
        const LONG first = origin + std::lround(start * scale);
        const LONG last = origin + std::lround((start + size) * scale);
        const LONG bandStart = AxisStart(group == IconGroup::apps ? layout.buttons.front() : layout.notification, axis);
        const LONG bandEnd = AxisEnd(group == IconGroup::apps ? layout.buttons.back() : layout.notification, axis);
        if (last > bandStart && first < bandEnd && last > first)
            result.push_back({child, std::max(first, bandStart), std::min(last, bandEnd), grip});
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
    const auto dockAxis = layout.Axis();
    if (axis_ != axis || dockAxis_ != dockAxis) {
        Clear();
        axis_ = axis;
        dockAxis_ = dockAxis;
    }
    const LONG point = AxisPoint(cursor, dockAxis);
    if (layout.buttons.empty() || !frame || anchor_ == point) return;
    const LONG along = offset.Along(axis_);
    const bool fresh = icons_.empty();
    if (fresh) {
        std::vector<Candidate> items;
        Collect(frame, group, layout, offset.Along(dockAxis), items);
        if (items.empty()) return;
        std::sort(items.begin(), items.end(), [](const Candidate& a, const Candidate& b) { return a.first < b.first; });
        const LONG padding = std::max(2, MulDiv(3, layout.dpi, 96));
        const LONG bandStart = group == IconGroup::apps ? AxisStart(layout.buttons.front(), dockAxis)
                                                        : AxisStart(layout.notification, dockAxis) - 2;
        const LONG bandEnd = group == IconGroup::apps ? AxisEnd(layout.buttons.back(), dockAxis)
                                                      : AxisEnd(layout.notification, dockAxis) + 2;
        icons_.reserve(items.size());
        planned_.reserve(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            const LONG start = i ? std::max(item.first - padding, (items[i - 1].last + item.first) / 2)
                                 : item.first - padding;
            const LONG end = i + 1 < items.size() ? std::min(item.last + padding, (item.last + items[i + 1].first) / 2)
                                                 : item.last + padding;
            const LONG first = std::max(bandStart, start);
            const LONG last = std::min(bandEnd, end);
            if (first >= last || (!planned_.empty() && first < planned_.back().last)) continue;
            planned_.push_back({first, last, along});
            icons_.push_back({winrt::make_weak(item.element), {}, (item.first + item.last) / 2,
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
    const bool alongDock = axis_ == dockAxis;
    const auto distance = [&](std::size_t i) {
        return std::abs(point - icons_[i].center - (alongDock ? applied_[i].offset : offset.Along(dockAxis)));
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
    anchor_ = point;
} catch (...) {
    Clear();
}

void IconWave::Plan(IconOffset baseline, IconOffset lower, IconOffset upper, MotionClock::time_point now) {
    if (icons_.empty()) return;
    const bool alongDock = axis_ == dockAxis_;
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
        const LONG lowerBound = alongDock ? std::max(minimum, previous - spacing) : minimum;
        const LONG offset = std::clamp(trail_.At(now - icon.delay), lowerBound, maximum);
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
    anchor_.reset();
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

LONG IconWave::LeadingOffset(IconOffset baseline, MotionAxis axis) const {
    LONG result = baseline.Along(axis);
    if (axis_ == axis)
        for (const auto& part : planned_) result = std::min(result, part.offset);
    return result;
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

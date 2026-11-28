#include "taskbar_region.h"
#include "geometry.h"
#include "taskbar_layer.h"

#include <memory>

namespace fullwintb::appearance {
namespace {

using Region = std::unique_ptr<std::remove_pointer_t<HRGN>, decltype(&DeleteObject)>;

Region EmptyRegion() { return Region(CreateRectRgn(0, 0, 0, 0), DeleteObject); }

Region ReadRegion(HWND bar) {
    auto region = EmptyRegion();
    if (region && GetWindowRgn(bar, region.get()) == ERROR) region.reset();
    return region;
}

Region CopyRegion(HRGN source) {
    auto region = EmptyRegion();
    if (region && CombineRgn(region.get(), source, nullptr, RGN_COPY) == ERROR) region.reset();
    return region;
}

bool SameRegion(HRGN left, HRGN right) {
    return left == right || (left && right && EqualRgn(left, right));
}

struct Span {
    LONG first;
    LONG last;
};

struct Groups {
    Span apps;
    Span notification;
    LONG widgetsEnd;
    LONG gap;
};

Groups MeasureGroups(const TaskbarLayout& layout) {
    const RECT& bounds = layout.dock;
    const auto axis = layout.Axis();
    const UINT dpi = layout.dpi;
    const LONG first = AxisStart(bounds, axis);
    Span apps{AxisStart(layout.buttons.front(), axis), AxisEnd(layout.buttons.back(), axis)};
    const LONG widgetLength = AxisSize(bounds, OtherAxis(axis)) * 4;
    const LONG widgetsEnd = layout.widgets && axis == MotionAxis::horizontal && apps.first - first > widgetLength
        ? first + widgetLength : first;
    if (widgetsEnd > first) apps.first = first;
    const Span notification{std::max(first, AxisStart(layout.notification, axis) - 2),
                            std::min(AxisEnd(bounds, axis), AxisEnd(layout.notification, axis) + 2)};
    const LONG gap = std::clamp(notification.first - apps.last, 0L, static_cast<LONG>(MulDiv(8, dpi, 96)));
    return {apps, notification, widgetsEnd, gap};
}

RECT StripAt(const TaskbarLayout& layout, Span span, IconOffset offset) {
    const auto axis = layout.Axis();
    RECT strip = layout.dock;
    AxisStart(strip, axis) = span.first;
    AxisEnd(strip, axis) = span.last;
    OffsetRect(&strip, offset.x, offset.y);
    return strip;
}

void PublishAppsInkSpan(const TaskbarLayout& layout) {
    const auto axis = layout.Axis();
    const LONG origin = AxisStart(layout.dock, axis);
    const LONG first = AxisStart(layout.startButton, axis) - origin;
    const LONG last = AxisEnd(layout.buttons.back(), axis) - origin;
    if (first < 0 || last <= first || last > UINT16_MAX) {
        RemovePropW(layout.bar, appsInkSpanProperty);
        return;
    }
    const auto value = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(MAKELONG(first, last)));
    if (GetPropW(layout.bar, appsInkSpanProperty) != value)
        SetPropW(layout.bar, appsInkSpanProperty, value);
}

Region BuildRegion(const TaskbarLayout& layout, IconOffsets offsets, HRGN nativeClip, IconSlices slices = {}) {
    auto region = EmptyRegion();
    auto part = EmptyRegion();
    if (!region || !part) return Region(nullptr, DeleteObject);
    const Groups groups = MeasureGroups(layout);
    const auto add = [&](LONG first, LONG last, IconOffset offset) {
        if (first >= last) return true;
        auto rect = StripAt(layout, {first, last}, offset);
        if (!IntersectRect(&rect, &rect, &layout.bounds)) return false;
        OffsetRect(&rect, -layout.dock.left - offset.x, -layout.dock.top - offset.y);
        if (!SetRectRgn(part.get(), rect.left, rect.top, rect.right, rect.bottom)) return false;
        // Intersect in the original dock coordinates so the native mask moves
        // with each strip, including its clipped edges and holes.
        if (nativeClip && CombineRgn(part.get(), part.get(), nativeClip, RGN_AND) == ERROR) return false;
        if (OffsetRgn(part.get(), layout.dock.left - layout.bounds.left + offset.x,
                      layout.dock.top - layout.bounds.top + offset.y) == ERROR) return false;
        return CombineRgn(region.get(), region.get(), part.get(), RGN_OR) != ERROR;
    };
    const auto addSpan = [&](Span span, IconGroup group) {
        const auto fallback = offsets.For(group);
        const auto row = slices.For(group);
        LONG start = span.first;
        for (const auto& slice : row.slices) {
            if (slice.first >= span.last) break;
            const LONG first = std::max(start, slice.first);
            const LONG last = std::min(span.last, slice.last);
            if (first >= last) continue;
            auto position = fallback;
            position.Along(row.axis) = slice.offset;
            if (!add(start, first, fallback) || !add(first, last, position)) return false;
            start = last;
        }
        return add(start, span.last, fallback);
    };
    if (!addSpan(groups.notification, IconGroup::notification)) return Region(nullptr, DeleteObject);
    const auto axis = layout.Axis();
    for (const auto& button : layout.buttons) {
        if (!addSpan({AxisStart(button, axis), AxisEnd(button, axis)}, IconGroup::apps))
            return Region(nullptr, DeleteObject);
    }
    if (!add(AxisStart(layout.dock, axis), groups.widgetsEnd, offsets.apps)) return Region(nullptr, DeleteObject);
    return region;
}

void RecordRegionWrite(HWND bar) {
    constexpr wchar_t property[] = L"FullWinTB.RegionWrites";
    const auto count = reinterpret_cast<ULONG_PTR>(GetPropW(bar, property));
    SetPropW(bar, property, reinterpret_cast<HANDLE>(count + 1));
}

bool ApplyRegion(HWND bar, Region& region, HRGN& applied, bool animated = false) {
    if (SameRegion(region.get(), applied)) return true;
    auto saved = CopyRegion(region.get());
    if (!saved || !WriteTaskbarRegion(bar, region.get(), animated)) return false;
    region.release();
    if (applied) DeleteObject(applied);
    applied = saved.release();
    RecordRegionWrite(bar);
    return true;
}

} // namespace

IconOffset IconTravel::Clamp(IconGroup group, IconOffset position) const {
    const auto first = minimum.For(group);
    const auto last = maximum.For(group);
    return {std::clamp(position.x, first.x, last.x), std::clamp(position.y, first.y, last.y)};
}

bool IconTravel::ShareRow(IconOffsets offsets) const {
    const auto across = OtherAxis(along);
    return std::abs(offsets.apps.Along(across) - offsets.notification.Along(across)) < thickness;
}

IconOffsets IconTravel::Project(IconOffsets offsets) const {
    offsets.apps = Clamp(IconGroup::apps, offsets.apps);
    offsets.notification = Clamp(IconGroup::notification, offsets.notification);
    if (ShareRow(offsets)) {
        auto& tray = offsets.notification.Along(along);
        auto& apps = offsets.apps.Along(along);
        tray = std::max(tray, minimum.apps.Along(along) - clearance);
        apps = std::min(apps, tray + clearance);
    }
    return offsets;
}

IconTravel TaskbarRegion::Travel(const TaskbarLayout& layout) {
    if (layout.buttons.empty()) return {};
    const Groups groups = MeasureGroups(layout);
    const auto along = layout.Axis();
    const auto across = OtherAxis(along);
    const LONG minimumAcross = std::min(0L, AxisStart(layout.bounds, across) - AxisStart(layout.dock, across));
    const LONG maximumAcross = std::max(0L, AxisEnd(layout.bounds, across) - AxisEnd(layout.dock, across));
    IconTravel travel;
    travel.along = along;
    travel.thickness = AxisSize(layout.dock, across);
    travel.clearance = std::max(0L, groups.notification.first - groups.apps.last - groups.gap);
    for (const auto group : {IconGroup::apps, IconGroup::notification}) {
        const auto span = group == IconGroup::apps ? groups.apps : groups.notification;
        auto& minimum = travel.minimum.For(group);
        auto& maximum = travel.maximum.For(group);
        minimum.Along(along) = std::min(0L, AxisStart(layout.dock, along) - span.first);
        maximum.Along(along) = std::max(0L, AxisEnd(layout.dock, along) - span.last);
        minimum.Along(across) = minimumAcross;
        maximum.Along(across) = maximumAcross;
    }
    return travel;
}

bool TaskbarRegion::Fits(const TaskbarLayout& layout, IconOffsets offsets) {
    return !layout.buttons.empty() && Travel(layout).Project(offsets) == offsets;
}

RECT TaskbarRegion::Strip(const TaskbarLayout& layout, IconOffsets offsets, IconGroup group) {
    if (layout.buttons.empty()) return {};
    const auto groups = MeasureGroups(layout);
    const auto span = group == IconGroup::apps ? Span{AxisStart(layout.startButton, layout.Axis()), groups.apps.last}
                                              : groups.notification;
    return StripAt(layout, span, offsets.For(group));
}

RECT TaskbarRegion::ReturnArea(const TaskbarLayout& layout, IconOffsets offsets, IconGroup group) {
    const auto current = Strip(layout, offsets, group);
    const auto original = Strip(layout, {}, group);
    RECT area{};
    UnionRect(&area, &original, &current);
    return area;
}

bool TaskbarRegion::Place(const TaskbarLayout& layout, IconOffsets offsets, bool animated, IconSlices slices) {
    if (layout.bar != bar_ || !SameRect(layout.bounds, bounds_) || !Fits(layout, offsets) || !OwnsCurrent())
        return false;
    auto region = BuildRegion(layout, offsets, previous_, slices);
    if (!region) return false;
    if (animated) {
        // XAML can still display the previous frame until its transform is presented.
        // Keep that frame's region for one update, then settle to the exact hit area.
        auto visible = CopyRegion(region.get());
        const HRGN previousFrame = motionRegion_ ? motionRegion_ : applied_;
        if (!visible || CombineRgn(visible.get(), visible.get(), previousFrame, RGN_OR) == ERROR ||
            !ApplyRegion(bar_, visible, applied_, true)) return false;
    } else if (!ApplyRegion(bar_, region, applied_)) {
        return false;
    }
    ClearPending();
    motionRegion_ = animated && !SameRegion(region.get(), applied_) ? region.release() : nullptr;
    return true;
}

bool TaskbarRegion::Rebase(const TaskbarLayout& layout, IconOffsets offsets, IconSlices slices) {
    RECT actual{};
    if (layout.bar != bar_ || !GetWindowRect(bar_, &actual) || !SameRect(layout.bounds, actual) ||
        !Fits(layout, offsets) || !OwnsCurrent()) return false;
    auto region = BuildRegion(layout, offsets, previous_, slices);
    if (!region || !ApplyRegion(bar_, region, applied_, true)) return false;
    bounds_ = layout.bounds;
    PublishAppsInkSpan(layout);
    ClearPending();
    return true;
}

bool TaskbarRegion::FinishMotion() {
    if (!motionRegion_) return true;
    if (!OwnsCurrent()) return false;
    Region exact(motionRegion_, DeleteObject);
    motionRegion_ = nullptr;
    if (ApplyRegion(bar_, exact, applied_, true)) return true;
    motionRegion_ = exact.release();
    return false;
}

bool TaskbarRegion::OwnsCurrent() const {
    return bar_ && applied_ && SameRegion(ReadRegion(bar_).get(), applied_);
}

bool TaskbarRegion::Update(const TaskbarLayout& layout, IconOffsets offsets, bool immediate) {
    const HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT bounds{};
    if (!bar || !GetWindowRect(bar, &bounds) ||
        IsRectEmpty(&layout.dock) ||
        FindWindowW(L"Shell_SecondaryTrayWnd", nullptr)) {
        Reset();
        return false;
    }
    if ((bar_ && bar_ != bar) || (bar_ && !SameRect(bounds_, bounds))) Reset();
    if (layout.bar != bar || !SameRect(layout.bounds, bounds) || !Fits(layout, offsets) ||
        GetWindowThreadProcessId(bar, nullptr) != GetCurrentThreadId()) return false;
    auto existing = ReadRegion(bar);
    if (!bar_ || !SameRegion(existing.get(), applied_ ? applied_ : previous_)) {
        if (!SameRect(layout.bounds, layout.dock)) return false;
        Reset();
        if (bar_) return false;
        bar_ = bar;
        bounds_ = bounds;
        previous_ = existing.release();
    }
    auto region = BuildRegion(layout, offsets, previous_);
    if (!region) return false;
    PublishAppsInkSpan(layout);
    if (SameRegion(region.get(), applied_)) {
        if (pending_) DeleteObject(pending_);
        pending_ = nullptr;
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    if (!SameRegion(region.get(), pending_)) {
        auto candidate = CopyRegion(region.get());
        if (!candidate) return false;
        if (pending_) DeleteObject(pending_);
        pending_ = candidate.release();
        pendingSince_ = now;
    }
    const bool settling = !immediate && now - pendingSince_ < 280;
    if (settling) {
        if (!applied_) return true;
        // Include both positions until Explorer has finished sliding its own icons.
        if (CombineRgn(region.get(), region.get(), applied_, RGN_OR) == ERROR) return false;
    }
    return ApplyRegion(bar, region, applied_) && settling;
}

void TaskbarRegion::Reset() {
    if (bar_ && applied_ && IsWindow(bar_)) {
        const auto current = ReadRegion(bar_);
        if (SameRegion(current.get(), applied_)) {
            if (!WriteTaskbarRegion(bar_, previous_)) return;
            previous_ = nullptr;
            RecordRegionWrite(bar_);
        }
    }
    if (previous_) DeleteObject(previous_);
    if (applied_) DeleteObject(applied_);
    if (bar_) RemovePropW(bar_, appsInkSpanProperty);
    ClearPending();
    bar_ = nullptr;
    previous_ = nullptr;
    applied_ = nullptr;
}

void TaskbarRegion::ClearPending() {
    if (pending_) DeleteObject(pending_);
    if (motionRegion_) DeleteObject(motionRegion_);
    pending_ = nullptr;
    motionRegion_ = nullptr;
    pendingSince_ = 0;
}

} // namespace fullwintb::appearance

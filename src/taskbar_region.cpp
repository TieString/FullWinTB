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
    LONG left;
    LONG right;
};

struct Groups {
    Span apps;
    Span notification;
    LONG widgetsRight;
    LONG gap;
};

Groups MeasureGroups(const TaskbarLayout& layout) {
    const RECT& bounds = layout.dock;
    const UINT dpi = layout.dpi;
    Span apps{layout.buttons.front().left, layout.buttons.back().right};
    const LONG widgetWidth = (bounds.bottom - bounds.top) * 4;
    const LONG widgetsRight = layout.widgets && apps.left - bounds.left > widgetWidth
        ? bounds.left + widgetWidth : bounds.left;
    if (widgetsRight > bounds.left) apps.left = bounds.left;
    const Span notification{std::max(bounds.left, layout.notification.left - 2),
                            std::min(bounds.right, layout.notification.right + 2)};
    const LONG gap = std::clamp(notification.left - apps.right, 0L, static_cast<LONG>(MulDiv(8, dpi, 96)));
    return {apps, notification, widgetsRight, gap};
}

RECT StripAt(const TaskbarLayout& layout, Span span, IconOffset offset) {
    RECT strip{span.left, layout.dock.top, span.right, layout.dock.bottom};
    OffsetRect(&strip, offset.x, offset.y);
    return strip;
}

Region BuildRegion(const TaskbarLayout& layout, IconOffsets offsets, HRGN nativeClip,
                   IconSlices slices = {}, bool strictHorizontalClip = false) {
    auto region = EmptyRegion();
    auto part = EmptyRegion();
    if (!region || !part) return Region(nullptr, DeleteObject);
    const Groups groups = MeasureGroups(layout);
    const auto add = [&](LONG left, LONG right, IconOffset offset) {
        if (left >= right) return true;
        auto rect = StripAt(layout, {left, right}, {offset.x, 0});
        if (!IntersectRect(&rect, &rect, &layout.dock)) return false;
        OffsetRect(&rect, -layout.dock.left, -layout.dock.top);
        if (!SetRectRgn(part.get(), rect.left, rect.top, rect.right, rect.bottom)) return false;
        // The native clip is expressed in dock coordinates, even for a tray
        // with one row lifted into the larger viewport and one at the dock.
        if (nativeClip) {
            const int kind = CombineRgn(part.get(), part.get(), nativeClip, RGN_AND);
            if (kind == ERROR) return false;
            if (strictHorizontalClip && offset.x && !offset.y) {
                RECT clipped{};
                if (kind != SIMPLEREGION || !GetRgnBox(part.get(), &clipped) || !SameRect(rect, clipped)) return false;
            }
        }
        if (OffsetRgn(part.get(), layout.dock.left - layout.bounds.left,
                      layout.dock.top - layout.bounds.top + offset.y) == ERROR) return false;
        return CombineRgn(region.get(), region.get(), part.get(), RGN_OR) != ERROR;
    };
    const auto addSpan = [&](Span span, IconGroup group) {
        const auto fallback = offsets.For(group);
        const auto row = slices.For(group);
        LONG left = span.left;
        for (const auto& slice : row.slices) {
            if (slice.left >= span.right) break;
            const LONG first = std::max(left, slice.left);
            const LONG last = std::min(span.right, slice.right);
            if (first >= last) continue;
            auto position = fallback;
            position.Along(row.axis) = slice.offset;
            if (!add(left, first, fallback) || !add(first, last, position)) return false;
            left = last;
        }
        return add(left, span.right, fallback);
    };
    if (!addSpan(groups.notification, IconGroup::notification)) return Region(nullptr, DeleteObject);
    for (const auto& button : layout.buttons) {
        if (!addSpan({button.left, button.right}, IconGroup::apps)) return Region(nullptr, DeleteObject);
    }
    if (!add(layout.dock.left, groups.widgetsRight, offsets.apps)) return Region(nullptr, DeleteObject);
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
    return std::abs(offsets.apps.y - offsets.notification.y) < height;
}

IconOffsets IconTravel::Project(IconOffsets offsets) const {
    offsets.apps = Clamp(IconGroup::apps, offsets.apps);
    offsets.notification = Clamp(IconGroup::notification, offsets.notification);
    if (ShareRow(offsets)) {
        offsets.notification.x = std::max(offsets.notification.x, minimum.apps.x - clearance);
        offsets.apps.x = std::min(offsets.apps.x, offsets.notification.x + clearance);
    }
    return offsets;
}

IconTravel TaskbarRegion::Travel(const TaskbarLayout& layout) {
    if (layout.buttons.empty()) return {};
    const Groups groups = MeasureGroups(layout);
    const LONG minimumY = std::min(0L, layout.bounds.top - layout.dock.top);
    const LONG maximumY = std::max(0L, layout.bounds.bottom - layout.dock.bottom);
    const LONG minimumApps = std::min(0L, layout.dock.left - groups.apps.left);
    const LONG minimumNotification = std::min(0L, layout.dock.left - groups.notification.left);
    const LONG maximumApps = std::max(0L, layout.dock.right - groups.apps.right);
    const LONG maximumNotification = std::max(0L, layout.dock.right - groups.notification.right);
    const LONG clearance = std::max(0L, groups.notification.left - groups.apps.right - groups.gap);
    return {{{minimumApps, minimumY}, {minimumNotification, minimumY}},
            {{maximumApps, maximumY}, {maximumNotification, maximumY}},
            clearance, layout.dock.bottom - layout.dock.top};
}

bool TaskbarRegion::Fits(const TaskbarLayout& layout, IconOffsets offsets) {
    return !layout.buttons.empty() && Travel(layout).Project(offsets) == offsets;
}

RECT TaskbarRegion::Strip(const TaskbarLayout& layout, IconOffsets offsets, IconGroup group) {
    if (layout.buttons.empty()) return {};
    const auto groups = MeasureGroups(layout);
    const auto span = group == IconGroup::apps ? Span{layout.startButton.left, groups.apps.right} : groups.notification;
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
    auto region = BuildRegion(layout, offsets, previous_, slices, true);
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
        layout.dock.right - layout.dock.left <= layout.dock.bottom - layout.dock.top ||
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

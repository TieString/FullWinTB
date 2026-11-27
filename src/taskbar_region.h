#pragma once

#include "taskbar_layout.h"
#include <span>

namespace fullwintb::appearance {

struct IconSlice {
    LONG left;
    LONG right;
    LONG offset;
};

struct IconSliceRow {
    MotionAxis axis = MotionAxis::horizontal;
    std::span<const IconSlice> slices;
};

struct IconSlices {
    IconSliceRow apps;
    IconSliceRow notification;
    IconSliceRow For(IconGroup group) const {
        return group == IconGroup::apps ? apps : notification;
    }
};

struct IconTravel {
    IconOffsets minimum;
    IconOffsets maximum;
    LONG clearance = 0;
    LONG height = 0;

    IconOffset Clamp(IconGroup group, IconOffset position) const;
    bool ShareRow(IconOffsets offsets) const;
    IconOffsets Project(IconOffsets offsets) const;
};

// Preserve any region Explorer or another application had already supplied.
class TaskbarRegion {
public:
    // Returns true while a narrower region still needs a stability check.
    bool Update(const TaskbarLayout& layout, IconOffsets offsets, bool immediate = false);
    bool Place(const TaskbarLayout& layout, IconOffsets offsets, bool animated, IconSlices slices = {});
    bool Rebase(const TaskbarLayout& layout, IconOffsets offsets, IconSlices slices = {});
    bool FinishMotion();
    bool MotionPending() const { return motionRegion_ != nullptr; }
    bool Installed() const { return applied_ != nullptr; }
    bool OwnsCurrent() const;
    static bool Fits(const TaskbarLayout& layout, IconOffsets offsets);
    static IconTravel Travel(const TaskbarLayout& layout);
    static RECT Strip(const TaskbarLayout& layout, IconOffsets offsets, IconGroup group);
    static RECT ReturnArea(const TaskbarLayout& layout, IconOffsets offsets, IconGroup group);
    void Reset();

private:
    void ClearPending();

    HWND bar_ = nullptr;
    RECT bounds_{};
    HRGN previous_ = nullptr; // In the original dock coordinates, including during vertical motion.
    HRGN applied_ = nullptr;
    HRGN pending_ = nullptr;
    HRGN motionRegion_ = nullptr;
    ULONGLONG pendingSince_ = 0;
};

} // namespace fullwintb::appearance

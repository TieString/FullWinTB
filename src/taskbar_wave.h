#pragma once

#include "icon_trail.h"
#include "taskbar_motion.h"
#include "taskbar_region.h"
#include "xaml_translation.h"
#include <optional>

namespace fullwintb::appearance {

bool IsTaskbarGrip(IconGroup group, const winrt::Windows::UI::Xaml::FrameworkElement& element);

class IconWave {
public:
    void Anchor(IconGroup group, const winrt::Windows::UI::Xaml::FrameworkElement& frame,
                const TaskbarLayout& layout, IconOffset offset, MotionAxis axis, POINT cursor,
                MotionClock::time_point now) noexcept;
    void Plan(IconOffset baseline, IconOffset minimum, IconOffset maximum, MotionClock::time_point now);
    bool Apply(IconOffset baseline, UINT dpi, bool planned);
    void Commit();
    void HoldFrame();
    void Clear() noexcept;

    bool Pending() const { return tilting_ || (!icons_.empty() && trail_.Pending(lastFrame_, longestDelay_)); }
    bool PositionChanged() const;
    bool TiltChanged() const;
    MotionAxis Axis() const { return axis_; }
    LONG LeadingX(IconOffset baseline) const;
    IconOffset AnchorOffset(IconOffset baseline) const;
    IconOffset GripOffset(IconOffset baseline) const;
    IconSliceRow Slices(IconOffset baseline, bool planned) const;

private:
    struct Icon {
        winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> element;
        XamlTranslation translation;
        LONG center;
        double centerDip;
        bool grip;
        LONG appliedShift = 0;
        IconTilt tilt;
        int angle = 0;
        int previousAngle = 0;
        int writtenAngle = 0;
        MotionClock::duration delay{};
        MotionClock::duration wantedDelay{};
    };

    std::vector<Icon> icons_;
    std::vector<IconSlice> planned_;
    std::vector<IconSlice> applied_;
    IconTrail trail_;
    MotionAxis axis_ = MotionAxis::horizontal;
    MotionClock::time_point lastFrame_{};
    MotionClock::duration longestDelay_{};
    std::optional<LONG> anchorX_;
    std::size_t nearest_ = 0;
    double dpiScale_ = 1.0;
    bool animateTilt_ = false;
    bool tilting_ = false;
};

} // namespace fullwintb::appearance
